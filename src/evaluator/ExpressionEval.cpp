#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "Frame.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "proxy_class.hpp"

namespace {

// Helper: convert Value -> string
std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Create a match result object with proper structure
ObjectPtr createMatchResult(
    const std::vector<re2::StringPiece>& groups,
    size_t matchPos,
    const std::string& input,
    const std::vector<std::string>& groupNames,
    const std::map<std::string, int>& nameToIndex,
    const Token& token) {
    auto result = std::make_shared<ObjectValue>();

    // Add numeric indices: "0" (full match), "1", "2", etc. (capture groups)
    for (size_t i = 0; i < groups.size(); ++i) {
        std::string captured = std::string(groups[i].data(), groups[i].size());
        result->properties[std::to_string(i)] = PropertyDescriptor{
            Value{captured}, false, false, true, token};
    }

    // Add metadata
    result->properties["index"] = PropertyDescriptor{
        Value{static_cast<double>(matchPos)}, false, false, true, token};

    result->properties["input"] = PropertyDescriptor{
        Value{input}, false, false, true, token};

    result->properties["length"] = PropertyDescriptor{
        Value{static_cast<double>(groups[0].length())}, false, false, true, token};

    // Add named groups object (if any named groups exist)
    if (!groupNames.empty()) {
        auto groupsObj = std::make_shared<ObjectValue>();

        for (const auto& [name, idx] : nameToIndex) {
            if (idx >= 0 && static_cast<size_t>(idx) < groups.size()) {
                groupsObj->properties[name] = PropertyDescriptor{
                    Value{std::string(groups[idx].data(), groups[idx].size())},
                    false, false, true, token};
            }
        }

        result->properties["groups"] = PropertyDescriptor{
            Value{groupsObj}, false, false, true, token};
    } else {
        result->properties["groups"] = PropertyDescriptor{
            Value{std::monostate{}}, false, false, true, token};
    }

    return result;
}

std::string to_property_key(const Value& v, const Token& token) {
    // string first
    if (auto ps = std::get_if<std::string>(&v)) {
        return *ps;
    }

    // number -> canonical integer if whole, otherwise decimal string
    if (auto pd = std::get_if<double>(&v)) {
        double d = *pd;
        if (!std::isfinite(d)) {
            throw SwaziError(
                "TypeError",
                "Invalid number for property key — must be finite.",
                token.loc);
        }
        double floor_d = std::floor(d);
        if (d == floor_d) {
            // whole number — print as integer to match object property storage
            return std::to_string(static_cast<long long>(d));
        }
        return std::to_string(d);
    }

    // boolean
    if (auto pb = std::get_if<bool>(&v)) {
        return *pb ? "kweli" : "sikweli";
    }

    // null/undefined
    if (std::holds_alternative<std::monostate>(v)) {
        return "null";
    }

    throw SwaziError(
        "TypeError",
        "Cannot convert value to a property key — unsupported type.",
        token.loc);
}

}  // anonymous namespace

inline uint32_t to_uint32(double d) {
    // Handle NaN and infinity
    if (!std::isfinite(d)) return 0;

    // Get the integer part
    int64_t i64 = static_cast<int64_t>(d);

    // JavaScript ToUint32: take modulo 2^32
    uint32_t result = static_cast<uint32_t>(i64);
    return result;
}
inline int32_t to_int32(double d) {
    if (!std::isfinite(d)) return 0;
    int64_t i64 = static_cast<int64_t>(d);
    int32_t result = static_cast<int32_t>(i64);  // Cast to signed 32-bit integer
    return result;
}

Value Evaluator::evaluate_expression(ExpressionNode* expr, EnvPtr env) {
    if (!expr) return std::monostate{};

    auto extract_promise_from_value = [](const Value& v) -> PromisePtr {
        if (std::holds_alternative<PromisePtr>(v)) {
            return std::get<PromisePtr>(v);
        }
        if (std::holds_alternative<ObjectPtr>(v)) {
            ObjectPtr obj = std::get<ObjectPtr>(v);
            if (!obj) return nullptr;
            auto it = obj->properties.find("__promise__");
            if (it == obj->properties.end()) return nullptr;
            const PropertyDescriptor& pd = it->second;
            if (std::holds_alternative<PromisePtr>(pd.value)) {
                return std::get<PromisePtr>(pd.value);
            }
        }
        return nullptr;
    };

    if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) return Value{
        n->value};
    if (auto s = dynamic_cast<StringLiteralNode*>(expr)) return Value{
        s->value};

    if (auto nn = dynamic_cast<NullNode*>(expr)) {
        return Value(std::monostate{});
    }
    if (auto line = dynamic_cast<LineNode*>(expr)) {
        return Value((double)line->token.line());
    }
    if (auto NaN = dynamic_cast<NaNNode*>(expr)) {
        return Value(std::numeric_limits<double>::quiet_NaN());
    }
    if (auto inf = dynamic_cast<InfNode*>(expr)) {
        return Value(std::numeric_limits<double>::infinity());
    }

    if (auto dt = dynamic_cast<DateTimeLiteralNode*>(expr)) {
        auto dateTimeVal = std::make_shared<DateTimeValue>(dt);
        return Value{dateTimeVal};
    }

    if (auto rl = dynamic_cast<RegexLiteralNode*>(expr)) {
        auto regex = std::make_shared<RegexValue>(rl->pattern, rl->flags);
        return Value{regex};
    }

    // --- await expression (suspends current async frame) ---
    if (auto awaitNode = dynamic_cast<AwaitExpressionNode*>(expr)) {
        // Use stable await_id as the key (assigned by parser). Cloning AST will preserve await_id.
        auto frame = current_frame();

        if (!awaitNode->await_id) {
            // missing id indicates parser didn't assign ids — fail fast with explanatory error
            throw SwaziError(
                "InternalError",
                "Await expression missing await_id; parser must assign stable ids.",
                awaitNode->token.loc);
        }

        size_t aid = awaitNode->await_id;

        // Short-circuit: if resumption already happened, return result or rethrow stored exception
        if (frame) {
            auto it_res = frame->awaited_results.find(aid);
            if (it_res != frame->awaited_results.end()) {
                Value v = it_res->second;
                frame->awaited_results.erase(it_res);
                frame->awaited_promises.erase(aid);
                return v;
            }

            auto it_ex = frame->awaited_exceptions.find(aid);
            if (it_ex != frame->awaited_exceptions.end()) {
                std::exception_ptr ep = it_ex->second;
                frame->awaited_exceptions.erase(it_ex);
                frame->awaited_promises.erase(aid);
                std::rethrow_exception(ep);
            }
        }

        // Evaluate operand exactly once -> must capture resulting Promise
        Value operand = evaluate_expression(awaitNode->expression.get(), env);

        // Accept either a raw PromisePtr or an object that contains a "__promise__" slot.
        auto extract_promise_from_value_local = [](const Value& v) -> PromisePtr {
            if (std::holds_alternative<PromisePtr>(v))
                return std::get<PromisePtr>(v);
            if (std::holds_alternative<ObjectPtr>(v)) {
                ObjectPtr obj = std::get<ObjectPtr>(v);
                if (!obj) return nullptr;
                auto it = obj->properties.find("__promise__");
                if (it == obj->properties.end()) return nullptr;
                if (std::holds_alternative<PromisePtr>(it->second.value))
                    return std::get<PromisePtr>(it->second.value);
            }
            return nullptr;
        };
        auto make_eptr_from_value = [this](const Value& reason) -> std::exception_ptr {
            try {
                // Prefer to preserve string messages and Error-like objects.
                if (std::holds_alternative<std::string>(reason)) {
                    throw std::runtime_error(std::get<std::string>(reason));
                } else if (std::holds_alternative<ObjectPtr>(reason)) {
                    ObjectPtr o = std::get<ObjectPtr>(reason);
                    if (o) {
                        auto it = o->properties.find("message");
                        if (it != o->properties.end() && std::holds_alternative<std::string>(it->second.value)) {
                            throw std::runtime_error(std::get<std::string>(it->second.value));
                        }
                        // fallback: attempt to stringify object
                    }
                    throw std::runtime_error(this->to_string_value(reason));
                } else {
                    // fallback for numbers/bool/null/others
                    throw std::runtime_error(this->to_string_value(reason));
                }
            } catch (...) {
                return std::current_exception();
            }
        };

        PromisePtr p = extract_promise_from_value_local(operand);

        if (!p) {
            auto resolved = std::make_shared<PromiseValue>();
            resolved->state = PromiseValue::State::FULFILLED;
            resolved->result = operand;
            p = resolved;
        }

        bool inAsync = false;
        if (frame) {
            inAsync = frame->is_async || (frame->function && frame->function->is_async);
        }

        if (!inAsync) {
            throw SwaziError("RuntimeError", "await can only be used inside an async function.", awaitNode->token.loc);
        }

        // store promise reference by numeric id
        frame->awaited_promises[aid] = p;

        // If promise already settled -> stamp result/exception and schedule resume
        if (p->state == PromiseValue::State::FULFILLED) {
            frame->awaited_results[aid] = p->result;

            PromisePtr callPromise = frame->pending_promise;
            std::weak_ptr<CallFrame> wf = frame;

            if (scheduler()) {
                scheduler()->enqueue_microtask([this, wf, callPromise]() {
                    auto f = wf.lock();
                    if (!f) return;

                    // Transfer ownership out of suspended_frames_ into call_stack_.
                    // Remove from suspended_frames_ first (if present), then push onto call stack.
                    this->remove_suspended_frame(f);

                    // Ensure the frame is on the call_stack_ before resuming.
                    bool present = false;
                    for (auto& ff : this->call_stack_) {
                        if (ff == f) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) {
                        this->push_frame(f);
                    }

                    f->is_suspended = false;
                    try {
                        execute_frame_until_await_or_return(f, callPromise);
                    } catch (...) {
                        // swallow resume errors
                    }
                });

            } else {
                auto f = wf.lock();
                if (f) {
                    // non-scheduler mode: resume inline (transfer ownership)
                    this->remove_suspended_frame(f);

                    bool present = false;
                    for (auto& ff : this->call_stack_) {
                        if (ff == f) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) this->push_frame(f);

                    f->is_suspended = false;
                    try {
                        execute_frame_until_await_or_return(f, callPromise);
                    } catch (...) {}
                }
            }

            // Keep a shared owner while suspended so caller popping won't destroy the frame.
            frame->is_suspended = true;
            this->add_suspended_frame(frame);
            pop_frame();
            throw SuspendExecution();
        }

        if (p->state == PromiseValue::State::REJECTED) {
            // Stamp exception into frame->awaited_exceptions[aid]
            frame->awaited_exceptions[aid] = make_eptr_from_value(p->result);

            // Mark the original promise as handled because the awaiting frame is going to
            // take responsibility for the rejection when it resumes (this prevents the
            // global unhandled-rejection check from reporting the original promise).
            this->mark_promise_and_ancestors_handled(p);

            PromisePtr callPromise = frame->pending_promise;
            std::weak_ptr<CallFrame> wf = frame;

            if (scheduler()) {
                scheduler()->enqueue_microtask([this, wf, callPromise]() {
                    auto f = wf.lock();
                    if (!f) return;

                    this->remove_suspended_frame(f);

                    bool present = false;
                    for (auto& ff : this->call_stack_) {
                        if (ff == f) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) {
                        this->push_frame(f);
                    }

                    f->is_suspended = false;
                    try {
                        execute_frame_until_await_or_return(f, callPromise);
                    } catch (...) {}
                });
            } else {
                auto f = wf.lock();
                if (f) {
                    this->remove_suspended_frame(f);

                    bool present = false;
                    for (auto& ff : this->call_stack_) {
                        if (ff == f) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) this->push_frame(f);

                    f->is_suspended = false;
                    try {
                        execute_frame_until_await_or_return(f, callPromise);
                    } catch (...) {}
                }
            }

            frame->is_suspended = true;
            this->add_suspended_frame(frame);
            pop_frame();
            throw SuspendExecution();
        }

        // Pending -> register then/catch callbacks that stamp resolution into frame->awaited_*[aid]
        {
            PromisePtr pcopy = p;
            size_t captured_aid = aid;

            // Mark the awaited promise as handled now that we're attaching handlers
            // which will deliver the resolution/rejection into the awaiting frame.
            if (pcopy) this->mark_promise_and_ancestors_handled(pcopy);

            pcopy->then_callbacks.push_back([this, wf = std::weak_ptr<CallFrame>(frame), captured_aid](Value res) {
                auto f_locked = wf.lock();
                if (!f_locked) return;

                f_locked->awaited_results[captured_aid] = res;
                PromisePtr callPromise = f_locked->pending_promise;

                // Schedule microtask to resume: the microtask will transfer ownership back and resume.
                if (scheduler()) {
                    scheduler()->enqueue_microtask([this, wf, callPromise]() {
                        auto f = wf.lock();
                        if (!f) return;

                        this->remove_suspended_frame(f);

                        bool present = false;
                        for (auto& ff : this->call_stack_) {
                            if (ff == f) {
                                present = true;
                                break;
                            }
                        }
                        if (!present) this->push_frame(f);

                        f->is_suspended = false;
                        try {
                            execute_frame_until_await_or_return(f, callPromise);
                        } catch (...) {}
                    });
                } else {
                    auto f = wf.lock();
                    if (!f) return;

                    this->remove_suspended_frame(f);

                    bool present = false;
                    for (auto& ff : this->call_stack_) {
                        if (ff == f) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) this->push_frame(f);

                    f->is_suspended = false;
                    try {
                        execute_frame_until_await_or_return(f, callPromise);
                    } catch (...) {}
                }
            });

            pcopy->catch_callbacks.push_back([this, wf = std::weak_ptr<CallFrame>(frame), captured_aid](Value reason) {
                auto f_locked = wf.lock();
                if (!f_locked) return;

                // Stamp exception into f_locked->awaited_exceptions[captured_aid]
                std::exception_ptr eptr;
                try {
                    if (std::holds_alternative<std::string>(reason)) {
                        throw std::runtime_error(std::get<std::string>(reason));
                    } else if (std::holds_alternative<ObjectPtr>(reason)) {
                        ObjectPtr o = std::get<ObjectPtr>(reason);
                        if (o) {
                            auto it = o->properties.find("message");
                            if (it != o->properties.end() && std::holds_alternative<std::string>(it->second.value)) {
                                throw std::runtime_error(std::get<std::string>(it->second.value));
                            }
                        }
                        throw std::runtime_error(this->to_string_value(reason));
                    } else {
                        throw std::runtime_error(this->to_string_value(reason));
                    }
                } catch (...) {
                    eptr = std::current_exception();
                }
                f_locked->awaited_exceptions[captured_aid] = eptr;

                PromisePtr callPromise = f_locked->pending_promise;

                if (scheduler()) {
                    scheduler()->enqueue_microtask([this, wf, callPromise]() {
                        auto f = wf.lock();
                        if (!f) return;

                        this->remove_suspended_frame(f);

                        bool present = false;
                        for (auto& ff : this->call_stack_) {
                            if (ff == f) {
                                present = true;
                                break;
                            }
                        }
                        if (!present) this->push_frame(f);

                        f->is_suspended = false;
                        try {
                            execute_frame_until_await_or_return(f, callPromise);
                        } catch (...) {}
                    });
                } else {
                    auto f = wf.lock();
                    if (!f) return;

                    this->remove_suspended_frame(f);

                    bool present = false;
                    for (auto& ff : this->call_stack_) {
                        if (ff == f) {
                            present = true;
                            break;
                        }
                    }
                    if (!present) this->push_frame(f);

                    f->is_suspended = false;
                    try {
                        execute_frame_until_await_or_return(f, callPromise);
                    } catch (...) {}
                }
            });

            // Suspend current frame: store it in suspended_frames_ and pop it from the active call stack.
            frame->is_suspended = true;
            this->add_suspended_frame(frame);
            pop_frame();
            throw SuspendExecution();
        }
    }

    if (auto y = dynamic_cast<YieldExpressionNode*>(expr)) {
        CallFramePtr cf = current_frame();
        if (!cf) {
            throw SwaziError("SyntaxError", "'yield' used outside of a generator function.", y->token.loc);
        }
        if (!(cf->function && cf->function->is_generator)) {
            throw SwaziError("SyntaxError", "'yield' used outside of a generator function.", y->token.loc);
        }

        // If resuming from this exact yield point
        if (cf->paused_yield == y) {
            cf->paused_yield = nullptr;

            // IMPORTANT: Restore the environment that was active when we paused
            // This is critical for nested blocks (if/for/while/etc)
            if (cf->paused_env) {
                env = cf->paused_env;  // Use the saved environment
                cf->paused_env = nullptr;
            }

            if (cf->generator_requested_return) {
                Value rval = cf->generator_return_value;
                cf->generator_requested_return = false;
                cf->generator_return_value = std::monostate{};
                throw GeneratorReturn(rval);
            }

            Value sent = std::monostate{};
            if (cf->generator_has_sent_value) {
                sent = cf->generator_sent_value;
                cf->generator_has_sent_value = false;
                cf->generator_sent_value = std::monostate{};
            }
            return sent;
        }

        // First-time hit: evaluate operand
        Value val = std::monostate{};
        if (y->expression) {
            val = evaluate_expression(y->expression.get(), env);
        }

        // CRITICAL FIX: Save the current environment before yielding
        // This preserves nested block scopes (if/for/while/etc)
        cf->paused_env = env;
        cf->paused_yield = y;

        throw GeneratorYield(val);
    }
    // Template literal evaluation: concatenate quasis and evaluated expressions.
    if (auto tpl = dynamic_cast<TemplateLiteralNode*>(expr)) {
        // quasis.size() is expected to be expressions.size() + 1, but tolerate mismatches.
        std::string out;
        size_t exprCount = tpl->expressions.size();
        size_t quasiCount = tpl->quasis.size();

        // Iterate through quasis and interleave expression values.
        for (size_t i = 0; i < quasiCount; ++i) {
            out += tpl->quasis[i];
            if (i < exprCount) {
                Value ev = evaluate_expression(tpl->expressions[i].get(), env);
                out += to_string_value(ev);
            }
        }

        return Value{
            out};
    }

    if (auto arrNode = dynamic_cast<ArrayExpressionNode*>(expr)) {
        auto arrVal = std::make_shared<ArrayValue>();
        arrVal->elements.reserve(arrNode->elements.size());

        for (auto& elemPtr : arrNode->elements) {
            if (!elemPtr) {
                // preserve undefined / empty slot behavior
                arrVal->elements.push_back(std::monostate{});
                continue;
            }

            // Support spread element nodes (parser should produce a SpreadElementNode)
            if (auto spread = dynamic_cast<SpreadElementNode*>(elemPtr.get())) {
                if (!spread->argument) {
                    throw SwaziError(
                        "SyntaxError",
                        "Spread element is missing an argument.",
                        spread->token.loc);
                }
                Value v = evaluate_expression(spread->argument.get(), env);

                // Common behavior: only arrays are spread into array literal
                if (std::holds_alternative<ArrayPtr>(v)) {
                    ArrayPtr src = std::get<ArrayPtr>(v);
                    if (src) {
                        for (auto& e : src->elements) {
                            arrVal->elements.push_back(e);
                        }
                    }
                } else {
                    // TODO: Extend to allow strings -> char elements, objects -> iterable values, etc.
                    throw SwaziError(
                        "TypeError",
                        "Spread operator in array expects an array value.",
                        spread->token.loc);
                }
                continue;
            }

            // normal (non-spread) element
            Value ev = evaluate_expression(elemPtr.get(), env);
            arrVal->elements.push_back(std::move(ev));
        }
        return Value{
            arrVal};
    }

    if (auto objNode = dynamic_cast<ObjectExpressionNode*>(expr)) {
        ObjectPtr obj = std::make_shared<ObjectValue>();

        // helper: convert FunctionExpressionNode -> FunctionDeclarationNode
        auto fnExprToDecl = [](FunctionExpressionNode* fe) -> std::shared_ptr<FunctionDeclarationNode> {
            auto declptr = std::make_shared<FunctionDeclarationNode>();
            declptr->token = fe->token;
            declptr->name = fe->name;
            // Preserve async flag from the FunctionExpressionNode
            declptr->is_async = fe->is_async;

            // clone parameter descriptors (ParameterNode unique_ptrs)
            declptr->parameters.reserve(fe->parameters.size());
            for (const auto& pp : fe->parameters) {
                if (pp)
                    declptr->parameters.push_back(pp->clone());
                else
                    declptr->parameters.push_back(nullptr);
            }

            declptr->body.reserve(fe->body.size());
            for (const auto& s : fe->body) declptr->body.push_back(s ? s->clone() : nullptr);
            return declptr;
        };

        for (const auto& p : objNode->properties) {
            if (!p) continue;

            Value val;

            // Handle Spread
            if (p->kind == PropertyKind::Spread) {
                if (!p->value) continue;

                // Unwrap SpreadElementNode if needed
                if (auto spreadNode = dynamic_cast<SpreadElementNode*>(p->value.get())) {
                    val = evaluate_expression(spreadNode->argument.get(), env);
                } else {
                    val = evaluate_expression(p->value.get(), env);
                }

                if (!std::holds_alternative<ObjectPtr>(val)) {
                    throw SwaziError(
                        "TypeError",
                        "Spread operator expects an object, but received a " + type_name(val) + " type",
                        p->token.loc);
                }

                ObjectPtr src = std::get<ObjectPtr>(val);
                if (!src) continue;

                for (const auto& kv : src->properties) {
                    obj->properties[kv.first] = kv.second;  // copy descriptor
                }
                continue;
            }

            // Determine property key
            std::string keyStr;
            if (p->computed) {
                if (!p->key) {
                    throw SwaziError(
                        "SyntaxError",
                        "Computed property is missing a key expression.",
                        p->token.loc);
                }
                keyStr = to_string_value(evaluate_expression(p->key.get(), env));
            } else if (!p->key_name.empty()) {
                keyStr = p->key_name;
            } else if (p->key) {
                keyStr = to_string_value(evaluate_expression(p->key.get(), env));
            } else {
                throw SwaziError(
                    "SyntaxError",
                    "Object property declared without a key.",
                    p->token.loc);
            }

            // Build property value
            if (p->kind == PropertyKind::Method) {
                if (auto fe = dynamic_cast<FunctionExpressionNode*>(p->value.get())) {
                    auto declptr = fnExprToDecl(fe);
                    EnvPtr methodClosure = std::make_shared<Environment>(env);
                    Environment::Variable thisVar;
                    thisVar.value = obj;
                    thisVar.is_constant = true;
                    methodClosure->set("$", thisVar);

                    FunctionPtr fnptr = std::make_shared<FunctionValue>(declptr->name, declptr->parameters, declptr, methodClosure, declptr->token);
                    fnptr->name = keyStr;
                    val = fnptr;
                } else {
                    val = evaluate_expression(p->value.get(), env);
                }
            } else if (p->kind == PropertyKind::KeyValue) {
                val = p->value ? evaluate_expression(p->value.get(), env) : std::monostate{};
            } else {
                // Shorthand
                if (!p->key_name.empty()) {
                    if (env->has(p->key_name))
                        val = env->get(p->key_name).value;
                    else
                        val = std::monostate{};
                } else if (p->key) {
                    val = evaluate_expression(p->key.get(), env);
                } else
                    val = std::monostate{};
            }

            PropertyDescriptor desc;
            desc.value = val;
            desc.is_private = p->is_private;
            desc.is_readonly = p->is_readonly;
            desc.is_locked = p->is_locked;
            desc.token = p->token;

            obj->properties[keyStr] = std::move(desc);
        }

        return Value{
            obj};
    }

    if (auto b = dynamic_cast<BooleanLiteralNode*>(expr)) return Value{
        b->value};

    if (auto id = dynamic_cast<IdentifierNode*>(expr)) {
        if (!env) {
            throw SwaziError(
                "ReferenceError",
                "Cannot resolve identifier '" + id->name + "' — no environment found.",
                id->token.loc);
        }

        if (!env->has(id->name)) {
            throw SwaziError(
                "ReferenceError",
                "Undefined identifier '" + id->name + "'.",
                id->token.loc);
        }

        return env->get(id->name).value;
    }
    // handle the $ / this node
    if (auto self = dynamic_cast<ThisExpressionNode*>(expr)) {
        // defensive checks similar to IdentifierNode handling
        if (!env) {
            throw SwaziError(
                "ReferenceError",
                "Cannot resolve '$' — no environment found for 'this'.",
                self->token.loc);
        }
        if (env->has("$")) {
            return env->get("$").value;
        }
        throw SwaziError(
            "ReferenceError",
            "Undefined 'this/self' ('$') — must be called within a valid class instance or a regular plain object.",
            self->token.loc);
    }

    if (auto rn = dynamic_cast<RangeExpressionNode*>(expr)) {
        Value startVal = evaluate_expression(rn->start.get(), env);
        Value endVal = evaluate_expression(rn->end.get(), env);

        int start = static_cast<int>(to_number(startVal, rn->token));
        int end = static_cast<int>(to_number(endVal, rn->token));

        size_t step = 1;
        if (rn->step) {
            Value stepVal = evaluate_expression(rn->step.get(), env);
            double stepNum = to_number(stepVal, rn->token);
            if (stepNum == 0.0) {
                throw SwaziError(
                    "ValueError",
                    "Range step cannot be zero",
                    rn->token.loc);
            }
            step = static_cast<size_t>(std::abs(stepNum));
        }

        auto range = std::make_shared<RangeValue>(start, end, step, rn->inclusive);
        return Value{range};
    }

    // Member access: object.property (e.g., arr.idadi, arr.ongeza, str.herufi)
    if (auto mem = dynamic_cast<MemberExpressionNode*>(expr)) {
        Value objVal = evaluate_expression(mem->object.get(), env);

        if (mem->is_optional && is_nullish(objVal)) {
            return std::monostate{};
        }

        if (std::holds_alternative<ClassPtr>(objVal)) {
            ClassPtr cls = std::get<ClassPtr>(objVal);
            if (!cls) return std::monostate{};

            // Walk the class -> super chain to find the first matching static property.
            ClassPtr walkCls = cls;
            const PropertyDescriptor* foundDesc = nullptr;
            ObjectPtr holder = nullptr;  // the static_table that actually holds the property

            while (walkCls) {
                if (walkCls->static_table) {
                    auto it = walkCls->static_table->properties.find(mem->property);
                    if (it != walkCls->static_table->properties.end()) {
                        foundDesc = &it->second;
                        holder = walkCls->static_table;
                        break;
                    }
                }
                walkCls = walkCls->super;
            }

            if (!foundDesc) return std::monostate{};  // not found anywhere in chain

            const PropertyDescriptor& desc = *foundDesc;

            // private static -> allowed only when access is internal (use holder for check)
            if (desc.is_private && !is_private_access_allowed(holder, env)) {
                throw std::runtime_error(
                    "PermissionError at " + mem->token.loc.to_string() +
                    "\nCannot access private static property '" + mem->property + "' from outside the declaring class/object." +
                    "\n --> Traced at:\n" + mem->token.loc.get_line_trace());
            }

            // Getter semantics: call getter if readonly and stored function
            if (desc.is_readonly && std::holds_alternative<FunctionPtr>(desc.value)) {
                FunctionPtr getter = std::get<FunctionPtr>(desc.value);
                return call_function(getter, {}, env, desc.token);
            }

            // Normal return of the value (from the class where it was found)
            return desc.value;
        }

        // Promise methods: then, catch, finally
        {
            PromisePtr prom = extract_promise_from_value(objVal);
            if (prom) {
                PromisePtr prom_local = prom;
                const std::string& prop = mem->property;
                Token memTok = mem->token;

                // --- Promise methods: then, catch, finally ---
                if (prop == "then") {
                    auto native_impl = [this, prom_local, memTok](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error("TypeError at " + token.loc.to_string() + "\nPromise.then expects a function argument (onFulfilled[, onRejected]).");
                        }
                        FunctionPtr onFulfilled = std::get<FunctionPtr>(args[0]);
                        FunctionPtr onRejected = nullptr;
                        if (args.size() >= 2) {
                            if (std::holds_alternative<FunctionPtr>(args[1])) {
                                onRejected = std::get<FunctionPtr>(args[1]);
                            } else if (!std::holds_alternative<std::monostate>(args[1])) {
                                throw std::runtime_error("TypeError at " + token.loc.to_string() + "\nPromise.then second argument must be a function if provided.");
                            }
                        }

                        // Create a promise to return and link it to the original.
                        auto next = std::make_shared<PromiseValue>();
                        next->parent = prom_local;

                        // helpers to resolve/reject next
                        auto resolve_next = [this, next](Value v) {
                            if (!next) return;
                            next->state = PromiseValue::State::FULFILLED;
                            next->result = v;
                            auto callbacks = next->then_callbacks;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([callbacks, v]() mutable {
                                    for (auto& c : callbacks) {
                                        try {
                                            c(v);
                                        } catch (...) {}
                                    }
                                });
                            } else {
                                for (auto& c : callbacks) {
                                    try {
                                        c(v);
                                    } catch (...) {}
                                }
                            }
                        };
                        auto reject_next = [this, next](Value r) {
                            if (!next) return;
                            next->state = PromiseValue::State::REJECTED;
                            next->result = r;
                            auto callbacks = next->catch_callbacks;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([callbacks, r]() mutable {
                                    for (auto& c : callbacks) {
                                        try {
                                            c(r);
                                        } catch (...) {}
                                    }
                                });
                            } else {
                                for (auto& c : callbacks) {
                                    try {
                                        c(r);
                                    } catch (...) {}
                                }
                            }
                        };

                        // Runner that calls user callback (either onFulfilled or onRejected) and chains result into `next` promise
                        auto run_cb_and_chain = [this, onFulfilled, onRejected, resolve_next, reject_next, memTok, next](Value v, bool wasRejected) {
                            try {
                                FunctionPtr cb = wasRejected ? onRejected : onFulfilled;
                                // If user didn't provide onRejected and this is a rejection, propagate rejection
                                if (!cb) {
                                    if (wasRejected) {
                                        reject_next(v);
                                        return;
                                    } else {
                                        resolve_next(v);
                                        return;
                                    }
                                }
                                Value res = this->call_function(cb, {v}, cb->closure, memTok);

                                // If callback returned a Promise, chain it to `next`.
                                PromisePtr p2 = nullptr;
                                if (std::holds_alternative<PromisePtr>(res)) {
                                    p2 = std::get<PromisePtr>(res);
                                } else if (std::holds_alternative<ObjectPtr>(res)) {
                                    ObjectPtr obj = std::get<ObjectPtr>(res);
                                    if (obj) {
                                        auto it = obj->properties.find("__promise__");
                                        if (it != obj->properties.end() && std::holds_alternative<PromisePtr>(it->second.value)) {
                                            p2 = std::get<PromisePtr>(it->second.value);
                                        }
                                    }
                                }

                                if (p2) {
                                    if (p2->state == PromiseValue::State::FULFILLED) {
                                        resolve_next(p2->result);
                                    } else if (p2->state == PromiseValue::State::PENDING) {
                                        p2->then_callbacks.push_back([resolve_next](Value rv) { resolve_next(rv); });
                                        p2->catch_callbacks.push_back([reject_next](Value rr) { reject_next(rr); });
                                        p2->parent = next;
                                    } else {  // rejected
                                        reject_next(p2->result);
                                    }
                                } else {
                                    resolve_next(res);
                                }

                            } catch (const std::exception& e) {
                                reject_next(std::string(e.what()));
                            } catch (...) {
                                reject_next(std::string("unknown exception"));
                            }
                        };

                        // If user provided an explicit rejection handler (second arg), mark the promise chain handled.
                        if (onRejected) this->mark_promise_and_ancestors_handled(prom_local);

                        // attach to original promise
                        if (prom_local->state == PromiseValue::State::FULFILLED) {
                            Value v = prom_local->result;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([run_cb_and_chain, v]() { run_cb_and_chain(v, false); });
                            } else {
                                run_cb_and_chain(v, false);
                            }
                        } else if (prom_local->state == PromiseValue::State::PENDING) {
                            prom_local->then_callbacks.push_back([run_cb_and_chain](Value v) { run_cb_and_chain(v, false); });
                            prom_local->catch_callbacks.push_back([run_cb_and_chain](Value r) { run_cb_and_chain(r, true); });
                        } else {  // original rejected -> if onRejected present call it, else propagate to next
                            if (onRejected) {
                                if (this->scheduler()) {
                                    this->scheduler()->enqueue_microtask([run_cb_and_chain, r = prom_local->result]() { run_cb_and_chain(r, true); });
                                } else {
                                    run_cb_and_chain(prom_local->result, true);
                                }
                            } else {
                                // propagate rejection to next (no handler attached)
                                if (this->scheduler()) {
                                    this->scheduler()->enqueue_microtask([reject_next, r = prom_local->result]() { reject_next(r); });
                                } else {
                                    reject_next(prom_local->result);
                                }
                            }
                        }

                        return Value{next};
                    };
                    return Value{std::make_shared<FunctionValue>(std::string("native:promise.then"), native_impl, env, memTok)};
                }

                if (prop == "catch") {
                    auto native_impl = [this, prom_local, memTok](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error("TypeError at " + token.loc.to_string() + "\nPromise.catch expects a function argument.");
                        }
                        FunctionPtr cb = std::get<FunctionPtr>(args[0]);
                        auto next = std::make_shared<PromiseValue>();
                        next->parent = prom_local;

                        auto resolve_next = [this, next](Value v) {
                            if (!next) return;
                            next->state = PromiseValue::State::FULFILLED;
                            next->result = v;
                            auto callbacks = next->then_callbacks;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([callbacks, v]() mutable {
                                    for (auto& c : callbacks) {
                                        try {
                                            c(v);
                                        } catch (...) {}
                                    }
                                });
                            } else {
                                for (auto& c : callbacks) {
                                    try {
                                        c(v);
                                    } catch (...) {}
                                }
                            }
                        };
                        auto reject_next = [this, next](Value r) {
                            if (!next) return;
                            next->state = PromiseValue::State::REJECTED;
                            next->result = r;
                            auto callbacks = next->catch_callbacks;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([callbacks, r]() mutable {
                                    for (auto& c : callbacks) {
                                        try {
                                            c(r);
                                        } catch (...) {}
                                    }
                                });
                            } else {
                                for (auto& c : callbacks) {
                                    try {
                                        c(r);
                                    } catch (...) {}
                                }
                            }
                        };

                        auto run_on_reject = [this, cb, resolve_next, reject_next, memTok, next](Value r) {
                            try {
                                Value res = this->call_function(cb, {r}, cb->closure, memTok);

                                PromisePtr p2 = nullptr;
                                if (std::holds_alternative<PromisePtr>(res)) {
                                    p2 = std::get<PromisePtr>(res);
                                } else if (std::holds_alternative<ObjectPtr>(res)) {
                                    ObjectPtr obj = std::get<ObjectPtr>(res);
                                    if (obj) {
                                        auto it = obj->properties.find("__promise__");
                                        if (it != obj->properties.end() && std::holds_alternative<PromisePtr>(it->second.value)) {
                                            p2 = std::get<PromisePtr>(it->second.value);
                                        }
                                    }
                                }

                                if (p2) {
                                    if (p2->state == PromiseValue::State::FULFILLED) {
                                        resolve_next(p2->result);
                                    } else if (p2->state == PromiseValue::State::PENDING) {
                                        p2->then_callbacks.push_back([resolve_next](Value rv) { resolve_next(rv); });
                                        p2->catch_callbacks.push_back([reject_next](Value rr) { reject_next(rr); });
                                        p2->parent = next;
                                    } else {
                                        reject_next(p2->result);
                                    }
                                } else {
                                    resolve_next(res);
                                }

                            } catch (const std::exception& e) {
                                reject_next(std::string(e.what()));
                            } catch (...) {
                                reject_next(std::string("unknown"));
                            }
                        };

                        // catch consumes rejection — mark chain handled immediately
                        this->mark_promise_and_ancestors_handled(prom_local);

                        if (prom_local->state == PromiseValue::State::REJECTED) {
                            Value reason = prom_local->result;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([run_on_reject, reason]() { run_on_reject(reason); });
                            } else {
                                run_on_reject(reason);
                            }
                        } else if (prom_local->state == PromiseValue::State::PENDING) {
                            prom_local->catch_callbacks.push_back([run_on_reject](Value r) { run_on_reject(r); });
                            prom_local->then_callbacks.push_back([resolve_next](Value v) { resolve_next(v); });
                        } else {  // already fulfilled -> forward
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([resolve_next, v = prom_local->result]() { resolve_next(v); });
                            } else {
                                resolve_next(prom_local->result);
                            }
                        }

                        return Value{next};
                    };
                    return Value{std::make_shared<FunctionValue>(std::string("native:promise.catch"), native_impl, env, memTok)};
                }

                if (prop == "finally") {
                    auto native_impl = [this, prom_local, memTok](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error("TypeError at " + token.loc.to_string() + "\nPromise.finally expects a function argument.");
                        }
                        FunctionPtr cb = std::get<FunctionPtr>(args[0]);
                        auto next = std::make_shared<PromiseValue>();
                        next->parent = prom_local;

                        auto resolve_next = [this, next](Value v) {
                            if (!next) return;
                            next->state = PromiseValue::State::FULFILLED;
                            next->result = v;
                            auto callbacks = next->then_callbacks;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([callbacks, v]() mutable {
                                    for (auto& c : callbacks) {
                                        try {
                                            c(v);
                                        } catch (...) {}
                                    }
                                });
                            } else {
                                for (auto& c : callbacks) {
                                    try {
                                        c(v);
                                    } catch (...) {}
                                }
                            }
                        };
                        auto reject_next = [this, next](Value r) {
                            if (!next) return;
                            next->state = PromiseValue::State::REJECTED;
                            next->result = r;
                            auto callbacks = next->catch_callbacks;
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([callbacks, r]() mutable {
                                    for (auto& c : callbacks) {
                                        try {
                                            c(r);
                                        } catch (...) {}
                                    }
                                });
                            } else {
                                for (auto& c : callbacks) {
                                    try {
                                        c(r);
                                    } catch (...) {}
                                }
                            }
                        };

                        auto run_finally = [this, cb, resolve_next, reject_next, memTok](Value outcome, bool wasRejected) {
                            try {
                                // call cb with no args; ignore its return for now (JS semantics are more subtle)
                                this->call_function(cb, {}, cb->closure, memTok);
                            } catch (...) {
                                // swallow finally errors for now and prefer to propagate original outcome
                            }
                            if (wasRejected)
                                reject_next(outcome);
                            else
                                resolve_next(outcome);
                        };

                        // do NOT mark the original promise handled — finally does not consume rejection reason.

                        if (prom_local->state == PromiseValue::State::PENDING) {
                            prom_local->then_callbacks.push_back([run_finally](Value v) { run_finally(v, false); });
                            prom_local->catch_callbacks.push_back([run_finally](Value r) { run_finally(r, true); });
                        } else if (prom_local->state == PromiseValue::State::FULFILLED) {
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([run_finally, v = prom_local->result]() { run_finally(v, false); });
                            } else {
                                run_finally(prom_local->result, false);
                            }
                        } else {
                            if (this->scheduler()) {
                                this->scheduler()->enqueue_microtask([run_finally, r = prom_local->result]() { run_finally(r, true); });
                            } else {
                                run_finally(prom_local->result, true);
                            }
                        }

                        return Value{next};
                    };
                    return Value{std::make_shared<FunctionValue>(std::string("native:promise.finally"), native_impl, env, memTok)};
                }
            }
        }

        // DateTime methods and properties
        {
            if (std::holds_alternative<DateTimePtr>(objVal)) {
                DateTimePtr dt = std::get<DateTimePtr>(objVal);
                const std::string& prop = mem->property;

                // Properties (direct access)
                if (prop == "year") return Value{static_cast<double>(dt->year)};
                if (prop == "month") return Value{static_cast<double>(dt->month)};
                if (prop == "day") return Value{static_cast<double>(dt->day)};
                if (prop == "hour") return Value{static_cast<double>(dt->hour)};
                if (prop == "minute") return Value{static_cast<double>(dt->minute)};
                if (prop == "second") return Value{static_cast<double>(dt->second)};
                if (prop == "ms") {
                    return Value{static_cast<double>(dt->fractionalNanoseconds / 1000000)};
                }
                if (prop == "isUTC") return Value{dt->isUTC};
                if (prop == "zone") {
                    if (dt->isUTC) {
                        return Value{std::string("UTC")};
                    }
                    int offsetSec = dt->tzOffsetSeconds;
                    char sign = (offsetSec >= 0) ? '+' : '-';
                    offsetSec = std::abs(offsetSec);
                    int hrs = offsetSec / 3600;
                    int mins = (offsetSec % 3600) / 60;
                    std::ostringstream oss;
                    oss << sign << std::setfill('0') << std::setw(2) << hrs
                        << ":" << std::setw(2) << mins;
                    return Value{oss.str()};
                }
                if (prop == "epochMillis") {
                    return Value{static_cast<double>(dt->epochNanoseconds) / 1000000.0};
                }
                if (prop == "epochSeconds") {
                    return Value{static_cast<double>(dt->epochNanoseconds) / 1000000000.0};
                }

                // Methods (return FunctionPtr)
                auto make_dt_fn = [this, dt, env, mem](auto impl) -> Value {
                    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                        return impl(args, callEnv, token);
                    };
                    return Value{std::make_shared<FunctionValue>(
                        "native:datetime." + mem->property, native_impl, env, mem->token)};
                };

                if (prop == "toStr" || prop == "str") {
                    return make_dt_fn([dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            return Value{dt->literalText};
                        }
                        if (!std::holds_alternative<std::string>(args[0])) {
                            throw SwaziError("TypeError", "toStr() expects a format string", token.loc);
                        }
                        try {
                            return Value{dt->format(std::get<std::string>(args[0]))};
                        } catch (const std::exception& e) {
                            throw SwaziError("ValueError", std::string("Format error: ") + e.what(), token.loc);
                        }
                    });
                }

                if (prop == "strftime") {
                    return make_dt_fn([dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            throw SwaziError("TypeError", "strftime() requires a format string", token.loc);
                        }
                        if (!std::holds_alternative<std::string>(args[0])) {
                            throw SwaziError("TypeError", "strftime() expects a format string", token.loc);
                        }
                        try {
                            return Value{dt->format(std::get<std::string>(args[0]))};
                        } catch (const std::exception& e) {
                            throw SwaziError("ValueError", std::string("strftime error: ") + e.what(), token.loc);
                        }
                    });
                }

                if (prop == "toISO") {
                    return make_dt_fn([dt](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                        return Value{dt->literalText};
                    });
                }

                if (prop == "toUTC") {
                    return make_dt_fn([dt](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                        try {
                            return Value{dt->setZone("UTC")};
                        } catch (const std::exception& e) {
                            throw SwaziError("ValueError", std::string("UTC conversion error: ") + e.what(), token.loc);
                        }
                    });
                }

                // Arithmetic methods
                if (prop == "addDays") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addDays() requires a numeric argument", token.loc);
                        int days = static_cast<int>(to_number(args[0], token));
                        return Value{dt->addDays(days)};
                    });
                }

                if (prop == "addMonths") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addMonths() requires a numeric argument", token.loc);
                        int months = static_cast<int>(to_number(args[0], token));
                        return Value{dt->addMonths(months)};
                    });
                }

                if (prop == "addYears") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addYears() requires a numeric argument", token.loc);
                        int years = static_cast<int>(to_number(args[0], token));
                        return Value{dt->addYears(years)};
                    });
                }

                if (prop == "addHours") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addHours() requires a numeric argument", token.loc);
                        double hours = to_number(args[0], token);
                        return Value{dt->addHours(hours)};
                    });
                }

                if (prop == "addMinutes") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addMinutes() requires a numeric argument", token.loc);
                        double minutes = to_number(args[0], token);
                        return Value{dt->addMinutes(minutes)};
                    });
                }

                if (prop == "addSeconds") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addSeconds() requires a numeric argument", token.loc);
                        double seconds = to_number(args[0], token);
                        return Value{dt->addSeconds(seconds)};
                    });
                }

                if (prop == "addMillis") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "addMillis() requires a numeric argument", token.loc);
                        double millis = to_number(args[0], token);
                        return Value{dt->addMillis(millis)};
                    });
                }

                // Subtract methods
                if (prop == "subtractDays") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractDays() requires a numeric argument", token.loc);
                        int days = static_cast<int>(to_number(args[0], token));
                        return Value{dt->subtractDays(days)};
                    });
                }

                if (prop == "subtractMonths") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractMonths() requires a numeric argument", token.loc);
                        int months = static_cast<int>(to_number(args[0], token));
                        return Value{dt->subtractMonths(months)};
                    });
                }

                if (prop == "subtractYears") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractYears() requires a numeric argument", token.loc);
                        int years = static_cast<int>(to_number(args[0], token));
                        return Value{dt->subtractYears(years)};
                    });
                }

                if (prop == "subtractHours") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractHours() requires a numeric argument", token.loc);
                        double hours = to_number(args[0], token);
                        return Value{dt->subtractHours(hours)};
                    });
                }

                if (prop == "subtractMinutes") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractMinutes() requires a numeric argument", token.loc);
                        double minutes = to_number(args[0], token);
                        return Value{dt->subtractMinutes(minutes)};
                    });
                }

                if (prop == "subtractSeconds") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractSeconds() requires a numeric argument", token.loc);
                        double seconds = to_number(args[0], token);
                        return Value{dt->subtractSeconds(seconds)};
                    });
                }

                if (prop == "subtractMillis") {
                    return make_dt_fn([this, dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "subtractMillis() requires a numeric argument", token.loc);
                        double millis = to_number(args[0], token);
                        return Value{dt->subtractMillis(millis)};
                    });
                }

                if (prop == "setZone") {
                    return make_dt_fn([dt](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) throw SwaziError("TypeError", "setZone() requires a timezone string", token.loc);
                        if (!std::holds_alternative<std::string>(args[0])) {
                            throw SwaziError("TypeError", "setZone() expects a string (e.g., 'UTC', '+05:30')", token.loc);
                        }
                        try {
                            return Value{dt->setZone(std::get<std::string>(args[0]))};
                        } catch (const std::exception& e) {
                            throw SwaziError("ValueError", std::string("setZone error: ") + e.what(), token.loc);
                        }
                    });
                }

                if (prop == "getLocale" || prop == "toLocale") {
                    return make_dt_fn([this, dt](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
                        // Helper: compute local offset (seconds) for a given epoch seconds.
                        auto local_tz_offset_for_seconds = [](std::time_t tt) -> int32_t {
                            std::tm local_tm{};
#ifdef _WIN32
                            localtime_s(&local_tm, &tt);
#else
                            localtime_r(&tt, &local_tm);
#endif

                            // If the platform provides tm_gmtoff (most BSDs, macOS, modern glibc), use it directly.
#if defined(__APPLE__) || defined(__FreeBSD__) || (defined(__GLIBC__) && defined(_BSD_SOURCE)) || defined(__linux__)
// tm_gmtoff is seconds east of UTC (positive east). Use it if present.
#if defined(__USE_MISC) || defined(__USE_BSD) || defined(__GLIBC__)
                            // The field may be present as local_tm.tm_gmtoff on many Unix-like systems
                            // Use a cast to int32_t to match tzOffsetSeconds semantics
                            return static_cast<int32_t>(local_tm.tm_gmtoff);
#endif
#endif

                            // Portable fallback:
                            // Build a GMT broken-down time for the same epoch and compute the difference:
                            std::tm gmt_tm{};
#ifdef _WIN32
                            gmtime_s(&gmt_tm, &tt);
#else
                            gmtime_r(&tt, &gmt_tm);
#endif

                            // Use copies because mktime/timegm may modify the tm struct
                            std::tm local_copy = local_tm;
                            std::tm gmt_copy = gmt_tm;

                            // Convert local broken-down time to epoch assuming local interpretation
                            std::time_t local_epoch = mktime(&local_copy);

                            // Convert GMT broken-down time to epoch (interpreting gmt_copy as UTC)
#ifdef _WIN32
                            std::time_t utc_epoch = _mkgmtime(&gmt_copy);
#else
                            std::time_t utc_epoch = timegm(&gmt_copy);
#endif

                            // local_epoch - utc_epoch == seconds east of UTC (positive if local is ahead of UTC)
                            double diff = std::difftime(local_epoch, utc_epoch);
                            return static_cast<int32_t>(diff);
                        };
                        // Compute epoch seconds from dt->epochNanoseconds (int64)
                        int64_t nanos = static_cast<int64_t>(dt->epochNanoseconds);
                        std::time_t seconds = static_cast<std::time_t>(nanos / 1'000'000'000LL);

                        int32_t local_offset = 0;
                        try {
                            local_offset = local_tz_offset_for_seconds(seconds);
                        } catch (...) {
                            // on error, default to UTC (offset 0)
                            local_offset = 0;
                        }

                        // Create a copy of the DateTimeValue preserving the instant but with local tz applied
                        auto newDt = std::make_shared<DateTimeValue>(*dt);
                        newDt->tzOffsetSeconds = local_offset;
                        newDt->isUTC = (local_offset == 0);
                        newDt->recompute_calendar_fields();
                        newDt->update_literal_text();
                        return Value{newDt};
                    });
                }

                throw SwaziError("ReferenceError",
                    "Unknown property '" + prop + "' on datetime",
                    mem->token.loc);
            }
        }

        // Regex instance methods and properties (in ExpressionEval.cpp)
        {
            if (std::holds_alternative<RegexPtr>(objVal)) {
                RegexPtr regex = std::get<RegexPtr>(objVal);
                const std::string& prop = mem->property;

                // Helper to create methods
                auto make_fn = [this, regex, env, mem](auto impl) -> Value {
                    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                        return impl(args, callEnv, token);
                    };
                    return Value{std::make_shared<FunctionValue>(
                        "regex." + mem->property, native_impl, env, mem->token)};
                };

                // ==================== Properties ====================

                if (prop == "pattern" || prop == "source") return Value{regex->pattern};
                if (prop == "flags") return Value{regex->flags};
                if (prop == "global") return Value{regex->global};
                if (prop == "ignoreCase") return Value{regex->ignoreCase};
                if (prop == "multiline") return Value{regex->multiline};
                if (prop == "dotAll") return Value{regex->dotAll};
                if (prop == "unicode") return Value{regex->unicode};
                if (prop == "lastIndex") return Value{static_cast<double>(regex->lastIndex)};

                // ==================== Methods ====================

                // test(str) -> bool
                if (prop == "test") {
                    return make_fn([this, regex](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            throw SwaziError("TypeError", "regex.test() requires a string argument", token.loc);
                        }

                        std::string str = to_string_value(args[0], true);
                        re2::RE2& re = regex->getCompiled();

                        if (regex->global) {
                            // Stateful: search from lastIndex
                            if (regex->lastIndex >= str.size()) {
                                regex->lastIndex = 0;
                                return Value{false};
                            }

                            re2::StringPiece input(str);
                            input.remove_prefix(regex->lastIndex);

                            if (re2::RE2::PartialMatch(input, re)) {
                                // Find match position to update lastIndex
                                re2::StringPiece match;
                                if (re2::RE2::FindAndConsume(&input, re, &match)) {
                                    regex->lastIndex = str.size() - input.size();
                                }
                                return Value{true};
                            } else {
                                regex->lastIndex = 0;
                                return Value{false};
                            }
                        } else {
                            // Non-global: just test once from beginning
                            return Value{re2::RE2::PartialMatch(str, re)};
                        }
                    });
                }

                // exec(str) -> match object | null
                if (prop == "exec") {
                    return make_fn([this, regex](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            throw SwaziError("TypeError", "regex.exec() requires a string argument", token.loc);
                        }

                        std::string str = to_string_value(args[0], true);
                        re2::RE2& re = regex->getCompiled();

                        int numGroups = regex->getNumGroups();
                        std::vector<re2::StringPiece> groups(numGroups + 1);  // +1 for full match

                        re2::StringPiece input(str);
                        size_t searchStart = 0;

                        if (regex->global) {
                            // Stateful execution using lastIndex
                            if (regex->lastIndex >= str.size()) {
                                regex->lastIndex = 0;
                                return Value{std::monostate{}};
                            }
                            searchStart = regex->lastIndex;
                            input.remove_prefix(searchStart);
                        }

                        // Perform the match
                        if (!re.Match(input, 0, input.size(), re2::RE2::UNANCHORED,
                                groups.data(), groups.size())) {
                            if (regex->global) {
                                regex->lastIndex = 0;
                            }
                            return Value{std::monostate{}};
                        }

                        // Calculate actual position in original string
                        size_t matchPos = searchStart + (groups[0].data() - input.data());

                        if (regex->global) {
                            regex->lastIndex = matchPos + groups[0].length();
                        }

                        // Create match result
                        return Value{createMatchResult(
                            groups, matchPos, str,
                            regex->getGroupNames(),
                            regex->getNameToIndex(),
                            token)};
                    });
                }

                // match(str) -> array of all matches | null
                if (prop == "match") {
                    return make_fn([this, regex](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            throw SwaziError("TypeError", "regex.match() requires a string argument", token.loc);
                        }

                        std::string str = to_string_value(args[0], true);
                        re2::RE2& re = regex->getCompiled();

                        auto result = std::make_shared<ArrayValue>();

                        if (regex->global) {
                            // Find all matches
                            re2::StringPiece input(str);
                            re2::StringPiece match;

                            while (re2::RE2::FindAndConsume(&input, re, &match)) {
                                result->elements.push_back(Value{std::string(match.data(), match.size())});
                            }

                            return result->elements.empty() ? Value{std::monostate{}} : Value{result};
                        } else {
                            // Single match - return array with one element or null
                            int numGroups = regex->getNumGroups();
                            std::vector<re2::StringPiece> groups(numGroups + 1);

                            if (re.Match(str, 0, str.size(), re2::RE2::UNANCHORED,
                                    groups.data(), groups.size())) {
                                for (const auto& g : groups) {
                                    result->elements.push_back(Value{std::string(g.data(), g.size())});
                                }
                                return Value{result};
                            }

                            return Value{std::monostate{}};
                        }
                    });
                }

                // replace(str, replacement) -> string
                if (prop == "replace") {
                    return make_fn([this, regex](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.size() < 2) {
                            throw SwaziError("TypeError",
                                "regex.replace() requires 2 arguments: str and replacement",
                                token.loc);
                        }

                        std::string str = to_string_value(args[0], true);
                        std::string replacement = to_string_value(args[1], true);
                        re2::RE2& re = regex->getCompiled();

                        std::string result = str;

                        if (regex->global) {
                            // Replace all matches
                            re2::RE2::GlobalReplace(&result, re, replacement);
                        } else {
                            // Replace only first match
                            re2::RE2::Replace(&result, re, replacement);
                        }

                        return Value{result};
                    });
                }

                // split(str, limit?) -> array
                if (prop == "split") {
                    return make_fn([this, regex](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            throw SwaziError("TypeError", "regex.split() requires a string argument", token.loc);
                        }

                        std::string str = to_string_value(args[0], true);
                        int limit = args.size() >= 2 ? static_cast<int>(to_number(args[1], token)) : -1;

                        re2::RE2& re = regex->getCompiled();
                        auto result = std::make_shared<ArrayValue>();

                        re2::StringPiece input(str);
                        re2::StringPiece match;
                        size_t lastPos = 0;
                        int count = 0;

                        while ((limit < 0 || count < limit) &&
                            re2::RE2::FindAndConsume(&input, re, &match)) {
                            // Add the text before the match
                            size_t matchStart = match.data() - str.data();
                            result->elements.push_back(Value{str.substr(lastPos, matchStart - lastPos)});
                            lastPos = matchStart + match.length();
                            count++;
                        }

                        // Add remaining text
                        if (limit < 0 || count < limit) {
                            result->elements.push_back(Value{str.substr(lastPos)});
                        }

                        return Value{result};
                    });
                }

                // setLastIndex(index) -> regex (for chaining)
                if (prop == "setLastIndex") {
                    return make_fn([this, regex](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                        if (args.empty()) {
                            throw SwaziError("TypeError", "regex.setLastIndex() requires a numeric argument", token.loc);
                        }

                        if (!regex->global) {
                            throw SwaziError("RegexError",
                                "Cannot set lastIndex on non-global regex (missing 'g' flag)",
                                token.loc);
                        }

                        double raw = to_number(args[0], token);
                        size_t idx = (raw < 0) ? 0 : static_cast<size_t>(raw);
                        regex->lastIndex = idx;

                        return Value{regex};
                    });
                }

                throw SwaziError("ReferenceError",
                    "Unknown property '" + prop + "' on regex. Available: pattern, flags, global, ignoreCase, multiline, dotAll, unicode, lastIndex, test(), exec(), match(), replace(), split(), setLastIndex()",
                    mem->token.loc);
            }
        }

        // --- Universal properties ---
        const std::string& prop = mem->property;

        if (prop == "toStr" || prop == "str") {
            auto make_fn = [this, objVal, env, mem]() -> Value {
                // Create a native function that converts the value to string
                auto native_impl = [this, objVal](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    // Handle numbers with optional radix parameter
                    if (std::holds_alternative<double>(objVal)) {
                        double num = std::get<double>(objVal);

                        // Check if radix argument is provided
                        if (!args.empty()) {
                            int radix = static_cast<int>(to_number(args[0], token));

                            // Validate radix range (2-36, like JavaScript)
                            if (radix < 2 || radix > 36) {
                                throw std::runtime_error(
                                    "RangeError at " + token.loc.to_string() +
                                    "\nRadix must be between 2 and 36. Got: " + std::to_string(radix) +
                                    "\n --> Traced at:\n" + token.loc.get_line_trace());
                            }

                            // Only support radix conversion for integers
                            if (std::floor(num) != num) {
                                throw std::runtime_error(
                                    "TypeError at " + token.loc.to_string() +
                                    "\nRadix conversion only works with integers. Got: " + std::to_string(num) +
                                    "\n --> Traced at:\n" + token.loc.get_line_trace());
                            }

                            // Convert to integer and then to string with radix
                            long long intNum = static_cast<long long>(num);
                            bool negative = intNum < 0;
                            if (negative) intNum = -intNum;

                            std::string result;
                            const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";

                            if (intNum == 0) {
                                result = "0";
                            } else {
                                while (intNum > 0) {
                                    result = digits[intNum % radix] + result;
                                    intNum /= radix;
                                }
                            }

                            if (negative) result = "-" + result;
                            return Value{result};
                        }

                        // No radix provided, use default string conversion
                        return Value{to_string_value(objVal, true)};
                    }

                    // Handle other simple types (no radix support)
                    if (std::holds_alternative<std::monostate>(objVal) ||
                        std::holds_alternative<std::string>(objVal) ||
                        std::holds_alternative<bool>(objVal)) {
                        return Value{to_string_value(objVal, true)};
                    }

                    // If this is a Buffer, delegate to buffer-string conversion so both
                    // universal.toStr and buffer.toStr behave the same.
                    if (std::holds_alternative<BufferPtr>(objVal)) {
                        BufferPtr buf = std::get<BufferPtr>(objVal);
                        if (!buf) return Value{std::string()};

                        // Default to utf8 for string conversion (even if buffer stores "binary" internally)
                        std::string enc = "utf8";
                        if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
                            enc = std::get<std::string>(args[0]);
                        }
                        std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                        // utf8 / binary: interpret raw bytes as string (binary is treated same as utf8)
                        if (enc == "utf8" || enc == "utf-8" || enc == "binary") {
                            return Value{std::string(buf->data.begin(), buf->data.end())};
                        }

                        // latin1 / iso-8859-1: map bytes 1:1 into a std::string
                        if (enc == "latin1" || enc == "iso-8859-1" || enc == "latin-1") {
                            std::string out;
                            out.reserve(buf->data.size());
                            for (uint8_t b : buf->data) out.push_back(static_cast<char>(b));
                            return Value{out};
                        }

                        // hex: produce lowercase hex string
                        if (enc == "hex") {
                            static const char* hex_digits = "0123456789abcdef";
                            std::string out;
                            out.reserve(buf->data.size() * 2);
                            for (uint8_t b : buf->data) {
                                out.push_back(hex_digits[(b >> 4) & 0xF]);
                                out.push_back(hex_digits[b & 0xF]);
                            }
                            return Value{out};
                        }

                        // base64: simple encoder
                        if (enc == "base64") {
                            static const char* b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                            const uint8_t* data = buf->data.data();
                            size_t len = buf->data.size();
                            std::string out;
                            out.reserve(((len + 2) / 3) * 4);

                            size_t i = 0;
                            while (i + 2 < len) {
                                uint32_t triple = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
                                out.push_back(b64_table[(triple >> 18) & 0x3F]);
                                out.push_back(b64_table[(triple >> 12) & 0x3F]);
                                out.push_back(b64_table[(triple >> 6) & 0x3F]);
                                out.push_back(b64_table[triple & 0x3F]);
                                i += 3;
                            }

                            if (i < len) {
                                uint32_t triple = uint32_t(data[i]) << 16;
                                out.push_back(b64_table[(triple >> 18) & 0x3F]);
                                if (i + 1 < len) {
                                    triple |= uint32_t(data[i + 1]) << 8;
                                    out.push_back(b64_table[(triple >> 12) & 0x3F]);
                                    out.push_back(b64_table[(triple >> 6) & 0x3F]);
                                    out.push_back('=');
                                } else {
                                    out.push_back(b64_table[(triple >> 12) & 0x3F]);
                                    out.push_back('=');
                                    out.push_back('=');
                                }
                            }
                            return Value{out};
                        }

                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nUnsupported buffer encoding '" + enc + "'. Supported encodings: utf8, binary, hex, base64." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    // Handle complex types - return type representation
                    std::string type_repr;
                    if (std::holds_alternative<ArrayPtr>(objVal)) {
                        type_repr = "[orodha]";
                    } else if (std::holds_alternative<ObjectPtr>(objVal)) {
                        type_repr = "[object]";
                    } else if (std::holds_alternative<FunctionPtr>(objVal)) {
                        FunctionPtr fn = std::get<FunctionPtr>(objVal);
                        std::string name = fn->name.empty() ? "<lambda>" : fn->name;
                        type_repr = "[kazi " + name + "]";
                    } else if (std::holds_alternative<ClassPtr>(objVal)) {
                        ClassPtr cp = std::get<ClassPtr>(objVal);
                        std::string name = cp ? cp->name : "<null>";
                        type_repr = "[muundo " + name + "]";
                    } else {
                        type_repr = "[unknown]";
                    }

                    return Value{type_repr};
                };

                auto fn = std::make_shared<FunctionValue>(
                    std::string("native:universal.toStr"),
                    native_impl,
                    env,
                    mem->token);
                return Value{fn};
            };
            return make_fn();
        }

        // aina -> type name
        if (prop == "aina") {
            return type_name(objVal);
        }

        {
            // type-checking booleans
            if (prop == "ninamba" || prop == "NINAMBA") return Value{
                std::holds_alternative<double>(objVal)};
            if (prop == "nineno" || prop == "NINENO") return Value{
                std::holds_alternative<std::string>(objVal)};
            if (prop == "nibool" || prop == "NIBOOL") return Value{
                std::holds_alternative<bool>(objVal)};
            if (prop == "niorodha" || prop == "NIORODHA") return Value{
                std::holds_alternative<ArrayPtr>(objVal)};
            if (prop == "nikazi" || prop == "NIKAZI") return Value{
                std::holds_alternative<FunctionPtr>(objVal)};
            if (prop == "niobject") return Value{
                std::holds_alternative<ObjectPtr>(objVal)};
        }

        if (std::holds_alternative<BufferPtr>(objVal)) {
            BufferPtr buf = std::get<BufferPtr>(objVal);

            // buf.size -> number of bytes
            if (mem->property == "size") {
                return Value{static_cast<double>(buf ? buf->data.size() : 0)};
            }

            // buffer.toStr([encoding]) -> string
            if (mem->property == "toStr" || mem->property == "str") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) return Value{std::string()};

                    // Default to utf8 for string conversion (even if buffer stores "binary" internally)
                    std::string enc = "utf8";
                    if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
                        enc = std::get<std::string>(args[0]);
                    }

                    // lowercase the encoding for simple comparison
                    std::transform(enc.begin(), enc.end(), enc.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                    // utf8 / binary: interpret raw bytes as string (binary is treated same as utf8)
                    if (enc == "utf8" || enc == "utf-8" || enc == "binary") {
                        return Value{std::string(buf->data.begin(), buf->data.end())};
                    }

                    // latin1 / iso-8859-1: map bytes 1:1 into a std::string
                    if (enc == "latin1" || enc == "iso-8859-1" || enc == "latin-1") {
                        std::string out;
                        out.reserve(buf->data.size());
                        for (uint8_t b : buf->data) out.push_back(static_cast<char>(b));
                        return Value{out};
                    }

                    // hex: produce lowercase hex string
                    if (enc == "hex") {
                        static const char* hex_digits = "0123456789abcdef";
                        std::string out;
                        out.reserve(buf->data.size() * 2);
                        for (uint8_t b : buf->data) {
                            out.push_back(hex_digits[(b >> 4) & 0xF]);
                            out.push_back(hex_digits[b & 0xF]);
                        }
                        return Value{out};
                    }

                    // base64: simple encoder
                    if (enc == "base64") {
                        static const char* b64_table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                        const uint8_t* data = buf->data.data();
                        size_t len = buf->data.size();
                        std::string out;
                        out.reserve(((len + 2) / 3) * 4);

                        size_t i = 0;
                        while (i + 2 < len) {
                            uint32_t triple = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | uint32_t(data[i + 2]);
                            out.push_back(b64_table[(triple >> 18) & 0x3F]);
                            out.push_back(b64_table[(triple >> 12) & 0x3F]);
                            out.push_back(b64_table[(triple >> 6) & 0x3F]);
                            out.push_back(b64_table[triple & 0x3F]);
                            i += 3;
                        }

                        if (i < len) {
                            uint32_t triple = uint32_t(data[i]) << 16;
                            out.push_back(b64_table[(triple >> 18) & 0x3F]);
                            if (i + 1 < len) {
                                triple |= uint32_t(data[i + 1]) << 8;
                                out.push_back(b64_table[(triple >> 12) & 0x3F]);
                                out.push_back(b64_table[(triple >> 6) & 0x3F]);
                                out.push_back('=');
                            } else {
                                out.push_back(b64_table[(triple >> 12) & 0x3F]);
                                out.push_back('=');
                                out.push_back('=');
                            }
                        }
                        return Value{out};
                    }

                    // unknown encoding
                    throw std::runtime_error(
                        "TypeError at " + token.loc.to_string() +
                        "\nUnsupported buffer encoding '" + enc + "'. Supported encodings: utf8, binary, hex, base64." +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                };

                return Value{std::make_shared<FunctionValue>(std::string("native:buffer."), native_impl, env, mem->token)};
            }

            // buf.slice(start, end?) -> Buffer
            if (mem->property == "slice") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) return Value{std::make_shared<BufferValue>()};

                    size_t start = 0;
                    size_t end = buf->data.size();

                    if (!args.empty() && std::holds_alternative<double>(args[0])) {
                        double start_d = std::get<double>(args[0]);
                        start = static_cast<size_t>(std::max(0.0, start_d));
                    }

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        double end_d = std::get<double>(args[1]);
                        end = static_cast<size_t>(std::max(0.0, end_d));
                    }

                    start = std::min(start, buf->data.size());
                    end = std::min(end, buf->data.size());
                    if (start > end) start = end;

                    auto result = std::make_shared<BufferValue>();
                    result->data.assign(buf->data.begin() + start, buf->data.begin() + end);
                    result->encoding = buf->encoding;

                    return Value{result};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.slice"), native_impl, env, mem->token)};
            }

            // buf.byteAt(index) -> number (0-255)
            if (mem->property == "byteAt") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nCannot read from null buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    if (args.empty() || !std::holds_alternative<double>(args[0])) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.byteAt() requires index argument." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    size_t index = static_cast<size_t>(std::get<double>(args[0]));
                    if (index >= buf->data.size()) {
                        throw std::runtime_error(
                            "RangeError at " + token.loc.to_string() +
                            "\nIndex " + std::to_string(index) + " out of bounds (buffer size: " + std::to_string(buf->data.size()) + ")." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    return Value{static_cast<double>(buf->data[index])};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.byteAt"), native_impl, env, mem->token)};
            }

            // buf.toArray() -> array of numbers (0-255)
            if (mem->property == "toArray") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        auto empty = std::make_shared<ArrayValue>();
                        return Value{empty};
                    }

                    auto arr = std::make_shared<ArrayValue>();
                    arr->elements.reserve(buf->data.size());

                    for (uint8_t byte : buf->data) {
                        arr->elements.push_back(Value{static_cast<double>(byte)});
                    }

                    return Value{arr};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.toArray"), native_impl, env, mem->token)};
            }

            // buf.copy(sourceBuffer, targetStart?, sourceStart?, sourceEnd?) -> number (bytes copied)
            if (mem->property == "copy") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nCannot copy to null buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.copy() requires source buffer as first argument." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    BufferPtr source = std::get<BufferPtr>(args[0]);
                    if (!source) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nSource buffer is null." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    size_t targetStart = 0;
                    size_t sourceStart = 0;
                    size_t sourceEnd = source->data.size();

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        targetStart = static_cast<size_t>(std::max(0.0, std::get<double>(args[1])));
                    }
                    if (args.size() >= 3 && std::holds_alternative<double>(args[2])) {
                        sourceStart = static_cast<size_t>(std::max(0.0, std::get<double>(args[2])));
                    }
                    if (args.size() >= 4 && std::holds_alternative<double>(args[3])) {
                        sourceEnd = static_cast<size_t>(std::max(0.0, std::get<double>(args[3])));
                    }

                    // Clamp to valid ranges
                    sourceStart = std::min(sourceStart, source->data.size());
                    sourceEnd = std::min(sourceEnd, source->data.size());
                    if (sourceStart > sourceEnd) sourceStart = sourceEnd;

                    targetStart = std::min(targetStart, buf->data.size());

                    // Calculate how many bytes to copy
                    size_t sourceLength = sourceEnd - sourceStart;
                    size_t targetAvailable = buf->data.size() - targetStart;
                    size_t bytesToCopy = std::min(sourceLength, targetAvailable);

                    // Copy bytes
                    std::copy(
                        source->data.begin() + sourceStart,
                        source->data.begin() + sourceStart + bytesToCopy,
                        buf->data.begin() + targetStart);

                    return Value{static_cast<double>(bytesToCopy)};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.copy"), native_impl, env, mem->token)};
            }

            // buf.set(array, offset?) -> void
            if (mem->property == "set") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nCannot set on null buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    if (args.empty()) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.set() requires array or buffer argument." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    size_t offset = 0;
                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::max(0.0, std::get<double>(args[1])));
                    }

                    offset = std::min(offset, buf->data.size());

                    // From buffer
                    if (std::holds_alternative<BufferPtr>(args[0])) {
                        BufferPtr source = std::get<BufferPtr>(args[0]);
                        if (!source) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\nSource buffer is null." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        size_t available = buf->data.size() - offset;
                        size_t toCopy = std::min(source->data.size(), available);

                        std::copy(source->data.begin(), source->data.begin() + toCopy, buf->data.begin() + offset);
                    }
                    // From array
                    else if (std::holds_alternative<ArrayPtr>(args[0])) {
                        ArrayPtr arr = std::get<ArrayPtr>(args[0]);
                        if (!arr) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\nSource array is null." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        size_t available = buf->data.size() - offset;
                        size_t toCopy = std::min(arr->elements.size(), available);

                        for (size_t i = 0; i < toCopy; ++i) {
                            if (std::holds_alternative<double>(arr->elements[i])) {
                                double val = std::get<double>(arr->elements[i]);
                                buf->data[offset + i] = static_cast<uint8_t>(static_cast<int>(val) & 0xFF);
                            }
                        }
                    } else {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.set() requires array or buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    return std::monostate{};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.set"), native_impl, env, mem->token)};
            }

            // buf.equals(other) -> bool
            if (mem->property == "equals") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) return Value{false};

                    if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                        return Value{false};
                    }

                    BufferPtr other = std::get<BufferPtr>(args[0]);
                    if (!other) return Value{false};

                    if (buf->data.size() != other->data.size()) return Value{false};

                    return Value{buf->data == other->data};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.equals"), native_impl, env, mem->token)};
            }

            // buf.includes(value, byteOffset?) -> bool
            if (mem->property == "includes") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) return Value{false};

                    if (args.empty()) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.includes() requires a value to search for." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    size_t byteOffset = 0;
                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        byteOffset = static_cast<size_t>(std::max(0.0, std::get<double>(args[1])));
                    }

                    byteOffset = std::min(byteOffset, buf->data.size());

                    // Search for buffer
                    if (std::holds_alternative<BufferPtr>(args[0])) {
                        BufferPtr needle = std::get<BufferPtr>(args[0]);
                        if (!needle || needle->data.empty()) return Value{true};

                        if (needle->data.size() > buf->data.size() - byteOffset) return Value{false};

                        auto it = std::search(
                            buf->data.begin() + byteOffset,
                            buf->data.end(),
                            needle->data.begin(),
                            needle->data.end());

                        return Value{it != buf->data.end()};
                    }
                    // Search for string
                    else if (std::holds_alternative<std::string>(args[0])) {
                        std::string needle = std::get<std::string>(args[0]);
                        if (needle.empty()) return Value{true};

                        std::vector<uint8_t> needleBytes(needle.begin(), needle.end());

                        if (needleBytes.size() > buf->data.size() - byteOffset) return Value{false};

                        auto it = std::search(
                            buf->data.begin() + byteOffset,
                            buf->data.end(),
                            needleBytes.begin(),
                            needleBytes.end());

                        return Value{it != buf->data.end()};
                    }
                    // Search for single byte
                    else if (std::holds_alternative<double>(args[0])) {
                        uint8_t needle = static_cast<uint8_t>(static_cast<int>(std::get<double>(args[0])) & 0xFF);

                        auto it = std::find(buf->data.begin() + byteOffset, buf->data.end(), needle);
                        return Value{it != buf->data.end()};
                    }

                    return Value{false};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.includes"), native_impl, env, mem->token)};
            }

            // buf.append(value) -> void (mutates the buffer)
            if (mem->property == "append") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nCannot append to null buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    if (args.empty()) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.append() requires a value argument." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    // Append another buffer
                    if (std::holds_alternative<BufferPtr>(args[0])) {
                        BufferPtr other = std::get<BufferPtr>(args[0]);
                        if (other) {
                            buf->data.insert(buf->data.end(), other->data.begin(), other->data.end());
                        }
                    }
                    // Append a string
                    else if (std::holds_alternative<std::string>(args[0])) {
                        std::string str = std::get<std::string>(args[0]);
                        buf->data.insert(buf->data.end(), str.begin(), str.end());
                    }
                    // Append an array of bytes
                    else if (std::holds_alternative<ArrayPtr>(args[0])) {
                        ArrayPtr arr = std::get<ArrayPtr>(args[0]);
                        if (arr) {
                            for (const auto& elem : arr->elements) {
                                if (std::holds_alternative<double>(elem)) {
                                    double val = std::get<double>(elem);
                                    if (val < 0 || val > 255) {
                                        throw std::runtime_error(
                                            "RangeError at " + token.loc.to_string() +
                                            "\nByte values must be 0-255." +
                                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                                    }
                                    buf->data.push_back(static_cast<uint8_t>(val));
                                }
                            }
                        }
                    }
                    // Append a single byte
                    else if (std::holds_alternative<double>(args[0])) {
                        double val = std::get<double>(args[0]);
                        if (val < 0 || val > 255) {
                            throw std::runtime_error(
                                "RangeError at " + token.loc.to_string() +
                                "\nByte value must be 0-255." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        buf->data.push_back(static_cast<uint8_t>(val));
                    } else {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.append() requires buffer, string, array, or number." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    return std::monostate{};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.append"), native_impl, env, mem->token)};
            }

            // buf.write(value, offset?) -> void (writes and grows if needed)
            if (mem->property == "write") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nCannot write to null buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    if (args.empty()) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.write() requires a value argument." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    size_t offset = 0;
                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::max(0.0, std::get<double>(args[1])));
                    }

                    std::vector<uint8_t> bytes_to_write;

                    // Convert value to bytes
                    if (std::holds_alternative<BufferPtr>(args[0])) {
                        BufferPtr other = std::get<BufferPtr>(args[0]);
                        if (other) bytes_to_write = other->data;
                    } else if (std::holds_alternative<std::string>(args[0])) {
                        std::string str = std::get<std::string>(args[0]);
                        bytes_to_write.assign(str.begin(), str.end());
                    } else if (std::holds_alternative<ArrayPtr>(args[0])) {
                        ArrayPtr arr = std::get<ArrayPtr>(args[0]);
                        if (arr) {
                            for (const auto& elem : arr->elements) {
                                if (std::holds_alternative<double>(elem)) {
                                    double val = std::get<double>(elem);
                                    if (val < 0 || val > 255) {
                                        throw std::runtime_error(
                                            "RangeError at " + token.loc.to_string() +
                                            "\nByte values must be 0-255." +
                                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                                    }
                                    bytes_to_write.push_back(static_cast<uint8_t>(val));
                                } else
                                    throw SwaziError("TypeError", "Can only use numeric values as elements in buffer arrays", token.loc);
                            }
                        }
                    } else if (std::holds_alternative<double>(args[0])) {
                        double val = std::get<double>(args[0]);
                        if (val < 0 || val > 255) {
                            throw std::runtime_error(
                                "RangeError at " + token.loc.to_string() +
                                "\nByte value must be 0-255." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        bytes_to_write.push_back(static_cast<uint8_t>(val));
                    } else {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.write() requires buffer, string, array, or number." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    // Grow buffer if necessary
                    size_t needed_size = offset + bytes_to_write.size();
                    if (needed_size > buf->data.size()) {
                        buf->data.resize(needed_size, 0);
                    }

                    // Write bytes
                    std::copy(bytes_to_write.begin(), bytes_to_write.end(), buf->data.begin() + offset);

                    return std::monostate{};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.write"), native_impl, env, mem->token)};
            }

            // buf.resize(n) -> void (grows buffer by n bytes, fills with zeros)
            if (mem->property == "resize") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!buf) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nCannot resize null buffer." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    if (args.empty() || !std::holds_alternative<double>(args[0])) {
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nbuf.resize() requires a numeric size argument." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    double delta_d = std::get<double>(args[0]);

                    // Allow both positive and negative deltas
                    int delta = static_cast<int>(delta_d);

                    size_t current_size = buf->data.size();
                    int new_size_signed = static_cast<int>(current_size) + delta;

                    if (new_size_signed < 0) {
                        throw std::runtime_error(
                            "RangeError at " + token.loc.to_string() +
                            "\nCannot resize buffer to negative size (current: " +
                            std::to_string(current_size) + ", delta: " + std::to_string(delta) + ")." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    size_t new_size = static_cast<size_t>(new_size_signed);

                    if (new_size > 1e9) {
                        throw std::runtime_error(
                            "RangeError at " + token.loc.to_string() +
                            "\nBuffer size cannot exceed 1e9 bytes." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    }

                    buf->data.resize(new_size, 0);  // fills new space with zeros

                    return std::monostate{};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.resize"), native_impl, env, mem->token)};
            }

            //-------------------
            // other useful API, provided or already implement in the buffer module
            //-------------------
            const std::string& prop = mem->property;
            if (prop == "writeUInt8") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        throw SwaziError("TypeError", "buffer.writeUInt8(value, [position=0]) requires atleast one argument", token.loc);
                    }

                    if (!std::holds_alternative<double>(args[0])) {
                        throw SwaziError("TypeError", "Can not writeUInt8 with a non numeric value. The first argument should be a 1 byte numeric value 0 - 255", token.loc);
                    }

                    double value = std::get<double>(args[0]);
                    size_t offset = 0;

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::get<double>(args[1]));
                        if (offset >= buf->data.size())
                            throw SwaziError("RangeError", "offset is out of buffer size bound.", token.loc);
                    }
                    if (value < 0 || value > 255) {
                        throw SwaziError("RangeError", "Value must be 0-255", token.loc);
                    }

                    buf->data[offset] = static_cast<uint8_t>(value);
                    return Value{buf};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.writeUInt8"), native_impl, env, mem->token)};
            }

            if (prop == "writeUInt16LE") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        throw SwaziError("TypeError", "buffer.writeUInt8(value, [position=0]) requires atleast one argument", token.loc);
                    }

                    if (!std::holds_alternative<double>(args[0])) {
                        throw SwaziError("TypeError", "Can not writeUInt8 with a non numeric value. The first argument should be a 1 byte numeric value 0 - 255", token.loc);
                    }

                    uint16_t value = static_cast<uint16_t>(std::get<double>(args[0]));
                    size_t offset = 0;

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::get<double>(args[1]));
                        if (offset >= buf->data.size())
                            throw SwaziError("RangeError", "offset is out of buffer size bound.", token.loc);
                    }

                    if (offset + 2 > buf->data.size()) {
                        throw SwaziError("RangeError", "Not enough space for UInt16", token.loc);
                    }

                    buf->data[offset] = value & 0xFF;
                    buf->data[offset + 1] = (value >> 8) & 0xFF;
                    return Value{buf};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.writeUInt8"), native_impl, env, mem->token)};
            }
            if (prop == "writeUInt16BE") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        throw SwaziError("TypeError", "buffer.writeUInt8(value, [position=0]) requires atleast one argument", token.loc);
                    }

                    if (!std::holds_alternative<double>(args[0])) {
                        throw SwaziError("TypeError", "Can not writeUInt8 with a non numeric value. The first argument should be a 1 byte numeric value 0 - 255", token.loc);
                    }

                    uint16_t value = static_cast<uint16_t>(std::get<double>(args[0]));
                    size_t offset = 0;

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::get<double>(args[1]));
                        if (offset >= buf->data.size())
                            throw SwaziError("RangeError", "offset is out of buffer size bound.", token.loc);
                    }

                    if (offset + 2 > buf->data.size()) {
                        throw SwaziError("RangeError", "Not enough space for UInt16", token.loc);
                    }

                    buf->data[offset] = (value >> 8) & 0xFF;
                    buf->data[offset + 1] = value & 0xFF;
                    return Value{buf};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.writeUInt8"), native_impl, env, mem->token)};
            }

            if (prop == "writeUInt32LE") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        throw SwaziError("TypeError", "buffer.writeUInt8(value, [position=0]) requires atleast one argument", token.loc);
                    }

                    if (!std::holds_alternative<double>(args[0])) {
                        throw SwaziError("TypeError", "Can not writeUInt8 with a non numeric value. The first argument should be a 1 byte numeric value 0 - 255", token.loc);
                    }

                    uint32_t value = static_cast<uint32_t>(std::get<double>(args[0]));
                    size_t offset = 0;

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::get<double>(args[1]));
                        if (offset >= buf->data.size())
                            throw SwaziError("RangeError", "offset is out of buffer size bound.", token.loc);
                    }

                    if (offset + 4 > buf->data.size()) {
                        throw SwaziError("RangeError", "Not enough bytes for UInt32", token.loc);
                    }

                    buf->data[offset] = value & 0xFF;
                    buf->data[offset + 1] = (value >> 8) & 0xFF;
                    buf->data[offset + 2] = (value >> 16) & 0xFF;
                    buf->data[offset + 3] = (value >> 24) & 0xFF;
                    return Value{buf};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.writeUInt8"), native_impl, env, mem->token)};
            }
            if (prop == "writeUInt32BE") {
                auto native_impl = [this, buf](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        throw SwaziError("TypeError", "buffer.writeUInt8(value, [position=0]) requires atleast one argument", token.loc);
                    }

                    if (!std::holds_alternative<double>(args[0])) {
                        throw SwaziError("TypeError", "Can not writeUInt8 with a non numeric value. The first argument should be a 1 byte numeric value 0 - 255", token.loc);
                    }

                    uint32_t value = static_cast<uint32_t>(std::get<double>(args[0]));
                    size_t offset = 0;

                    if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                        offset = static_cast<size_t>(std::get<double>(args[1]));
                        if (offset >= buf->data.size())
                            throw SwaziError("RangeError", "offset is out of buffer size bound.", token.loc);
                    }

                    if (offset + 4 > buf->data.size()) {
                        throw SwaziError("RangeError", "Not enough space for UInt16", token.loc);
                    }

                    buf->data[offset] = (value >> 24) & 0xFF;
                    buf->data[offset + 1] = (value >> 16) & 0xFF;
                    buf->data[offset + 2] = (value >> 8) & 0xFF;
                    buf->data[offset + 3] = value & 0xFF;
                    return Value{buf};
                };
                return Value{std::make_shared<FunctionValue>(std::string("native:buffer.writeUInt8"), native_impl, env, mem->token)};
            }

            // No other properties recognized on buffers yet
            throw std::runtime_error(
                "ReferenceError at " + mem->token.loc.to_string() +
                "\nUnknown property '" + mem->property + "' on buffer." +
                "\n --> Traced at:\n" + mem->token.loc.get_line_trace());
        }

        // File methods
        if (std::holds_alternative<FilePtr>(objVal)) {
            FilePtr file = std::get<FilePtr>(objVal);
            const std::string& prop = mem->property;
            auto make_fn = [this, file, env, mem](auto impl) -> Value {
                auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    return impl(args, callEnv, token);
                };
                return Value{std::make_shared<FunctionValue>("file." + mem->property, native_impl, env, mem->token)};
            };

            // file.read(n?) -> string | Buffer
            if (prop == "read") {
                return make_fn([file](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    if (!file->is_open) {
                        throw SwaziError("IOError", "Cannot read from closed file", token.loc);
                    }

                    size_t n = SIZE_MAX;
                    if (!args.empty() && std::holds_alternative<double>(args[0])) {
                        n = static_cast<size_t>(std::get<double>(args[0]));
                    }

                    std::vector<uint8_t> data;
                    data.reserve(std::min(n, size_t(4096)));

#ifdef _WIN32
                    DWORD total_read = 0;
                    while (total_read < n) {
                        DWORD to_read = static_cast<DWORD>(std::min(size_t(n - total_read), size_t(4096)));
                        DWORD read_count = 0;
                        uint8_t chunk[4096];

                        if (!ReadFile((HANDLE)file->handle, chunk, to_read, &read_count, nullptr)) {
                            throw SwaziError("IOError", "Read failed", token.loc);
                        }
                        if (read_count == 0) break;

                        data.insert(data.end(), chunk, chunk + read_count);
                        total_read += read_count;
                        file->file_pos += read_count;
                    }
#else
                    size_t total_read = 0;
                    while (total_read < n) {
                        uint8_t chunk[4096];
                        ssize_t to_read = std::min(n - total_read, sizeof(chunk));
                        ssize_t read_count = ::read(file->fd, chunk, to_read);

                        if (read_count < 0) {
                            throw SwaziError("IOError", "Read failed: " + std::string(std::strerror(errno)), token.loc);
                        }
                        if (read_count == 0) break;

                        data.insert(data.end(), chunk, chunk + read_count);
                        total_read += read_count;
                        file->file_pos += read_count;
                    }
#endif

                    if (file->is_binary) {
                        auto buf = std::make_shared<BufferValue>();
                        buf->data = std::move(data);
                        buf->encoding = "binary";
                        return Value{buf};
                    }

                    return Value{std::string(data.begin(), data.end())};
                });
            }

            // file.write(data) -> number (bytes written)
            if (prop == "write") {
                return make_fn([file](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    if (!file->is_open) {
                        throw SwaziError("IOError", "Cannot write to closed file", token.loc);
                    }
                    if (args.empty()) {
                        throw SwaziError("TypeError", "write requires data argument", token.loc);
                    }

                    std::vector<uint8_t> data;
                    if (std::holds_alternative<BufferPtr>(args[0])) {
                        data = std::get<BufferPtr>(args[0])->data;
                    } else if (std::holds_alternative<std::string>(args[0])) {
                        std::string s = std::get<std::string>(args[0]);
                        data.assign(s.begin(), s.end());
                    } else {
                        throw SwaziError("TypeError", "write expects string or Buffer", token.loc);
                    }

#ifdef _WIN32
                    DWORD written = 0;
                    if (!WriteFile((HANDLE)file->handle, data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
                        throw SwaziError("IOError", "Write failed", token.loc);
                    }
                    file->file_pos += written;
                    return Value{static_cast<double>(written)};
#else
                    ssize_t written = ::write(file->fd, data.data(), data.size());
                    if (written < 0) {
                        throw SwaziError("IOError", "Write failed: " + std::string(std::strerror(errno)), token.loc);
                    }
                    file->file_pos += written;
                    return Value{static_cast<double>(written)};
#endif
                });
            }

            // file.close() -> null
            if (prop == "close") {
                return make_fn([file](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    if (file->is_open) {
                        file->close_internal();
                    }
                    return std::monostate{};
                });
            }

            // file.seek(offset, whence=0) -> number (new position)
            if (prop == "seek") {
                return make_fn([file](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    if (!file->is_open) {
                        throw SwaziError("IOError", "Cannot seek closed file", token.loc);
                    }
                    if (args.empty()) {
                        throw SwaziError("TypeError", "seek requires offset argument", token.loc);
                    }

                    long long offset = static_cast<long long>(std::get<double>(args[0]));
                    int whence = 0;  // 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END
                    if (args.size() >= 2) {
                        whence = static_cast<int>(std::get<double>(args[1]));
                    }

#ifdef _WIN32
                    DWORD method = (whence == 2) ? FILE_END : (whence == 1) ? FILE_CURRENT
                                                                            : FILE_BEGIN;
                    LARGE_INTEGER dist;
                    dist.QuadPart = offset;
                    LARGE_INTEGER newPos;
                    if (!SetFilePointerEx((HANDLE)file->handle, dist, &newPos, method)) {
                        throw SwaziError("IOError", "Seek failed", token.loc);
                    }
                    file->file_pos = static_cast<size_t>(newPos.QuadPart);
                    return Value{static_cast<double>(newPos.QuadPart)};
#else
                    off_t result = lseek(file->fd, offset, whence);
                    if (result < 0) {
                        throw SwaziError("IOError", "Seek failed: " + std::string(std::strerror(errno)), token.loc);
                    }
                    file->file_pos = static_cast<size_t>(result);
                    return Value{static_cast<double>(result)};
#endif
                });
            }

            // file.tell() -> number (current position)
            if (prop == "tell") {
                return make_fn([file](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{static_cast<double>(file->file_pos)};
                });
            }

            // file.readLine() -> string | null
            if (prop == "readLine") {
                return make_fn([file](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                    if (!file->is_open) {
                        throw SwaziError("IOError", "Cannot read from closed file", token.loc);
                    }

                    std::string line;
#ifdef _WIN32
                    char ch;
                    DWORD read_count;
                    while (ReadFile((HANDLE)file->handle, &ch, 1, &read_count, nullptr) && read_count == 1) {
                        file->file_pos++;
                        if (ch == '\n') break;
                        if (ch != '\r') line += ch;
                    }
#else
                    char ch;
                    while (::read(file->fd, &ch, 1) == 1) {
                        file->file_pos++;
                        if (ch == '\n') break;
                        if (ch != '\r') line += ch;
                    }
#endif

                    return line.empty() ? Value{std::monostate{}} : Value{line};
                });
            }

            // Properties
            if (prop == "position") return Value{static_cast<double>(file->file_pos)};
            if (prop == "path") return Value{file->path};
            if (prop == "is_open") return Value{file->is_open};

            throw SwaziError("ReferenceError", "Unknown file property: " + prop, mem->token.loc);
        }

        if (std::holds_alternative<RangePtr>(objVal)) {
            RangePtr range = std::get<RangePtr>(objVal);
            const std::string& prop = mem->property;

            // Helper to create native function values
            auto make_fn = [this, range, env, mem](std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) -> Value {
                auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    return impl(args, callEnv, token);
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:range.") + mem->property, native_impl, env, mem->token);
                return Value{fn};
            };

            // r.toArray() -> converts range to array
            if (prop == "toArray") {
                return make_fn([this, range](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!range) {
                        throw SwaziError("TypeError", "Cannot convert null range to array", token.loc);
                    }

                    auto arr = std::make_shared<ArrayValue>();

                    // Create a copy to iterate without modifying original
                    RangeValue r = *range;

                    // Limit array size to prevent memory issues
                    const size_t MAX_RANGE_ELEMENTS = 1000000;  // 1 million elements max
                    size_t count = 0;

                    while (r.hasNext() && count < MAX_RANGE_ELEMENTS) {
                        arr->elements.push_back(Value{static_cast<double>(r.next())});
                        count++;
                    }

                    if (count >= MAX_RANGE_ELEMENTS && r.hasNext()) {
                        throw SwaziError(
                            "RangeError",
                            "Range too large to convert to array (exceeds 1,000,000 elements)",
                            token.loc);
                    }

                    return Value{arr};
                });
            }

            // r.reverse() / r.geuza() -> reverses the range
            if (prop == "reverse" || prop == "geuza") {
                return make_fn([this, range](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!range) {
                        throw SwaziError("TypeError", "Cannot reverse null range", token.loc);
                    }

                    // Create a new reversed range
                    auto reversed = std::make_shared<RangeValue>(
                        range->end,  // swap start and end
                        range->start,
                        range->step,
                        range->inclusive);

                    // Reset to start position
                    reversed->cur = reversed->start;

                    // Infer direction (will be opposite of original)
                    reversed->increasing = (reversed->start <= reversed->end);

                    return Value{reversed};
                });
            }

            // r.kuna(x) / r.includes(x) -> checks if range contains x
            if (prop == "kuna" || prop == "includes") {
                return make_fn([this, range](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (!range) {
                        throw SwaziError("TypeError", "Cannot check inclusion on null range", token.loc);
                    }

                    if (args.empty()) {
                        throw SwaziError(
                            "TypeError",
                            "range.kuna/includes requires an argument (number or range)",
                            token.loc);
                    }

                    const Value& arg = args[0];

                    // Case 1: Check if a number is in the range
                    if (std::holds_alternative<double>(arg)) {
                        int x = static_cast<int>(std::get<double>(arg));

                        // Check if x is within bounds (inclusive/exclusive aware)
                        bool withinBounds;
                        if (range->increasing) {
                            if (range->inclusive) {
                                withinBounds = (x >= range->start && x <= range->end);
                            } else {
                                withinBounds = (x >= range->start && x < range->end);
                            }
                        } else {
                            if (range->inclusive) {
                                withinBounds = (x <= range->start && x >= range->end);
                            } else {
                                withinBounds = (x <= range->start && x > range->end);
                            }
                        }

                        if (!withinBounds) return Value{false};

                        // Check if x aligns with the step
                        int offset = std::abs(x - range->start);
                        return Value{(offset % static_cast<int>(range->step)) == 0};
                    }

                    // Case 2: Check if another range is entirely contained within this range
                    // True only if EVERY element of the other range is in this range
                    if (std::holds_alternative<RangePtr>(arg)) {
                        RangePtr other = std::get<RangePtr>(arg);
                        if (!other) return Value{false};

                        // Iterate through all elements of the other range
                        // and check if each one is contained in this range
                        int current = other->start;
                        int step = static_cast<int>(other->step);

                        while (true) {
                            // Check if current element is in this range
                            bool withinBounds;
                            if (range->increasing) {
                                if (range->inclusive) {
                                    withinBounds = (current >= range->start && current <= range->end);
                                } else {
                                    withinBounds = (current >= range->start && current < range->end);
                                }
                            } else {
                                if (range->inclusive) {
                                    withinBounds = (current <= range->start && current >= range->end);
                                } else {
                                    withinBounds = (current <= range->start && current > range->end);
                                }
                            }

                            if (!withinBounds) return Value{false};

                            // Check step alignment
                            int offset = std::abs(current - range->start);
                            if ((offset % static_cast<int>(range->step)) != 0) {
                                return Value{false};
                            }

                            // Move to next element in other range
                            // Check if we've reached the end
                            if (other->increasing) {
                                if (other->inclusive) {
                                    if (current == other->end) break;
                                    if (current + step > other->end) break;
                                } else {
                                    if (current + step >= other->end) break;
                                }
                                current += step;
                            } else {
                                if (other->inclusive) {
                                    if (current == other->end) break;
                                    if (current - step < other->end) break;
                                } else {
                                    if (current - step <= other->end) break;
                                }
                                current -= step;
                            }
                        }

                        return Value{true};
                    }

                    throw SwaziError(
                        "TypeError",
                        "range.kuna/includes expects a number or another range",
                        token.loc);
                });
            }

            // If no matching property found, throw error
            throw std::runtime_error(
                "ReferenceError at " + mem->token.loc.to_string() +
                "\nUnknown property '" + mem->property + "' on range." +
                "\n --> Traced at:\n" + mem->token.loc.get_line_trace());
        }

        // Function introspection and methods
        if (std::holds_alternative<FunctionPtr>(objVal)) {
            FunctionPtr fn = std::get<FunctionPtr>(objVal);
            const std::string& prop = mem->property;

            // fn.name -> string
            if (prop == "name") {
                return Value{fn->name.empty() ? std::string("<anonymous>") : fn->name};
            }

            // fn.arity -> {min: number, max: number}
            if (prop == "arity") {
                auto obj = std::make_shared<ObjectValue>();
                size_t min = 0, max = 0;

                if (fn->is_native) {
                    // Native functions: unknown arity
                    obj->properties["min"] = {Value{0.0}, false, true, false, mem->token};
                    obj->properties["max"] = {Value{std::numeric_limits<double>::infinity()}, false, true, false, mem->token};
                } else {
                    for (const auto& p : fn->parameters) {
                        if (!p) {
                            max++;
                            continue;
                        }
                        if (p->is_rest) {
                            max = SIZE_MAX;  // infinite for rest params
                            min += p->rest_required_count;
                        } else if (!p->defaultValue) {
                            min++;
                            max++;
                        } else {
                            max++;
                        }
                    }
                    obj->properties["min"] = {Value{static_cast<double>(min)}, false, true, false, mem->token};
                    obj->properties["max"] = {Value{max == SIZE_MAX ? std::numeric_limits<double>::infinity() : static_cast<double>(max)}, false, true, false, mem->token};
                }
                return Value{obj};
            }

            // fn.isAsync() -> bool (method)
            if (prop == "isAsync") {
                auto native_impl = [fn](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{fn->is_async};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.isAsync", native_impl, env, mem->token)};
            }

            // fn.isNative() -> bool (method)
            if (prop == "isNative") {
                auto native_impl = [fn](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{fn->is_native};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.isNative", native_impl, env, mem->token)};
            }

            // fn.isGenerator() -> bool (method)
            if (prop == "isGenerator") {
                auto native_impl = [fn](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{fn->is_generator};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.isGenerator", native_impl, env, mem->token)};
            }

            // fn.body -> {size: number, id: string}
            if (prop == "body") {
                auto obj = std::make_shared<ObjectValue>();

                if (fn->is_native || !fn->body) {
                    obj->properties["size"] = {Value{0.0}, false, true, false, mem->token};
                    obj->properties["id"] = {Value{std::string("<native>")}, false, true, false, mem->token};
                } else {
                    size_t size = fn->body->body.size();
                    // Create stable hash from body content
                    std::hash<std::string> hasher;
                    std::string body_repr = fn->name + ":" + std::to_string(size);
                    size_t hash_val = hasher(body_repr);

                    obj->properties["size"] = {Value{static_cast<double>(size)}, false, true, false, mem->token};
                    obj->properties["id"] = {Value{std::to_string(hash_val)}, false, true, false, mem->token};
                }
                return Value{obj};
            }

            // fn.params() -> array of param descriptors
            if (prop == "params") {
                auto native_impl = [this, fn](const std::vector<Value>&, EnvPtr callEnv, const Token& token) -> Value {
                    auto arr = std::make_shared<ArrayValue>();

                    if (fn->is_native) {
                        return Value{arr};  // Empty for native functions
                    }

                    for (const auto& p : fn->parameters) {
                        if (!p) continue;

                        auto desc = std::make_shared<ObjectValue>();
                        desc->properties["name"] = {Value{p->name}, false, true, false, token};
                        desc->properties["is_rest"] = {Value{p->is_rest}, false, true, false, token};

                        bool hasDefault = (p->defaultValue != nullptr);
                        desc->properties["has_default"] = {Value{hasDefault}, false, true, false, token};
                        arr->elements.push_back(Value{desc});
                    }

                    return Value{arr};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.params", native_impl, env, mem->token)};
            }

            // fn.source -> {file: string, line: number, col: number}
            if (prop == "source") {
                auto obj = std::make_shared<ObjectValue>();
                obj->properties["file"] = {Value{fn->token.loc.filename}, false, true, false, mem->token};
                obj->properties["line"] = {Value{static_cast<double>(fn->token.loc.line)}, false, true, false, mem->token};
                obj->properties["col"] = {Value{static_cast<double>(fn->token.loc.col)}, false, true, false, mem->token};
                return Value{obj};
            }

            // fn.signature -> string
            if (prop == "signature") {
                std::ostringstream sig;
                sig << "(";
                if (!fn->is_native) {
                    for (size_t i = 0; i < fn->parameters.size(); ++i) {
                        if (i > 0) sig << ", ";
                        auto& p = fn->parameters[i];
                        if (p) {
                            if (p->is_rest) sig << "...";
                            sig << p->name;
                        }
                    }
                } else {
                    sig << "...";
                }
                sig << ")";
                return Value{sig.str()};
            }

            // fn.wrap(wrapperFn) -> new wrapped function
            if (prop == "wrap") {
                auto native_impl = [this, fn, mem](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                        throw SwaziError("TypeError", "fn.wrap() requires a wrapper function", token.loc);
                    }

                    FunctionPtr wrapperFn = std::get<FunctionPtr>(args[0]);

                    // Create new wrapped function
                    auto wrapped = std::make_shared<FunctionValue>(
                        fn->name + "$wrapped",
                        std::vector<std::shared_ptr<ParameterNode>>{},
                        nullptr,
                        fn->closure,
                        mem->token);

                    wrapped->is_native = true;
                    wrapped->is_async = fn->is_async;
                    wrapped->is_generator = fn->is_generator;

                    // Store wrapper implementation
                    wrapped->native_impl = [this, wrapperFn, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& tok) -> Value {
                        // Build args array for wrapper
                        auto argsArr = std::make_shared<ArrayValue>();
                        argsArr->elements = args;

                        // Call: wrapperFn(originalFn, argsArray)
                        return this->call_function(wrapperFn, {fn, Value{argsArr}}, callEnv, tok);
                    };

                    wrapped->wrapper_impl = [this, wrapperFn, fn](FunctionPtr orig, const std::vector<Value>& args, EnvPtr env, const Token& tok) -> Value {
                        auto argsArr = std::make_shared<ArrayValue>();
                        argsArr->elements = args;
                        return this->call_function(wrapperFn, {fn, Value{argsArr}}, env, tok);
                    };

                    return Value{wrapped};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.wrap", native_impl, env, mem->token)};
            }

            // fn.partial(...boundArgs) -> new function with bound args
            if (prop == "partial") {
                auto native_impl = [this, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    // Clone bound arguments
                    std::vector<Value> boundArgs = args;

                    // Create the partial function directly
                    auto partialFn = std::make_shared<FunctionValue>(
                        fn->name + "$partial",
                        std::vector<std::shared_ptr<ParameterNode>>{},  // Empty params
                        nullptr,
                        fn->closure,
                        fn->token);

                    partialFn->is_native = true;
                    partialFn->is_async = fn->is_async;
                    partialFn->is_generator = fn->is_generator;

                    // The native_impl of the partial function combines bound + call args
                    partialFn->native_impl = [this, fn, boundArgs](const std::vector<Value>& callArgs, EnvPtr env, const Token& tok) -> Value {
                        std::vector<Value> combined;
                        combined.reserve(boundArgs.size() + callArgs.size());
                        combined.insert(combined.end(), boundArgs.begin(), boundArgs.end());
                        combined.insert(combined.end(), callArgs.begin(), callArgs.end());
                        return this->call_function(fn, combined, env, tok);
                    };

                    return Value{partialFn};  // Return the partial function directly
                };
                return Value{std::make_shared<FunctionValue>("native:fn.partial", native_impl, env, mem->token)};
            }

            // fn.compose(otherFn) -> new function f(g(x))
            if (prop == "compose") {
                auto native_impl = [this, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                        throw SwaziError("TypeError", "fn.compose() requires a function argument", token.loc);
                    }

                    FunctionPtr g = std::get<FunctionPtr>(args[0]);

                    auto composed = std::make_shared<FunctionValue>(
                        fn->name + "$compose$" + g->name,
                        std::vector<std::shared_ptr<ParameterNode>>{},
                        nullptr,
                        fn->closure,
                        fn->token);

                    composed->is_native = true;
                    composed->native_impl = [this, fn, g](const std::vector<Value>& callArgs, EnvPtr env, const Token& tok) -> Value {
                        Value gResult = this->call_function(g, callArgs, env, tok);
                        return this->call_function(fn, {gResult}, env, tok);
                    };

                    return Value{composed};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.compose", native_impl, env, mem->token)};
            }

            // fn.bind(receiver, ...args) -> new function with bound receiver and args
            if (prop == "bind") {
                auto native_impl = [this, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (args.empty()) {
                        throw SwaziError("TypeError", "fn.bind() requires at least a receiver argument", token.loc);
                    }

                    Value receiver = args[0];
                    std::vector<Value> boundArgs(args.begin() + 1, args.end());

                    auto bound = std::make_shared<FunctionValue>(
                        fn->name + "$bound",
                        std::vector<std::shared_ptr<ParameterNode>>{},
                        nullptr,
                        fn->closure,
                        fn->token);

                    bound->is_native = true;
                    bound->is_async = fn->is_async;
                    bound->is_generator = fn->is_generator;
                    bound->native_impl = [this, fn, receiver, boundArgs](const std::vector<Value>& callArgs, EnvPtr env, const Token& tok) -> Value {
                        std::vector<Value> combined;
                        combined.reserve(boundArgs.size() + callArgs.size());
                        combined.insert(combined.end(), boundArgs.begin(), boundArgs.end());
                        combined.insert(combined.end(), callArgs.begin(), callArgs.end());

                        // If receiver is an object, call with receiver
                        if (std::holds_alternative<ObjectPtr>(receiver)) {
                            return this->call_function_with_receiver(fn, std::get<ObjectPtr>(receiver), combined, env, tok);
                        }
                        return this->call_function(fn, combined, env, tok);
                    };

                    return Value{bound};
                };
                return Value{std::make_shared<FunctionValue>("native:fn.bind", native_impl, env, mem->token)};
            }

            // fn.invoke(options) -> fundamental execution primitive
            if (prop == "invoke") {
                auto native_impl = [this, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
                        throw SwaziError("TypeError", "fn.invoke() requires an options object {receiver?, arguments}", token.loc);
                    }

                    ObjectPtr opts = std::get<ObjectPtr>(args[0]);

                    // Extract receiver (default null)
                    Value receiver = std::monostate{};
                    auto recIt = opts->properties.find("receiver");
                    if (recIt != opts->properties.end()) {
                        receiver = recIt->second.value;
                    }

                    // Extract arguments array (required)
                    std::vector<Value> invokeArgs;
                    auto argsIt = opts->properties.find("arguments");
                    if (argsIt != opts->properties.end()) {
                        if (std::holds_alternative<ArrayPtr>(argsIt->second.value)) {
                            ArrayPtr arr = std::get<ArrayPtr>(argsIt->second.value);
                            if (arr) invokeArgs = arr->elements;
                        } else {
                            throw SwaziError("TypeError", "fn.invoke() options.arguments must be an array", token.loc);
                        }
                    }

                    // Execute with or without receiver
                    if (std::holds_alternative<ObjectPtr>(receiver)) {
                        return this->call_function_with_receiver(fn, std::get<ObjectPtr>(receiver), invokeArgs, callEnv, token);
                    }
                    return this->call_function(fn, invokeArgs, callEnv, token);
                };
                return Value{std::make_shared<FunctionValue>("native:fn.invoke", native_impl, env, mem->token)};
            }

            // fn.call(receiver, ...args) -> convenience wrapper
            if (prop == "call") {
                auto native_impl = [this, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (args.empty()) {
                        return this->call_function(fn, {}, callEnv, token);
                    }

                    Value receiver = args[0];
                    std::vector<Value> callArgs(args.begin() + 1, args.end());

                    if (std::holds_alternative<ObjectPtr>(receiver)) {
                        return this->call_function_with_receiver(fn, std::get<ObjectPtr>(receiver), callArgs, callEnv, token);
                    }
                    return this->call_function(fn, callArgs, callEnv, token);
                };
                return Value{std::make_shared<FunctionValue>("native:fn.call", native_impl, env, mem->token)};
            }

            // fn.apply(receiver, argsArray) -> convenience wrapper
            if (prop == "apply") {
                auto native_impl = [this, fn](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    Value receiver = args.size() > 0 ? args[0] : std::monostate{};

                    std::vector<Value> callArgs;
                    if (args.size() > 1 && std::holds_alternative<ArrayPtr>(args[1])) {
                        ArrayPtr arr = std::get<ArrayPtr>(args[1]);
                        if (arr) callArgs = arr->elements;
                    }

                    if (std::holds_alternative<ObjectPtr>(receiver)) {
                        return this->call_function_with_receiver(fn, std::get<ObjectPtr>(receiver), callArgs, callEnv, token);
                    }
                    return this->call_function(fn, callArgs, callEnv, token);
                };
                return Value{std::make_shared<FunctionValue>("native:fn.apply", native_impl, env, mem->token)};
            }
        }

        // String property 'herufi' (length)
        if (std::holds_alternative<std::string>(objVal) && (mem->property == "herufi" || mem->property == "size")) {
            const std::string& s = std::get<std::string>(objVal);
            return Value{
                static_cast<double>(s.size())};
        }

        // String methods/properties
        if (std::holds_alternative<std::string>(objVal)) {
            const std::string s_val = std::get<std::string>(objVal);
            const std::string& prop = mem->property;

            // helper to create native function values (captures s_val and this)
            auto make_fn = [this, s_val, env, mem](std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) -> Value {
                auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    return impl(args, callEnv, token);
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:string.") + mem->property, native_impl, env, mem->token);
                return Value{fn};
            };

            // herufiNdogo() -> toLowerCase
            if (prop == "herufiNdogo" || prop == "toLower") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string out = s_val;
                    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return Value{out};
                });
            }

            // herufiKubwa() -> toUpperCase
            if (prop == "herufiKubwa" || prop == "toUpper") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string out = s_val;
                    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    return Value{out};
                });
            }

            // sawazisha() -> trim()
            if (prop == "sawazisha" || prop == "trim") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    size_t a = 0, b = s_val.size();
                    while (a < b && std::isspace(static_cast<unsigned char>(s_val[a]))) ++a;
                    while (b > a && std::isspace(static_cast<unsigned char>(s_val[b - 1]))) --b;
                    return Value{s_val.substr(a, b - a)};
                });
            }

            // anzaNa(prefix) -> startsWith
            if (prop == "huanzaNa" || prop == "startsWith") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.anzaNa requires 1 argument (prefix)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    std::string pref = to_string_value(args[0]);
                    if (pref.size() > s_val.size()) return Value{false};
                    return Value{s_val.rfind(pref, 0) == 0};
                });
            }

            // ishaNa(suffix) -> endsWith
            if (prop == "huishaNa" || prop == "endsWith") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.ishaNa requires 1 argument (suffix)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    std::string suf = to_string_value(args[0]);
                    if (suf.size() > s_val.size()) return Value{false};
                    return Value{s_val.compare(s_val.size() - suf.size(), suf.size(), suf) == 0};
                });
            }

            // kuna(sub) -> includes
            if (prop == "kuna" || prop == "includes") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.kuna requires 1 argument (substring)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    std::string sub = to_string_value(args[0]);
                    return Value{s_val.find(sub) != std::string::npos};
                });
            }

            // tafuta(sub, fromIndex?) -> indexOf
            if (prop == "tafuta" || prop == "indexOf") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.tafuta requires 1 argument (substring)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    std::string sub = to_string_value(args[0]);
                    size_t from = 0;
                    if (args.size() >= 2) from = static_cast<size_t>(std::max(0LL, static_cast<long long>(to_number(args[1], token))));
                    size_t pos = s_val.find(sub, from);
                    if (pos == std::string::npos) return Value{static_cast<double>(-1)};
                    return Value{static_cast<double>(pos)};
                });
            }

            // tafutaMwisho(sub, fromIndex?) -> lastIndexOf
            if (prop == "tafutaMwisho" || prop == "lastIndexOf") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.tafutaMwisho requires 1 argument (substring)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());

                    std::string sub = to_string_value(args[0]);
                    size_t from = std::string::npos;  // Default to searching the whole string

                    // Handle the optional fromIndex argument
                    if (args.size() >= 2) {
                        // The search starts *at or before* this index, moving backward.
                        // We cast to size_t after getting the number.
                        from = static_cast<size_t>(to_number(args[1], token));
                    }

                    size_t pos = s_val.rfind(sub, from);

                    if (pos == std::string::npos) return Value{static_cast<double>(-1)};

                    return Value{static_cast<double>(pos)};
                });
            }

            // slesi(start?, end?) -> substring-like slice
            if (prop == "slesi" || prop == "substr") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    long long n = static_cast<long long>(s_val.size());
                    long long start = 0;
                    long long end = n;
                    if (args.size() >= 1) start = static_cast<long long>(to_number(args[0], token));
                    if (args.size() >= 2) end = static_cast<long long>(to_number(args[1], token));
                    if (start < 0) start = std::max(0LL, n + start);
                    if (end < 0) end = std::max(0LL, n + end);
                    start = std::min(std::max(0LL, start), n);
                    end = std::min(std::max(0LL, end), n);
                    return Value{s_val.substr((size_t)start, (size_t)(end - start))};
                });
            }

            // badilisha(old, neu) -> replace first occurrence
            if (prop == "badilisha" || prop == "replace") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.size() < 2)
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.badilisha requires 2 arguments (old, new)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    std::string oldv = to_string_value(args[0]);
                    std::string newv = to_string_value(args[1]);
                    std::string out = s_val;
                    size_t pos = out.find(oldv);
                    if (pos != std::string::npos) out.replace(pos, oldv.size(), newv);
                    return Value{out};
                });
            }

            // badilishaZote(old, new) -> replace all occurrences
            if (prop == "badilishaZote" || prop == "replaceAll") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.size() < 2)
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.badilishaZote requires 2 arguments (old, new)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    std::string oldv = to_string_value(args[0]);
                    std::string newv = to_string_value(args[1]);
                    if (oldv.empty()) return Value{s_val};  // avoid infinite loop
                    std::string out;
                    size_t pos = 0, prev = 0;
                    while ((pos = s_val.find(oldv, prev)) != std::string::npos) {
                        out.append(s_val, prev, pos - prev);
                        out.append(newv);
                        prev = pos + oldv.size();
                    }
                    out.append(s_val, prev, std::string::npos);
                    return Value{out};
                });
            }

            // orodhesha(separator?) -> split into ArrayPtr
            if (prop == "orodhesha" || prop == "split") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string sep;
                    bool useSep = false;
                    if (!args.empty()) {
                        sep = to_string_value(args[0]);
                        useSep = true;
                    }
                    auto out = std::make_shared<ArrayValue>();
                    if (!useSep) {
                        for (size_t i = 0; i < s_val.size(); ++i) out->elements.push_back(Value{std::string(1, s_val[i])});
                        return Value{out};
                    }
                    if (sep.empty()) {
                        for (size_t i = 0; i < s_val.size(); ++i) out->elements.push_back(Value{std::string(1, s_val[i])});
                        return Value{out};
                    }
                    size_t pos = 0, prev = 0;
                    while ((pos = s_val.find(sep, prev)) != std::string::npos) {
                        out->elements.push_back(Value{s_val.substr(prev, pos - prev)});
                        prev = pos + sep.size();
                    }
                    out->elements.push_back(Value{s_val.substr(prev)});
                    return Value{out};
                });
            }

            // unganisha(other) -> concat and return new string
            if (prop == "unganisha" || prop == "concat") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.unganisha requires at least 1 argument (string to concat)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    return Value{s_val + to_string_value(args[0])};
                });
            }

            // rudia(n) -> repeat string n times
            if (prop == "rudia") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.rudia requires 1 argument (repeat count)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    long long n = static_cast<long long>(to_number(args[0], token));
                    if (n <= 0) return Value{std::string()};
                    std::string out;
                    out.reserve(s_val.size() * (size_t)n);
                    for (long long i = 0; i < n; ++i) out += s_val;
                    return Value{out};
                });
            }

            // herufiYa(index) -> charAt (single-char string or empty)
            if (prop == "herufiYa" || prop == "charAt") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.herufiYa requires 1 argument (index)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                    long long idx = static_cast<long long>(to_number(args[0], token));
                    if (idx < 0 || (size_t)idx >= s_val.size()) return Value{std::string()};
                    return Value{std::string(1, s_val[(size_t)idx])};
                });
            }

            // urefu() -> returns string length (same as .herufi property)
            if (prop == "urefu") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    return Value{static_cast<double>(s_val.size())};
                });
            }

            // geuza() -> reverse() -> returns a reversed string
            if (prop == "geuza" || prop == "reverse") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string out = s_val;
                    std::reverse(out.begin(), out.end());
                    return Value{out};
                });
            }

            // nambaYaHerufi(index) -> charCodeAt (returns the code point of the character at the index)
            if (prop == "nambaYaHerufi" || prop == "charCodeAt") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty())
                        throw std::runtime_error(
                            "TypeError at " + token.loc.to_string() +
                            "\nstr.nambaYaHerufi requires 1 argument (index)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());

                    long long idx = static_cast<long long>(to_number(args[0], token));

                    if (idx < 0 || (size_t)idx >= s_val.size()) {
                        // Return NaN or a specific error indicator for out-of-bounds index
                        // Returning 0.0 or -1.0 is common, but 0.0 aligns with JavaScript's behavior for an empty string's charCodeAt(0)
                        return Value{static_cast<double>(0)};
                    }
                    // Return the ASCII/char code as a double
                    return Value{static_cast<double>(static_cast<unsigned char>(s_val[(size_t)idx]))};
                });
            }

            // No matching string property -> fall through to unknown property error below
        }

        // --- Number methods & properties (place this after string methods, before array methods) ---
        if (std::holds_alternative<double>(objVal)) {
            double num = std::get<double>(objVal);
            const std::string& prop = mem->property;

            // ---------- Properties ----------
            if (prop == "isNaN") {
                return Value{std::isnan(num)};
            }
            if (prop == "isInf") {
                return Value{!std::isfinite(num)};
            }
            if (prop == "isInt") {
                return Value{std::isfinite(num) && std::floor(num) == num};
            }
            if (prop == "nidesimali" || prop == "isDec") {
                return Value{std::isfinite(num) && std::floor(num) != num};
            }
            if (prop == "nichanya" || prop == "isPos") {
                return Value{num > 0};
            }
            if (prop == "nihasi" || prop == "isNeg") {
                return Value{num < 0};
            }

            // boolean "is" properties: odd, even, prime
            if (prop == "niwitiri" || prop == "nishufwa" || prop == "nitasa" || prop == "isEven" || prop == "isOdd" || prop == "isPrime") {
                if (!std::isfinite(num) || std::floor(num) != num) {
                    return Value{false};
                }
                if (num > static_cast<double>(LLONG_MAX) || num < static_cast<double>(LLONG_MIN)) {
                    return Value{false};
                }

                long long n = static_cast<long long>(std::llround(num));

                if (prop == "niwitiri" || prop == "isEven") {
                    return Value{(n % 2) != 0};
                }
                if (prop == "nishufwa" || prop == "isOdd") {
                    return Value{(n % 2) == 0};
                }
                if (prop == "nitasa" || prop == "isPrime") {
                    if (n < 2) return Value{false};
                    if (n % 2 == 0) return Value{n == 2};
                    long long limit = static_cast<long long>(std::sqrt((long double)n));
                    for (long long i = 3; i <= limit; i += 2) {
                        if (n % i == 0) return Value{false};
                    }
                    return Value{true};
                }
            }

            // ---------- Methods (return FunctionValue) ----------

            if (prop == "abs") {
                auto native_impl = [num](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{std::fabs(num)};
                };
                return Value{std::make_shared<FunctionValue>("native:number.abs", native_impl, env, mem->token)};
            }

            if (prop == "kadiria" || prop == "round") {
                auto native_impl = [num](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{static_cast<double>(std::round(num))};
                };
                return Value{std::make_shared<FunctionValue>("native:number.kadiria", native_impl, env, mem->token)};
            }

            if (prop == "kadiriajuu" || prop == "ceil") {
                auto native_impl = [num](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{static_cast<double>(std::ceil(num))};
                };
                return Value{std::make_shared<FunctionValue>("native:number.kadiriajuu", native_impl, env, mem->token)};
            }

            if (prop == "kadiriachini" || prop == "floor") {
                auto native_impl = [num](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                    return Value{static_cast<double>(std::floor(num))};
                };
                return Value{std::make_shared<FunctionValue>("native:number.kadiriachini", native_impl, env, mem->token)};
            }

            if (prop == "kipeo" || prop == "pow") {
                auto native_impl = [this, num](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    double b = args.empty() ? num : to_number(args[0], token);
                    return Value{args.empty() ? num * num : std::pow(num, b)};
                };
                return Value{std::make_shared<FunctionValue>("native:number.kipeo", native_impl, env, mem->token)};
            }

            if (prop == "kipeuo" || prop == "root") {
                auto native_impl = [this, num](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    double b = args.empty() ? 2.0 : to_number(args[0], token);
                    if (b == 0.0) throw std::runtime_error(
                        "ValueError at " + token.loc.to_string() +
                        "\nCannot divide by zero" +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                    if (!std::isfinite(b)) throw std::runtime_error(
                        "ValueError at " + token.loc.to_string() +
                        "\nRoot degree value is infinite" +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                    if (num == 0.0 && b < 0.0) throw std::runtime_error(
                        "ValueError at " + token.loc.to_string() +
                        "\nCannot raise 0.0 to a negative power" +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                    if (num < 0.0 && std::fabs(b - std::round(b)) > 1e-12) throw std::runtime_error(
                        "ValueError at " + token.loc.to_string() +
                        "\nCannot compute fractional root of a negative number (would produce a complex result: a + bi)" +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                    return Value{std::pow(num, 1.0 / b)};
                };
                return Value{std::make_shared<FunctionValue>("native:number.kipeuo", native_impl, env, mem->token)};
            }

            if (prop == "kubwa" || prop == "max" || prop == "ndogo" || prop == "min") {
                bool wantMax = (prop == "kubwa" || prop == "max");
                auto native_impl = [this, num, wantMax](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error(
                        "ValueError at " + token.loc.to_string() +
                        "\nFunction n." + (wantMax ? "kubwa / max" : "ndogo / min") + " requires at least 1 argument" +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                    if (args.size() == 1) return Value{wantMax ? std::max(num, to_number(args[0], token)) : std::min(num, to_number(args[0], token))};
                    auto out = std::make_shared<ArrayValue>();
                    out->elements.reserve(args.size());
                    for (const auto& a : args) {
                        double v = to_number(a, token);
                        out->elements.push_back(Value{wantMax ? std::max(num, v) : std::min(num, v)});
                    }
                    return Value{out};
                };
                return Value{std::make_shared<FunctionValue>("native:number." + prop, native_impl, env, mem->token)};
            }

            if (prop == "kadiriaKwa" || prop == "toFixed") {
                auto native_impl = [this, num](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    int digits = 0;
                    if (!args.empty()) digits = static_cast<int>(to_number(args[0], token));
                    std::ostringstream oss;
                    oss.setf(std::ios::fixed);
                    oss.precision(std::max(0, digits));
                    oss << num;
                    return Value{oss.str()};
                };
                return Value{std::make_shared<FunctionValue>("native:number.toFixed", native_impl, env, mem->token)};
            }

            if (prop == "kwaKiwango") {
                auto native_impl = [this, num](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error(
                        "ValueError at " + token.loc.to_string() +
                        "\nn.kwaKiwango needs at least one argument" +
                        "\n --> Traced at:\n" + token.loc.get_line_trace());
                    double factor = to_number(args[0], token);
                    return Value{num * factor};
                };
                return Value{std::make_shared<FunctionValue>("native:number.kwaKiwango", native_impl, env, mem->token)};
            }

            // If none matched, fallthrough to unknown prop logic below
        }

        // Array properties & methods
        if (std::holds_alternative<ArrayPtr>(objVal)) {
            ArrayPtr arr = std::get<ArrayPtr>(objVal);

            // length property
            if (mem->property == "idadi" || mem->property == "size") {
                return Value{static_cast<double>(arr ? arr->elements.size() : 0)};
            }

            const std::string& prop = mem->property;

            // Recognized array method names
            if (prop == "empty" || prop == "join" || prop == "reduce" || prop == "filter" || prop == "map" ||
                prop == "slice" || prop == "splice" || prop == "includes" || prop == "sort" ||
                prop == "reverse" || prop == "extend" || prop == "unshift" || prop == "insert" ||
                prop == "shift" || prop == "removeAll" || prop == "pop" || prop == "push" ||
                prop == "urefu" || prop == "indexOf" || prop == "indexYa" || prop == "tafutaIndex" ||
                prop == "ongeza" || prop == "toa" || prop == "ondoa" || prop == "ondoaMwanzo" ||
                prop == "ongezaMwanzo" || prop == "ingiza" || prop == "slesi" || prop == "clear" ||
                prop == "badili" || prop == "tafuta" || prop == "kuna" || prop == "panga" ||
                prop == "geuza" || prop == "chambua" || prop == "punguza" ||
                prop == "unganisha" || prop == "ondoaZote" || prop == "pachika" || prop == "kwaKila" || prop == "forEach" || prop == "fill" || prop == "every" || prop == "some" || prop == "baadhi") {
                auto native_impl = [this, arr, prop](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (!arr) return std::monostate{};

                    // urefu() -> returns array length (same as .idadi property)
                    if (prop == "urefu") {
                        return Value{static_cast<double>(arr->elements.size())};
                    }

                    // empty() -> bool, true if empty
                    if (prop == "empty") {
                        return Value{static_cast<bool>(arr->elements.empty())};
                    }

                    // push: ongeza(value...)
                    if (prop == "ongeza" || prop == "push") {
                        if (args.empty())
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.ongeza requires at least 1 argument (value to push). Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        arr->elements.insert(arr->elements.end(), args.begin(), args.end());
                        return Value{static_cast<double>(arr->elements.size())};
                    }

                    // pop (from end) : toa
                    if (prop == "toa" || prop == "pop") {
                        if (arr->elements.empty()) return std::monostate{};
                        Value v = arr->elements.back();
                        arr->elements.pop_back();
                        return v;
                    }

                    // remove by value: ondoa(value)
                    if (prop == "ondoa") {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.ondoa requires 1 argument (element to remove). Returns boolean (kweli|sikweli)." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        auto it = std::find_if(arr->elements.begin(), arr->elements.end(), [&](const Value& elem) {
                            return is_equal(elem, args[0]);
                        });
                        if (it != arr->elements.end()) {
                            arr->elements.erase(it);
                            return Value{true};
                        }
                        return Value{false};
                    }

                    // remove all by value: ondoaZote(value)
                    if (prop == "ondoaZote" || prop == "removeAll") {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.ondoaZote requires 1 argument (element to remove all occurrences). Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        size_t before = arr->elements.size();
                        arr->elements.erase(
                            std::remove_if(arr->elements.begin(), arr->elements.end(), [&](const Value& elem) {
                                return is_equal(elem, args[0]);
                            }),
                            arr->elements.end());
                        size_t removed = before - arr->elements.size();
                        return Value{static_cast<double>(removed)};
                    }

                    // shift: ondoaMwanzo
                    if (prop == "ondoaMwanzo" || prop == "shift") {
                        if (arr->elements.empty()) return std::monostate{};
                        Value v = arr->elements.front();
                        arr->elements.erase(arr->elements.begin());
                        return v;
                    }

                    // unshift: ongezaMwanzo(...)
                    if (prop == "ongezaMwanzo" || prop == "unshift") {
                        if (args.empty())
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.ongezaMwanzo requires at least 1 argument (value to unshift). Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        arr->elements.insert(arr->elements.begin(), args.begin(), args.end());
                        return Value{static_cast<double>(arr->elements.size())};
                    }

                    // insert(value, index)
                    if (prop == "ingiza" || prop == "insert") {
                        if (args.size() < 2)
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.ingiza requires 2 arguments (value, index). Got " + std::to_string(args.size()) + "." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        const Value& val = args[0];
                        long long idx = static_cast<long long>(to_number(args[1], token));
                        if (idx < 0) idx = 0;
                        size_t uidx = static_cast<size_t>(std::min<long long>(idx, static_cast<long long>(arr->elements.size())));
                        arr->elements.insert(arr->elements.begin() + uidx, val);
                        return Value{static_cast<double>(arr->elements.size())};
                    }

                    // clear: clear()
                    if (prop == "clear") {
                        arr->elements.clear();
                        return std::monostate{};
                    }

                    // extend: panua(otherArray)
                    if (prop == "extend") {
                        if (args.empty() || !std::holds_alternative<ArrayPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.panua expects an array as the first argument. Got " + (args.empty() ? "none" : "a non-array type") + "." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        ArrayPtr other = std::get<ArrayPtr>(args[0]);
                        auto out = std::make_shared<ArrayValue>();
                        if (arr) out->elements.insert(out->elements.end(), arr->elements.begin(), arr->elements.end());
                        if (other) out->elements.insert(out->elements.end(), other->elements.begin(), other->elements.end());
                        return Value{out};
                    }

                    // reverse: geuza()
                    if (prop == "geuza" || prop == "reverse") {
                        std::reverse(arr->elements.begin(), arr->elements.end());
                        return Value{arr};
                    }

                    // sort: panga([comparator])
                    if (prop == "panga" || prop == "sort") {
                        if (!args.empty() && !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.panga expects a comparator function as the first argument. Got a non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        if (args.empty()) {
                            std::sort(arr->elements.begin(), arr->elements.end(), [this](const Value& A, const Value& B) {
                                return to_string_value(A) < to_string_value(B);
                            });
                        } else {
                            FunctionPtr cmp = std::get<FunctionPtr>(args[0]);
                            std::sort(arr->elements.begin(), arr->elements.end(), [&](const Value& A, const Value& B) {
                                Value res = call_function(cmp, {A, B}, callEnv, token);
                                return to_number(res, token) < 0;
                            });
                        }
                        return Value{arr};
                    }

                    // indexOf: indexOf(value, start?) -> returns index or -1
                    if (prop == "indexOf" || prop == "indexYa") {
                        if (args.empty())
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.indexOf requires at least 1 argument (value to search). Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        const Value& target = args[0];
                        long long n = static_cast<long long>(arr->elements.size());
                        if (n == 0) return Value{static_cast<double>(-1)};
                        long long startNum = 0;
                        bool backwardMode = false;
                        if (args.size() >= 2) {
                            startNum = static_cast<long long>(to_number(args[1], token));
                            if (startNum == -1) backwardMode = true;
                        }
                        if (backwardMode) {
                            for (long long i = n - 1; i >= 0; --i) {
                                if (is_equal(arr->elements[(size_t)i], target)) return Value{static_cast<double>(i)};
                                if (i == 0) break;
                            }
                            return Value{static_cast<double>(-1)};
                        }
                        long long startIndex = startNum;
                        if (args.size() >= 2 && startIndex < 0) startIndex = std::max(0LL, n + startIndex);
                        if (startIndex >= n) return Value{static_cast<double>(-1)};
                        for (long long i = startIndex; i < n; ++i) {
                            if (is_equal(arr->elements[(size_t)i], target)) return Value{static_cast<double>(i)};
                        }
                        return Value{static_cast<double>(-1)};
                    }

                    // find: tafuta(fn)
                    if (prop == "tafuta") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.tafuta requires a function as its first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, callEnv, token);
                            if (to_bool(res)) return arr->elements[i];
                        }
                        return std::monostate{};
                    }

                    // findIndex: tafutaIndex(fn)
                    if (prop == "tafutaIndex") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.tafutaIndex requires a function as its first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, callEnv, token);
                            if (to_bool(res)) return Value{static_cast<double>(i)};
                        }
                        return Value{static_cast<double>(-1)};
                    }

                    // includes: kuna(value)
                    if (prop == "kuna" || prop == "includes") {
                        if (args.empty())
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.kuna requires 1 argument (value to check). Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        for (const auto& e : arr->elements)
                            if (is_equal(e, args[0])) return Value{true};
                        return Value{false};
                    }

                    // slice: slesi(start?, end?)
                    if (prop == "slesi" || prop == "slice") {
                        long long n = static_cast<long long>(arr->elements.size());
                        long long start = 0;
                        long long end = n;
                        if (args.size() >= 1) start = static_cast<long long>(to_number(args[0], token));
                        if (args.size() >= 2) end = static_cast<long long>(to_number(args[1], token));
                        if (start < 0) start = std::max(0LL, n + start);
                        if (end < 0) end = std::max(0LL, n + end);
                        start = std::min(std::max(0LL, start), n);
                        end = std::min(std::max(0LL, end), n);
                        auto out = std::make_shared<ArrayValue>();
                        for (long long i = start; i < end; ++i) out->elements.push_back(arr->elements[(size_t)i]);
                        return Value{out};
                    }

                    // splice: pachika(start, deleteCount, ...items)
                    if (prop == "pachika" || prop == "splice") {
                        if (args.size() < 2) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.pachika requires at least 2 arguments (start, deleteCount). Got " + std::to_string(args.size()) + "." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        long long start = static_cast<long long>(to_number(args[0], token));
                        long long delCount = static_cast<long long>(to_number(args[1], token));
                        if (start < 0) start = std::max(0LL, (long long)arr->elements.size() + start);
                        start = std::min(start, (long long)arr->elements.size());
                        delCount = std::max(0LL, std::min(delCount, (long long)arr->elements.size() - start));
                        auto out = std::make_shared<ArrayValue>();
                        out->elements.insert(out->elements.end(), arr->elements.begin() + start, arr->elements.begin() + start + delCount);
                        arr->elements.erase(arr->elements.begin() + start, arr->elements.begin() + start + delCount);
                        if (args.size() > 2) arr->elements.insert(arr->elements.begin() + start, args.begin() + 2, args.end());
                        return Value{out};
                    }

                    // map: badili(fn)
                    if (prop == "badili" || prop == "map") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.badili requires a function as the first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr mapper = std::get<FunctionPtr>(args[0]);
                        auto out = std::make_shared<ArrayValue>();
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(mapper, {arr->elements[i], Value{static_cast<double>(i)}}, callEnv, token);
                            out->elements.push_back(res);
                        }
                        return Value{out};
                    }

                    // forEach : kwaKila(fn)
                    if (prop == "kwaKila" || prop == "forEach") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.kwaKila requires a function argument. Syntax: arr.kwaKila(fn), fn(elem, index?, array?)" +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        FunctionPtr fn = std::get<FunctionPtr>(args[0]);
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            call_function(fn, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, callEnv, token);
                        }
                        return std::monostate{};
                    }

                    // filter: chambua(fn)
                    if (prop == "chambua" || prop == "filter") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.chambua requires a filter function as the first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);
                        auto out = std::make_shared<ArrayValue>();
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, callEnv, token);
                            if (to_bool(res)) out->elements.push_back(arr->elements[i]);
                        }
                        return Value{out};
                    }

                    // reduce: punguza(fn, initial?)
                    if (prop == "punguza" || prop == "reduce") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.punguza requires a reducer function as the first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr reducer = std::get<FunctionPtr>(args[0]);
                        size_t startIndex = 0;
                        Value acc;
                        if (args.size() >= 2) {
                            acc = args[1];
                            startIndex = 0;
                        } else {
                            if (arr->elements.empty())
                                throw std::runtime_error(
                                    "TypeError at " + token.loc.to_string() +
                                    "\narr.punguza called on empty array without initial value." +
                                    "\n --> Traced at:\n" + token.loc.get_line_trace());
                            acc = arr->elements[0];
                            startIndex = 1;
                        }
                        for (size_t i = startIndex; i < arr->elements.size(); ++i) {
                            acc = call_function(reducer, {acc, arr->elements[i], Value{static_cast<double>(i)}}, callEnv, token);
                        }
                        return acc;
                    }

                    // join: unganisha(separator?)
                    if (prop == "unganisha" || prop == "join") {
                        std::string sep = ",";
                        if (!args.empty()) sep = to_string_value(args[0]);
                        std::ostringstream oss;
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            if (i) oss << sep;
                            oss << to_string_value(arr->elements[i]);
                        }
                        return Value{oss.str()};
                    }

                    if (prop == "fill") {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.fill requires at least 1 argument (value to fill)." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        const Value& fillVal = args[0];
                        size_t arrSize = arr->elements.size();

                        // Default start = 0, end = array length
                        int64_t start = 0;
                        int64_t end = static_cast<int64_t>(arrSize);

                        // Parse start index (args[1])
                        if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                            start = static_cast<int64_t>(std::get<double>(args[1]));
                            // Handle negative indices
                            if (start < 0) {
                                start = std::max(static_cast<int64_t>(0),
                                    static_cast<int64_t>(arrSize) + start);
                            } else {
                                start = std::min(start, static_cast<int64_t>(arrSize));
                            }
                        }

                        // Parse end index (args[2])
                        if (args.size() >= 3 && std::holds_alternative<double>(args[2])) {
                            end = static_cast<int64_t>(std::get<double>(args[2]));
                            // Handle negative indices
                            if (end < 0) {
                                end = std::max(static_cast<int64_t>(0),
                                    static_cast<int64_t>(arrSize) + end);
                            } else {
                                end = std::min(end, static_cast<int64_t>(arrSize));
                            }
                        }

                        // Fill the range [start, end)
                        for (int64_t i = start; i < end; ++i) {
                            arr->elements[i] = fillVal;
                        }

                        return Value{arr};
                    }

                    // some: baadhi(fn) - returns true if at least one element passes the test
                    if (prop == "baadhi" || prop == "some") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.baadhi requires a predicate function as the first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, callEnv, token);
                            if (to_bool(res)) return Value{true};
                        }
                        return Value{false};
                    }

                    // every: every(fn) - returns true if all elements pass the test
                    if (prop == "every") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\narr.every requires a predicate function as the first argument. Got 0 or non-function type." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, callEnv, token);
                            if (!to_bool(res)) return Value{false};
                        }
                        return Value{true};
                    }

                    return std::monostate{};
                };

                auto fn = std::make_shared<FunctionValue>(std::string("native:array.") + prop, native_impl, env, mem->token);
                return Value{fn};
            }
        }

        // with proxy
        if (std::holds_alternative<ObjectPtr>(objVal)) {
            ObjectPtr obj = std::get<ObjectPtr>(objVal);
            if (obj) {
                auto proxy_it = obj->properties.find("__proxy__");
                if (proxy_it != obj->properties.end() &&
                    proxy_it->second.is_private &&
                    std::holds_alternative<ProxyPtr>(proxy_it->second.value)) {
                    ProxyPtr proxy = std::get<ProxyPtr>(proxy_it->second.value);

                    // Try to call handler.get trap
                    FunctionPtr get_trap = get_handler_method(proxy->handler, "get", mem->token);
                    if (get_trap) {
                        // Call: handler.get(target, key)
                        std::vector<Value> trap_args = {
                            Value{proxy->target},
                            Value{mem->property}};
                        return this->call_function(get_trap, trap_args, env, mem->token);
                    }

                    // No trap, fall through to default behavior on target
                    objVal = Value{proxy->target};
                }
            }
        }

        // Object properties & methods
        if (std::holds_alternative<ObjectPtr>(objVal)) {
            ObjectPtr op = std::get<ObjectPtr>(objVal);
            const std::string& prop = mem->property;

            if (prop == "__proto__") {
                auto obj = std::make_shared<ObjectValue>();
                obj->properties["object_size"] = {Value(static_cast<double>(op ? op->properties.size() : 0)), false, false, true, Token()};
                {
                    auto arr = std::make_shared<ArrayValue>();
                    for (auto& key : op->properties) {
                        arr->elements.push_back(key.first);
                    }
                    obj->properties["properties"] = {Value(arr), false, false, true, Token()};
                }
                {
                    auto set_private__proto = [&](const std::vector<Value>& args, EnvPtr env, const Token& token) {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\n__proto__.set_private requires an object key (string) as its first argument. Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        std::string name = to_string_value(args[0]);
                        auto it = op->properties.find(name);
                        if (it == op->properties.end()) {
                            throw std::runtime_error(
                                "ReferenceError at " + token.loc.to_string() +
                                "\nCannot set property to private: object does not have a property named `" + to_string_value(args[0], true) + "`." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        PropertyDescriptor& desc = it->second;
                        desc.is_private = true;
                        return Value(bool(desc.is_private));
                    };

                    auto set_private = std::make_shared<FunctionValue>("__proto__set_private", set_private__proto, env, Token{});
                    obj->properties["set_private"] = {
                        set_private,
                        false,
                        false,
                        true,
                        Token()};
                }
                {
                    auto set_lock__proto = [&](const std::vector<Value>& args, EnvPtr env, const Token& token) {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\n__proto__.set_lock requires an object key (string) as its first argument. Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        std::string name = to_string_value(args[0]);
                        auto it = op->properties.find(name);
                        if (it == op->properties.end()) {
                            throw std::runtime_error(
                                "ReferenceError at " + token.loc.to_string() +
                                "\nCannot set lock: object does not have a property named `" + to_string_value(args[0], true) + "`." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        PropertyDescriptor& desc = it->second;
                        desc.is_locked = true;
                        return Value(bool(desc.is_locked));
                    };

                    auto set_lock = std::make_shared<FunctionValue>("__proto__set_lock", set_lock__proto, env, Token{});
                    obj->properties["set_lock"] = {
                        set_lock,
                        false,
                        false,
                        true,
                        Token()};
                }
                {
                    auto delete__proto = [&](const std::vector<Value>& args, EnvPtr env, const Token& token) {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\n__proto__.delete requires a property key (string) to delete as its first argument. Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        if (op->is_frozen) throw std::runtime_error(
                            "PermissionError at " + token.loc.to_string() +
                            "\nCannot delete properties: object is frozen (no deletions/modifications allowed)." +
                            "\n --> Traced at:\n" + token.loc.get_line_trace());
                        std::string name = to_string_value(args[0]);
                        auto it = op->properties.find(name);
                        if (it == op->properties.end()) {
                            throw std::runtime_error(
                                "ReferenceError at " + token.loc.to_string() +
                                "\nCannot delete property: object does not have a key named `" + to_string_value(args[0], true) + "`." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        PropertyDescriptor& desc = it->second;
                        if (desc.is_private && !is_private_access_allowed(op, env)) {
                            throw std::runtime_error(
                                "PermissionError at " + token.loc.to_string() +
                                "\nCannot delete a private member from outside the object." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        if (desc.is_locked && !is_private_access_allowed(op, env)) {
                            throw std::runtime_error(
                                "PermissionError at " + token.loc.to_string() +
                                "\nCannot delete a locked member from outside the object." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }

                        auto itp = op->properties.find(name);
                        if (itp != op->properties.end()) {
                            op->properties.erase(itp);
                            return Value(bool(true));
                        }

                        return Value(bool(false));
                    };

                    auto delete_prop = std::make_shared<FunctionValue>("__proto__delete", delete__proto, env, Token{});
                    obj->properties["delete"] = {
                        delete_prop,
                        false,
                        false,
                        true,
                        Token()};
                }
                {
                    auto freeze__proto = [&](const std::vector<Value>& args, EnvPtr env, const Token& token) {
                        bool freeze = true;
                        if (!args.empty()) {
                            freeze = to_bool(args[0]);
                        }
                        op->is_frozen = freeze;
                        return Value(op->is_frozen);
                    };

                    auto freeze = std::make_shared<FunctionValue>("__proto__freeze", freeze__proto, env, Token{});
                    obj->properties["freeze"] = {
                        freeze,
                        false,
                        false,
                        true,
                        Token()};
                }
                {
                    auto has__proto = [&](const std::vector<Value>& args, EnvPtr env, const Token& token) {
                        if (args.empty()) {
                            throw std::runtime_error(
                                "TypeError at " + token.loc.to_string() +
                                "\n__proto__.has requires a property key (string) as its first argument. Got 0 arguments." +
                                "\n --> Traced at:\n" + token.loc.get_line_trace());
                        }
                        std::string key = to_string_value(args[0]);
                        auto it = op->properties.find(key);
                        if (it != op->properties.end()) {
                            return Value(bool(true));
                        }
                        return Value(bool(false));
                    };

                    auto has = std::make_shared<FunctionValue>("__proto__has", has__proto, env, Token{});
                    obj->properties["has"] = {
                        has,
                        false,
                        false,
                        true,
                        Token()};
                }
                {
                    auto is_frozen_fn = [&](const std::vector<Value>& args, EnvPtr env, const Token& token) {
                        return Value(op->is_frozen);
                    };

                    auto is_frozen = std::make_shared<FunctionValue>("__proto__is_frozen", is_frozen_fn, env, Token{});
                    obj->properties["is_frozen"] = {
                        is_frozen,
                        false,
                        true,
                        true,
                        Token()};
                }
                return Value(obj);
            }
            return get_object_property(op, mem->property, env, mem->token);
        }
        // For other non-array/non-string objects, return undefined for unknown props
        throw std::runtime_error(
            "ReferenceError at " + mem->token.loc.to_string() +
            "\nUnknown property '" + mem->property + "' on value." +
            "\n --> Traced at:\n" + mem->token.loc.get_line_trace());
    }

    // Indexing: obj[index]
    if (auto idx = dynamic_cast<IndexExpressionNode*>(expr)) {
        // Evaluate receiver first.
        Value objVal = evaluate_expression(idx->object.get(), env);

        // Optional chaining: if receiver is nullish and this is an optional index,
        // short-circuit and return undefined without evaluating the index expression.
        if (idx->is_optional && is_nullish(objVal)) {
            return std::monostate{};
        }

        // Ensure index expression exists
        if (!idx->index) {
            throw std::runtime_error(
                "TypeError at " + idx->token.loc.to_string() +
                "\nIndex expression missing." +
                "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
        }

        // Now safe to evaluate the index expression.
        Value indexVal = evaluate_expression(idx->index.get(), env);

        if (std::holds_alternative<ObjectPtr>(objVal)) {
            ObjectPtr obj = std::get<ObjectPtr>(objVal);
            if (obj) {
                auto proxy_it = obj->properties.find("__proxy__");
                if (proxy_it != obj->properties.end() &&
                    proxy_it->second.is_private &&
                    std::holds_alternative<ProxyPtr>(proxy_it->second.value)) {
                    ProxyPtr proxy = std::get<ProxyPtr>(proxy_it->second.value);

                    // Try to call handler.get trap
                    FunctionPtr get_trap = get_handler_method(proxy->handler, "get", idx->token);
                    if (get_trap) {
                        // Convert indexVal to string key
                        std::string key = to_property_key(indexVal, idx->token);

                        // Call: handler.get(target, key)
                        std::vector<Value> trap_args = {
                            Value{proxy->target},
                            Value{key}};
                        return this->call_function(get_trap, trap_args, env, idx->token);
                    }

                    // No trap, use target instead
                    objVal = Value{proxy->target};
                }
            }
        }

        // Array indexing uses numeric interpretation of indexVal OR range slicing
        if (std::holds_alternative<ArrayPtr>(objVal)) {
            ArrayPtr arr = std::get<ArrayPtr>(objVal);
            if (!arr) return std::monostate{};

            // Case 1: Range-based slicing (arr[range])
            if (std::holds_alternative<RangePtr>(indexVal)) {
                RangePtr range = std::get<RangePtr>(indexVal);
                if (!range) return std::monostate{};

                // Create result array
                auto result = std::make_shared<ArrayValue>();

                // Create a copy to iterate
                RangeValue r = *range;

                // Safety check for extremely large ranges
                const size_t MAX_SLICE_SIZE = 1000000;  // 1 million elements max
                size_t collected = 0;

                while (r.hasNext() && collected < MAX_SLICE_SIZE) {
                    int index = r.next();

                    // Skip negative indices
                    if (index < 0) continue;

                    // Skip indices beyond array bounds
                    if ((size_t)index >= arr->elements.size()) {
                        // If we're past the end and moving forward, stop early
                        if (r.increasing) break;
                        continue;
                    }

                    // Add element at this index
                    result->elements.push_back(arr->elements[(size_t)index]);
                    collected++;
                }

                // Check if we hit the safety limit
                if (collected >= MAX_SLICE_SIZE && r.hasNext()) {
                    throw std::runtime_error(
                        "RangeError at " + idx->token.loc.to_string() +
                        "\nRange slice exceeded maximum size of 1,000,000 elements." +
                        "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                }

                return Value{result};
            }

            // Case 2: Single numeric index (existing behavior)
            long long rawIndex = static_cast<long long>(to_number(indexVal, idx->token));
            if (rawIndex < 0 || (size_t)rawIndex >= arr->elements.size()) {
                return std::monostate{};
            }
            return arr->elements[(size_t)rawIndex];
        }

        // Buffer indexing uses index or range
        if (std::holds_alternative<BufferPtr>(objVal)) {
            BufferPtr buf = std::get<BufferPtr>(objVal);
            if (!buf) return std::monostate{};

            // Case 1: Range-based slicing (buf[range])
            // if (std::holds_alternative<RangePtr>(indexVal)) {
            //     RangePtr range = std::get<RangePtr>(indexVal);
            //     if (!range) return std::monostate{};

            //     // Create result array buffer
            //     auto result = std::make_shared<BufferValue>();

            //     // Create a copy to iterate
            //     RangeValue r = *range;

            //     // Safety check for extremely large ranges
            //     const size_t MAX_SLICE_SIZE = 1000000;  // 1 million elements max
            //     size_t collected = 0;

            //     while (r.hasNext() && collected < MAX_SLICE_SIZE) {
            //         int index = r.next();

            //         // Skip negative indices
            //         if (index < 0) continue;

            //         // Skip indices beyond buffer bounds
            //         if ((size_t)index >= buf->data.size()) {
            //             // If we're past the end and moving forward, stop early
            //             if (r.increasing) break;
            //             continue;
            //         }

            //         // Add element at this index
            //         result->data.push_back(buf->data[(size_t)index]);
            //         collected++;
            //     }

            //     // Check if we hit the safety limit
            //     if (collected >= MAX_SLICE_SIZE && r.hasNext()) {
            //         throw std::runtime_error(
            //             "RangeError at " + idx->token.loc.to_string() +
            //             "\nRange slice exceeded maximum size of 1,000,000 elements." +
            //             "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
            //     }

            //     result->encoding = buf->encoding;

            //     return Value{result};
            // }

            // Case 2: Single numeric index (buf)
            long long index = static_cast<long long>(to_number(indexVal, idx->token));
            if (index < 0 || (size_t)index >= buf->data.size()) {
                throw SwaziError(
                    "RangeError",
                    "Index " + std::to_string(index) + " is out of bounds (buffer size: " + std::to_string(buf->data.size()) + "). Index should be from 0-" + std::to_string(buf->data.size() - 1) + ".",
                    idx->token.loc);
            }

            return Value{static_cast<double>(buf->data[(size_t)index])};
        }

        // String indexing: return single-char string OR range slice
        if (std::holds_alternative<std::string>(objVal)) {
            std::string s = std::get<std::string>(objVal);

            // Case 1: Range-based slicing (str[range])
            if (std::holds_alternative<RangePtr>(indexVal)) {
                RangePtr range = std::get<RangePtr>(indexVal);
                if (!range) return std::monostate{};

                // Create result string
                std::string result;

                // Create a copy to iterate
                RangeValue r = *range;

                // Safety check for extremely large ranges
                const size_t MAX_STRING_SLICE = 10000000;  // 10 million characters max
                size_t collected = 0;

                // Pre-allocate if we can estimate size (for efficiency)
                if (r.increasing) {
                    size_t estimated = 0;
                    if (r.inclusive) {
                        estimated = (r.end >= r.start) ? ((r.end - r.start) / r.step + 1) : 0;
                    } else {
                        estimated = (r.end > r.start) ? ((r.end - r.start - 1) / r.step + 1) : 0;
                    }
                    if (estimated > 0 && estimated < MAX_STRING_SLICE) {
                        result.reserve(std::min(estimated, s.size()));
                    }
                }

                while (r.hasNext() && collected < MAX_STRING_SLICE) {
                    int index = r.next();

                    // Skip negative indices
                    if (index < 0) continue;

                    // Skip indices beyond string bounds
                    if ((size_t)index >= s.size()) {
                        // If we're past the end and moving forward, stop early
                        if (r.increasing) break;
                        continue;
                    }

                    // Add character at this index
                    result += s[(size_t)index];
                    collected++;
                }

                // Check if we hit the safety limit
                if (collected >= MAX_STRING_SLICE && r.hasNext()) {
                    throw std::runtime_error(
                        "RangeError at " + idx->token.loc.to_string() +
                        "\nString slice exceeded maximum size of 10,000,000 characters." +
                        "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                }

                return Value{result};
            }

            // Case 2: Single numeric index (existing behavior)
            long long rawIndex = static_cast<long long>(to_number(indexVal, idx->token));
            if (rawIndex < 0 || (size_t)rawIndex >= s.size()) {
                return std::monostate{};
            }
            return Value{std::string(1, s[(size_t)rawIndex])};
        }

        // Objects: use stringified index as key, go through unified getter (privacy/getter enforced)
        if (std::holds_alternative<ObjectPtr>(objVal)) {
            ObjectPtr op = std::get<ObjectPtr>(objVal);
            if (!op) return std::monostate{};
            std::string key = to_property_key(indexVal, idx->token);
            return get_object_property(op, key, env, idx->token);
        }

        throw std::runtime_error(
            "TypeError at " + idx->token.loc.to_string() +
            "\nAttempted to index a non-array/non-string/non-object value." +
            "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
    }

    if (auto u = dynamic_cast<UnaryExpressionNode*>(expr)) {
        Value operand = evaluate_expression(u->operand.get(), env);

        if (u->op == "!" || u->op == "si") {
            return Value{!to_bool(operand)};
        }

        if (u->op == "-") {
            // ensure to_number receives token for accurate traced errors
            return Value{-to_number(operand, u->token)};
        }

        if (u->op == "~") {
            int32_t val = to_int32(to_number(operand, u->token));
            return Value{static_cast<double>(~val)};
        }
        // new: 'aina' unary operator -> returns runtime type name string (same semantics as obj.aina)
        if (u->op == "aina") {
            return type_name(operand);
        }

        throw std::runtime_error(
            "SyntaxError at " + u->token.loc.to_string() +
            "\nUnknown unary operator '" + u->op + "'." +
            "\n --> Traced at:\n" + u->token.loc.get_line_trace());
    }

    if (auto b = dynamic_cast<BinaryExpressionNode*>(expr)) {
        // --- handle ++ / -- and += / -= and *= as side-effecting ops ---
        if (b->token.type == TokenType::INCREMENT ||
            b->token.type == TokenType::DECREMENT ||
            b->token.type == TokenType::PLUS_ASSIGN ||
            b->token.type == TokenType::MINUS_ASSIGN ||
            b->token.type == TokenType::TIMES_ASSIGN) {
            // Case A: left is an identifier (x++, x += ...)
            if (auto leftIdent = dynamic_cast<IdentifierNode*>(b->left.get())) {
                // Search up the environment chain to update the defining environment
                EnvPtr walk = env;
                while (walk) {
                    auto it = walk->values.find(leftIdent->name);
                    if (it != walk->values.end()) {
                        if (it->second.is_constant) {
                            throw std::runtime_error(
                                "TypeError at " + b->token.loc.to_string() +
                                "\nCannot assign to constant '" + leftIdent->name + "'." +
                                "\n --> Traced at:\n" + b->token.loc.get_line_trace());
                        }

                        // IMPORTANT: avoid converting stored value to number until we know
                        // we're on the numeric path. For += we must first check string concat.
                        if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                            double oldv = to_number(it->second.value, b->token);
                            double newv = oldv;
                            if (b->token.type == TokenType::INCREMENT)
                                newv = oldv + 1.0;
                            else
                                newv = oldv - 1.0;
                            it->second.value = Value{newv};
                            return Value{newv};
                        }

                        if (b->token.type == TokenType::PLUS_ASSIGN) {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            // string concat if either side is string
                            if (std::holds_alternative<std::string>(it->second.value) ||
                                std::holds_alternative<std::string>(rightVal)) {
                                std::string out = to_string_value(it->second.value) + to_string_value(rightVal);
                                it->second.value = Value{out};
                                return Value{out};
                            }
                            // numeric path
                            double oldv = to_number(it->second.value, b->token);
                            double rv = to_number(rightVal, b->token);
                            double newv = oldv + rv;
                            it->second.value = Value{newv};
                            return Value{newv};
                        }

                        // other compound ops: numeric-only
                        {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            double oldv = to_number(it->second.value, b->token);
                            double rv = to_number(rightVal, b->token);
                            double newv = oldv;
                            if (b->token.type == TokenType::MINUS_ASSIGN)
                                newv = oldv - rv;
                            else if (b->token.type == TokenType::TIMES_ASSIGN)
                                newv = oldv * rv;
                            else
                                newv = oldv + rv;  // fallback numeric
                            it->second.value = Value{newv};
                            return Value{newv};
                        }
                    }
                    walk = walk->parent;
                }

                // Not found in any parent -> create in current env (same behavior as assignment).
                // For += with string RHS create a string, otherwise create numeric start value.
                if (b->token.type == TokenType::PLUS_ASSIGN) {
                    Value rightVal = evaluate_expression(b->right.get(), env);
                    Environment::Variable var;
                    if (std::holds_alternative<std::string>(rightVal)) {
                        var.value = to_string_value(rightVal);
                    } else {
                        var.value = Value{to_number(rightVal, b->token)};
                    }
                    var.is_constant = false;
                    env->set(leftIdent->name, var);
                    return var.value;
                }

                // compute start value (treat missing as 0 for numeric ops)
                double start = 0.0;
                if (b->token.type == TokenType::INCREMENT)
                    start = 1.0;
                else if (b->token.type == TokenType::DECREMENT)
                    start = -1.0;
                else if (b->token.type == TokenType::TIMES_ASSIGN) {
                    // undefined treated as 0, 0 * rhs => 0
                    start = 0.0;
                } else {
                    Value rightVal = evaluate_expression(b->right.get(), env);
                    double rv = to_number(rightVal, b->token);
                    start = (b->token.type == TokenType::MINUS_ASSIGN) ? -rv : rv;
                }
                Environment::Variable var;
                var.value = start;
                var.is_constant = false;
                env->set(leftIdent->name, var);
                return Value{start};
            }

            // Case B: left is an index expression (arr[idx]++ or arr[idx] += ...)
            if (auto idx = dynamic_cast<IndexExpressionNode*>(b->left.get())) {
                // Evaluate the object expression (this will return ArrayPtr or ObjectPtr)
                Value objVal = evaluate_expression(idx->object.get(), env);
                Value indexVal = evaluate_expression(idx->index.get(), env);

                // --- ARRAY PATH ---
                if (std::holds_alternative<ArrayPtr>(objVal)) {
                    ArrayPtr arr = std::get<ArrayPtr>(objVal);
                    if (!arr) {
                        throw std::runtime_error(
                            "TypeError at " + b->token.loc.to_string() +
                            "\nCannot assign into null array." +
                            "\n --> Traced at:\n" + b->token.loc.get_line_trace());
                    }

                    long long rawIndex = static_cast<long long>(to_number(indexVal, idx->token));
                    if (rawIndex < 0) {
                        throw std::runtime_error(
                            "RangeError at " + idx->token.loc.to_string() +
                            "\nNegative array index not supported." +
                            "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                    }
                    size_t uidx = static_cast<size_t>(rawIndex);
                    if (uidx >= arr->elements.size()) arr->elements.resize(uidx + 1);

                    // Avoid converting element to number until numeric path chosen.
                    if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                        double oldv = to_number(arr->elements[uidx], b->token);
                        double newv = oldv;
                        if (b->token.type == TokenType::INCREMENT)
                            newv = oldv + 1.0;
                        else
                            newv = oldv - 1.0;
                        arr->elements[uidx] = Value{newv};
                        return Value{newv};
                    }

                    if (b->token.type == TokenType::PLUS_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        if (std::holds_alternative<std::string>(arr->elements[uidx]) ||
                            std::holds_alternative<std::string>(rightVal)) {
                            std::string out = to_string_value(arr->elements[uidx]) + to_string_value(rightVal);
                            arr->elements[uidx] = Value{out};
                            return Value{out};
                        }
                        double oldv = to_number(arr->elements[uidx], b->token);
                        double rv = to_number(rightVal, b->token);
                        double newv = oldv + rv;
                        arr->elements[uidx] = Value{newv};
                        return Value{newv};
                    }

                    // other compound ops: numeric-only
                    {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double oldv = to_number(arr->elements[uidx], b->token);
                        double rv = to_number(rightVal, b->token);
                        double newv = oldv;
                        if (b->token.type == TokenType::MINUS_ASSIGN)
                            newv = oldv - rv;
                        else if (b->token.type == TokenType::TIMES_ASSIGN)
                            newv = oldv * rv;
                        else
                            newv = oldv + rv;
                        arr->elements[uidx] = Value{newv};
                        return Value{newv};
                    }
                }

                // --- OBJECT PATH (obj[key]++ / obj[key] += v / obj[key] *= v) ---
                if (std::holds_alternative<ObjectPtr>(objVal)) {
                    ObjectPtr op = std::get<ObjectPtr>(objVal);
                    if (!op) {
                        throw std::runtime_error(
                            "TypeError at " + b->token.loc.to_string() +
                            "\nCannot operate on null object." +
                            "\n --> Traced at:\n" + b->token.loc.get_line_trace());
                    }

                    // convert indexVal -> property key string
                    std::string prop = to_string_value(indexVal);

                    // Read the current value using unified getter (handles getters, privacy checks for reads).
                    // If property doesn't exist get_object_property returns undefined (std::monostate).
                    Value curVal = get_object_property(op, prop, env, idx->token);

                    // Handle INCREMENT / DECREMENT
                    if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                        double oldv = to_number(curVal, b->token);
                        double newv = (b->token.type == TokenType::INCREMENT) ? oldv + 1.0 : oldv - 1.0;
                        // Write via the centralized setter so freeze/permissions are enforced.
                        set_object_property(op, prop, Value{newv}, env, idx->token);
                        return Value{newv};
                    }

                    // Handle += (string concat if either side string)
                    if (b->token.type == TokenType::PLUS_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        if (std::holds_alternative<std::string>(curVal) || std::holds_alternative<std::string>(rightVal)) {
                            std::string out = to_string_value(curVal) + to_string_value(rightVal);
                            set_object_property(op, prop, Value{out}, env, idx->token);
                            return Value{out};
                        }
                        double oldn = to_number(curVal, b->token);
                        double rv = to_number(rightVal, b->token);
                        double newv = oldn + rv;
                        set_object_property(op, prop, Value{newv}, env, idx->token);
                        return Value{newv};
                    }

                    // Other compound numeric ops: -=, *= etc.
                    {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double oldn = to_number(curVal, b->token);
                        double rv = to_number(rightVal, b->token);
                        double newv = oldn;
                        if (b->token.type == TokenType::MINUS_ASSIGN)
                            newv = oldn - rv;
                        else if (b->token.type == TokenType::TIMES_ASSIGN)
                            newv = oldn * rv;
                        else
                            newv = oldn + rv;  // fallback numeric
                        set_object_property(op, prop, Value{newv}, env, idx->token);
                        return Value{newv};
                    }
                }

                // Fallback: not array nor object
                throw std::runtime_error(
                    "TypeError at " + b->token.loc.to_string() +
                    "\nIndexed target is not an array or object." +
                    "\n --> Traced at:\n" + b->token.loc.get_line_trace());
            }
        }
        // --- Normal binary evaluation path (short-circuit logicals returning operands) ---
        Value left = evaluate_expression(b->left.get(), env);
        const std::string& op = b->op;

        // Logical AND: short-circuit; return operand (left or right) instead of boolean.
        if (op == "&&" || op == "na") {
            // If left is falsy -> return left (no RHS evaluation).
            if (!to_bool(left)) return left;
            // Otherwise evaluate RHS and return the RHS value (preserve identity).
            return evaluate_expression(b->right.get(), env);
        }

        if (op == "??") {
            // If left is null -> return left (no RHS evaluation).
            if (!std::holds_alternative<std::monostate>(left)) return left;
            // Otherwise evaluate RHS and return the RHS value (preserve identity).
            return evaluate_expression(b->right.get(), env);
        }

        // Logical OR: short-circuit; return operand (left or right).
        if (op == "||" || op == "au") {
            // If left is truthy -> return left (no RHS evaluation).
            if (to_bool(left)) return left;
            // Otherwise evaluate RHS and return it.
            return evaluate_expression(b->right.get(), env);
        }

        // For all other binary operators evaluate RHS now (both operands required)
        Value right = evaluate_expression(b->right.get(), env);

        // DateTime binary operations - ONLY comparisons allowed
        if (std::holds_alternative<DateTimePtr>(left) || std::holds_alternative<DateTimePtr>(right)) {
            DateTimePtr dtLeft = std::holds_alternative<DateTimePtr>(left) ? std::get<DateTimePtr>(left) : nullptr;
            DateTimePtr dtRight = std::holds_alternative<DateTimePtr>(right) ? std::get<DateTimePtr>(right) : nullptr;

            // Comparison operators (both operands must be DateTime)
            if (dtLeft && dtRight) {
                if (op == "==" || op == "sawa") {
                    return Value{dtLeft->epochNanoseconds == dtRight->epochNanoseconds};
                }
                if (op == "!=" || op == "sisawa") {
                    return Value{dtLeft->epochNanoseconds != dtRight->epochNanoseconds};
                }
                if (op == "===") {
                    return Value{dtLeft->epochNanoseconds == dtRight->epochNanoseconds};
                }
                if (op == "!==") {
                    return Value{dtLeft->epochNanoseconds != dtRight->epochNanoseconds};
                }
                if (op == "<") {
                    return Value{dtLeft->epochNanoseconds < dtRight->epochNanoseconds};
                }
                if (op == ">") {
                    return Value{dtLeft->epochNanoseconds > dtRight->epochNanoseconds};
                }
                if (op == "<=") {
                    return Value{dtLeft->epochNanoseconds <= dtRight->epochNanoseconds};
                }
                if (op == ">=") {
                    return Value{dtLeft->epochNanoseconds >= dtRight->epochNanoseconds};
                }
            }

            // Any other operator with DateTime is an error
            throw SwaziError("TypeError",
                "DateTime values only support comparison operators (==, !=, <, >, <=, >=). "
                "Use instance methods like .addDays(), .addMillis() for arithmetic.",
                b->token.loc);
        }

        if (op == "+") {
            // --- string concatenation if either is string ---
            if (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right)) {
                return Value{to_string_value(left) + to_string_value(right)};
            }

            // --- array concatenation if both are arrays ---
            if (std::holds_alternative<ArrayPtr>(left) && std::holds_alternative<ArrayPtr>(right)) {
                ArrayPtr a1 = std::get<ArrayPtr>(left);
                ArrayPtr a2 = std::get<ArrayPtr>(right);

                if (!a1 || !a2) {
                    throw std::runtime_error(
                        "TypeError at " + b->token.loc.to_string() +
                        "\nCannot concatenate null arrays." +
                        "\n --> Traced at:\n" + b->token.loc.get_line_trace());
                }

                auto result = std::make_shared<ArrayValue>();
                result->elements.reserve(a1->elements.size() + a2->elements.size());
                result->elements.insert(result->elements.end(), a1->elements.begin(), a1->elements.end());
                result->elements.insert(result->elements.end(), a2->elements.begin(), a2->elements.end());

                return Value{result};
            }

            // --- fallback numeric addition ---
            return Value{to_number(left, b->token) + to_number(right, b->token)};
        }

        if (op == "-") return Value{to_number(left, b->token) - to_number(right, b->token)};
        if (op == "*") return Value{to_number(left, b->token) * to_number(right, b->token)};
        if (op == "/") {
            double r = to_number(right, b->token);
            if (r == 0.0) throw std::runtime_error(
                "MathError at " + b->token.loc.to_string() +
                "\nDivision by zero." +
                "\n --> Traced at:\n" + b->token.loc.get_line_trace());
            return Value{to_number(left, b->token) / r};
        }
        if (op == "%") {
            double r = to_number(right, b->token);
            if (r == 0.0) throw std::runtime_error(
                "MathError at " + b->token.loc.to_string() +
                "\nModulo by zero." +
                "\n --> Traced at:\n" + b->token.loc.get_line_trace());
            return Value{std::fmod(to_number(left, b->token), r)};
        }
        if (op == "**") return Value{std::pow(to_number(left, b->token), to_number(right, b->token))};

        // Bitwise AND, OR, XOR MUST use signed conversion (ToInt32).
        if (op == "&") {
            int32_t l = to_int32(to_number(left, b->token));
            int32_t r = to_int32(to_number(right, b->token));
            return Value{static_cast<double>(l & r)};
        }

        if (op == "|") {
            int32_t l = to_int32(to_number(left, b->token));
            int32_t r = to_int32(to_number(right, b->token));
            return Value{static_cast<double>(l | r)};
        }

        if (op == "^") {
            int32_t l = to_int32(to_number(left, b->token));
            int32_t r = to_int32(to_number(right, b->token));
            return Value{static_cast<double>(l ^ r)};
        }

        // Left shift (<<) - Uses ToInt32, but result is ToUint32.
        if (op == "<<") {
            int32_t l = to_int32(to_number(left, b->token));
            uint32_t r = static_cast<uint32_t>(to_number(right, b->token)) & 0x1F;
            int32_t result_int32 = l << r;
            // Final result must be cast back to unsigned for JS ToUint32 result.
            return Value{static_cast<double>(static_cast<uint32_t>(result_int32))};
        }

        // Right shift (>>) - Uses ToInt32, resulting in Arithmetic Shift.
        if (op == ">>") {
            int32_t l = to_int32(to_number(left, b->token));
            uint32_t r = static_cast<uint32_t>(to_number(right, b->token)) & 0x1F;
            // C++'s >> on int32_t performs Arithmetic Shift.
            return Value{static_cast<double>(l >> r)};
        }

        if (op == ">>>") {
            uint32_t l = to_uint32(to_number(left, b->token));
            uint32_t r = static_cast<uint32_t>(to_number(right, b->token)) & 0x1F;
            return Value{static_cast<double>(l >> r)};  // Performs logical shift
        }

        if (op == "===") {
            return Value(to_bool(Value(is_strict_equal(left, right))));
        }
        if (op == "!==") {
            return Value(!is_strict_equal(left, right));
        }
        if (op == "==" || op == "sawa") {
            return Value{is_equal(left, right)};
        }
        if (op == "!=" || op == "sisawa") {
            return Value{!is_equal(left, right)};
        }

        bool bothStrings = std::holds_alternative<std::string>(left) && std::holds_alternative<std::string>(right);
        if (bothStrings) {
            const std::string& ls = std::get<std::string>(left);
            const std::string& rs = std::get<std::string>(right);

            if (op == ">") return Value{ls > rs};
            if (op == "<") return Value{ls < rs};
            if (op == ">=") return Value{ls >= rs};
            if (op == "<=") return Value{ls <= rs};
        } else {
            // Numeric fallback (preserve existing behavior for mixed or non-string values).
            double ln = to_number(left, b->token);
            double rn = to_number(right, b->token);

            if (op == ">") return Value{ln > rn};
            if (op == "<") return Value{ln < rn};
            if (op == ">=") return Value{ln >= rn};
            if (op == "<=") return Value{ln <= rn};
        }

        throw std::runtime_error(
            "SyntaxError at " + b->token.loc.to_string() +
            "\nUnknown binary operator '" + op + "'." +
            "\n --> Traced at:\n" + b->token.loc.get_line_trace());
    }

    if (auto call = dynamic_cast<CallExpressionNode*>(expr)) {
        // --- special-case: obj.instanceOf(Class) member-call ---
        if (auto mem = dynamic_cast<MemberExpressionNode*>(call->callee.get())) {
            if (mem->property == "instanceOf") {
                // evaluate the object expression (left side of the member)
                Value objVal = evaluate_expression(mem->object.get(), env);

                // require at least one argument
                if (call->arguments.empty()) {
                    throw SwaziError(
                        "TypeError", "obj.instanceOf expects a class as its first argument.", call->token.loc);
                }

                // evaluate the first argument (expected to be a Class)
                Value arg0 = evaluate_expression(call->arguments[0].get(), env);

                if (!std::holds_alternative<ClassPtr>(arg0)) {
                    throw SwaziError(
                        "TypeError",
                        "instanceOf expects a class value as its first argument.", call->arguments[0]->token.loc);
                }

                ClassPtr targetClass = std::get<ClassPtr>(arg0);
                if (!targetClass) return Value{false};

                // If left side is not an object, return false (not an instance)
                if (!std::holds_alternative<ObjectPtr>(objVal)) {
                    return Value{false};
                }

                ObjectPtr inst = std::get<ObjectPtr>(objVal);
                if (!inst) return Value{false};

                // Look for __class__ link on the instance
                auto it = inst->properties.find("__class__");
                if (it == inst->properties.end()) return Value{false};
                const PropertyDescriptor& pd = it->second;
                if (!std::holds_alternative<ClassPtr>(pd.value)) return Value{false};
                ClassPtr objClass = std::get<ClassPtr>(pd.value);

                // Strict identity check (no subclass traversal)
                bool result = (objClass == targetClass);
                return Value{result};
            }
        }

        // --- fallback: original call handling (unchanged) ---
        auto eval_args = [&](std::vector<Value>& out) {
            out.clear();
            out.reserve(call->arguments.size());
            for (auto& argPtr : call->arguments) {
                if (!argPtr) {
                    out.push_back(std::monostate{});
                    continue;
                }

                if (auto spread = dynamic_cast<SpreadElementNode*>(argPtr.get())) {
                    if (!spread->argument) {
                        throw std::runtime_error(
                            "SyntaxError at " + spread->token.loc.to_string() + "\n" +
                            "Missing expression after spread operator" +
                            "\n --> Traced at:\n" +
                            spread->token.loc.get_line_trace());
                    }
                    Value v = evaluate_expression(spread->argument.get(), env);

                    if (std::holds_alternative<ArrayPtr>(v)) {
                        ArrayPtr src = std::get<ArrayPtr>(v);
                        if (src) {
                            for (auto& e : src->elements) out.push_back(e);
                        }
                        continue;
                    }

                    if (std::holds_alternative<std::string>(v)) {
                        std::string s = std::get<std::string>(v);
                        for (char c : s) out.push_back(Value{
                            std::string(1, c)});
                        continue;
                    }

                    throw SwaziError(
                        "TypeError",
                        "Invalid spread operation — expected iterable value (array or string).",
                        spread->token.loc);
                }

                out.push_back(evaluate_expression(argPtr.get(), env));
            }
        };

        // Evaluate callee first (receiver/computed parts inside callee are handled
        // by their own node logic; those nodes must also short-circuit when optional).
        Value calleeVal = evaluate_expression(call->callee.get(), env);

        // If this was an optional call and the callee is nullish, short-circuit without
        // evaluating call arguments.
        if (call->is_optional && is_nullish(calleeVal)) {
            return std::monostate{};
        }

        if (std::holds_alternative<ObjectPtr>(calleeVal)) {
            ObjectPtr obj = std::get<ObjectPtr>(calleeVal);
            if (obj) {
                auto proxy_it = obj->properties.find("__proxy__");
                if (proxy_it != obj->properties.end() &&
                    std::holds_alternative<ProxyPtr>(proxy_it->second.value)) {
                    ProxyPtr proxy = std::get<ProxyPtr>(proxy_it->second.value);
                    FunctionPtr call_trap = get_handler_method(proxy->handler, "call", call->token);

                    if (call_trap) {
                        // Evaluate args first
                        std::vector<Value> args;
                        eval_args(args);

                        // Convert args to array for trap
                        auto args_arr = std::make_shared<ArrayValue>();
                        args_arr->elements = args;

                        // Call: handler.call(target, args)
                        std::vector<Value> trap_args = {
                            Value{proxy->target},
                            Value{args_arr}};
                        return call_function(call_trap, trap_args, env, call->token);
                    }
                }
            }
        }

        if (std::holds_alternative<FunctionPtr>(calleeVal)) {
            std::vector<Value> args;
            eval_args(args);

            // Compute an effective token to pass to the callee:
            // - If the AST call node has a token, use it.
            // - Else if the FunctionValue has a token with a filename, use that.
            // - Else synthesize a builtin token using the function name so errors are informative.
            Token effectiveTok = call->token;
            FunctionPtr fn = std::get<FunctionPtr>(calleeVal);
            if (effectiveTok.loc.filename.empty() && fn) {
                if (!fn->token.loc.filename.empty()) {
                    effectiveTok = fn->token;
                } else {
                    // synthesize a helpful builtin token (line/col set to 1)
                    effectiveTok.loc.filename = std::string("<builtin:") + (fn->name.empty() ? "<anonymous>" : fn->name) + ">";
                    effectiveTok.loc.line = 1;
                    effectiveTok.loc.col = 1;
                }
            }

            return call_function(fn, args, env, effectiveTok);
        }
        throw SwaziError(
            "TypeError",
            "Attempted to call a non-function value.",
            call->token.loc);
    }

    if (auto t = dynamic_cast<TernaryExpressionNode*>(expr)) {
        // Evaluate condition first
        Value condVal = evaluate_expression(t->condition.get(), env);
        if (to_bool(condVal)) {
            // condition true → evaluate thenExpr
            return evaluate_expression(t->thenExpr.get(), env);
        } else {
            // condition false → evaluate elseExpr
            return evaluate_expression(t->elseExpr.get(), env);
        }
    }

    if (auto ln = dynamic_cast<LambdaNode*>(expr)) {
        // Convert LambdaNode to a callable FunctionValue
        auto fnDecl = std::make_shared<FunctionDeclarationNode>();
        fnDecl->is_async = ln->is_async;

        // clone params from ln->params
        fnDecl->parameters.reserve(ln->params.size());
        for (const auto& pp : ln->params) {
            if (pp)
                fnDecl->parameters.push_back(pp->clone());
            else
                fnDecl->parameters.push_back(nullptr);
        }

        if (ln->isBlock) {
            // Clone each statement so we do not mutate the original AST node (ln)
            fnDecl->body.reserve(ln->blockBody.size());
            for (const auto& sptr : ln->blockBody) {
                fnDecl->body.push_back(sptr ? sptr->clone() : nullptr);
            }
        } else {
            // expression-lambda: wrap exprBody into a ReturnStatementNode for consistent execution
            auto retStmt = std::make_unique<ReturnStatementNode>();
            retStmt->value = ln->exprBody ? ln->exprBody->clone() : nullptr;
            fnDecl->body.push_back(std::move(retStmt));
        }

        // Construct FunctionValue from the persisted declaration. FunctionValue ctor clones again as needed.
        auto fn = std::make_shared<FunctionValue>(
            std::string("<lambda>"),
            fnDecl->parameters,
            fnDecl,
            env,
            ln->token);

        return fn;
    }

    if (auto ne = dynamic_cast<NewExpressionNode*>(expr)) {
        // Evaluate callee to find class (callee is usually an IdentifierNode)
        Value calleeVal = evaluate_expression(ne->callee.get(), env);
        if (!std::holds_alternative<ClassPtr>(calleeVal)) {
            throw SwaziError(
                "TypeError",
                "Attempted to instantiate a non-class value.",
                ne->token.loc);
        }
        ClassPtr cls = std::get<ClassPtr>(calleeVal);

        // create an object instance
        auto instance = std::make_shared<ObjectValue>();

        // attach a link to its class so futa / reflection can find it
        PropertyDescriptor classLink;
        classLink.value = cls;
        classLink.is_private = true;
        classLink.token = ne->token;
        instance->properties["__class__"] = std::move(classLink);

        // Evaluate and populate instance properties (non-static) in top-down class chain
        // We want super class initializers to run before derived.
        std::vector<ClassPtr> chain;
        for (ClassPtr walk = cls; walk; walk = walk->super) chain.push_back(walk);
        std::reverse(chain.begin(), chain.end());

        for (auto& c : chain) {
            if (!c || !c->body) continue;

            // Use the environment where the class was declared for evaluating instance initializers
            // and building method closures. If (for some reason) the defining_env is null, fall back
            // to the current evaluation env to preserve previous behavior.
            EnvPtr classEnv = c->defining_env ? c->defining_env : env;

            // properties
            for (auto& p : c->body->properties) {
                if (!p) continue;
                if (p->is_static) continue;
                // evaluate initializer in a child env where '$' is bound to instance so initializers can reference $
                auto initEnv = std::make_shared<Environment>(classEnv);
                Environment::Variable thisVar;
                thisVar.value = instance;
                thisVar.is_constant = false;
                initEnv->set("$", thisVar);
                Value initVal = std::monostate{};
                if (p->value) initVal = evaluate_expression(p->value.get(), initEnv);

                PropertyDescriptor pd;
                pd.value = initVal;
                pd.is_private = p->is_private;
                pd.is_locked = p->is_locked;
                pd.is_readonly = false;
                pd.token = p->token;
                instance->properties[p->name] = std::move(pd);
            }

            // methods (non-static)
            for (auto& m : c->body->methods) {
                if (!m) continue;
                if (m->is_static) continue;

                // Skip constructors and destructors: they must remain on the ClassValue (AST)
                // and be invoked by the instantiation/delete machinery (new/super/delete).
                // Materializing them onto the instance exposes them as callable instance props
                // (e.g. ob.Base(...)) which is incorrect.
                if (m->is_constructor || m->is_destructor) continue;

                // persist a FunctionDeclarationNode like other functions do
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = m->name;
                persisted->token = m->token;
                persisted->is_async = m->is_async;

                // clone parameter descriptors from ClassMethodNode::params
                persisted->parameters.reserve(m->params.size());
                for (const auto& pp : m->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // clone body statements
                persisted->body.reserve(m->body.size());
                for (const auto& s : m->body) persisted->body.push_back(s ? s->clone() : nullptr);

                // IMPORTANT: each instance method must have a closure where '$' is bound and whose parent
                // is the class's defining environment (so free identifiers captured by the method resolve
                // to the module where the class was defined).
                EnvPtr methodClosureParent = classEnv;
                EnvPtr methodClosure = std::make_shared<Environment>(methodClosureParent);
                Environment::Variable thisVar;
                thisVar.value = instance;
                thisVar.is_constant = true;  // treat $ as constant inside methods
                methodClosure->set("$", thisVar);

                // create the FunctionValue using the methodClosure (so call_function finds '$')
                auto fn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, methodClosure, persisted->token);

                PropertyDescriptor pd;
                pd.value = fn;
                pd.is_private = m->is_private;
                pd.is_locked = m->is_locked;
                pd.is_readonly = m->is_getter;
                pd.token = m->token;
                instance->properties[m->name] = std::move(pd);
            }
        }

        // Call constructor if any on the class itself (derived class constructor should be found on cls->body->methods with is_constructor)
        if (cls->body) {
            // Find constructor method in the class (only in this class, not searching super automatically)
            ClassMethodNode* ctorNode = nullptr;
            for (auto& m : cls->body->methods) {
                if (m && m->is_constructor) {
                    ctorNode = m.get();
                    break;
                }
            }

            if (ctorNode) {
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = ctorNode->name;
                persisted->token = ctorNode->token;
                persisted->is_async = ctorNode->is_async;

                // clone parameter descriptors from ctorNode->params
                persisted->parameters.reserve(ctorNode->params.size());
                for (const auto& pp : ctorNode->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // clone body statements
                persisted->body.reserve(ctorNode->body.size());
                for (const auto& stmt : ctorNode->body) {
                    persisted->body.push_back(stmt ? stmt->clone() : nullptr);
                }

                // Create FunctionValue for constructor with closure = env (the class-decl environment)
                EnvPtr ctorClosure = cls->defining_env ? cls->defining_env : env;
                auto constructorFn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, ctorClosure, persisted->token);

                // Evaluate call arguments
                std::vector<Value> ctorArgs;
                ctorArgs.reserve(ne->arguments.size());
                for (auto& a : ne->arguments) ctorArgs.push_back(evaluate_expression(a.get(), env));

                // Call constructor with the instance as receiver so $ is available in the call frame,
                // and set current_class_context so 'super(...)' works inside constructor.
                ClassPtr saved_ctx = current_class_context;
                current_class_context = cls;
                call_function_with_receiver(constructorFn, instance, ctorArgs, env, persisted->token);
                current_class_context = saved_ctx;
            }
        }

        // return the created instance as a Value
        return instance;
    }

    if (auto se = dynamic_cast<SuperExpressionNode*>(expr)) {
        if (!current_class_context) {
            throw SwaziError(
                "ContextError",
                "'super/supa(...)' may only be invoked inside a class constructor.",
                se->token.loc);
        }
        // find the instance from env via $
        EnvPtr walk = env;
        ObjectPtr receiver = nullptr;
        while (walk) {
            auto it = walk->values.find("$");
            if (it != walk->values.end()) {
                if (std::holds_alternative<ObjectPtr>(it->second.value)) {
                    receiver = std::get<ObjectPtr>(it->second.value);
                    break;
                }
            }
            walk = walk->parent;
        }
        if (!receiver) {
            throw SwaziError(
                "ReferenceError",
                "No '$' receiver found for 'super(...)' call — must be called within a class instance.",
                se->token.loc);
        }

        ClassPtr parent = current_class_context->super;
        if (!parent) return std::monostate{};  // no-op if no super

        // find parent constructor
        if (!parent->body) return std::monostate{};
        for (auto& m : parent->body->methods) {
            if (m && m->is_constructor) {
                // create FunctionValue from parent constructor and call with receiver
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = m->name;
                persisted->token = m->token;
                persisted->is_async = m->is_async;

                // clone parameters
                persisted->parameters.reserve(m->params.size());
                for (const auto& pp : m->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // clone body
                persisted->body.reserve(m->body.size());
                for (const auto& s : m->body) persisted->body.push_back(s ? s->clone() : nullptr);

                EnvPtr parentCtorClosure = parent->defining_env ? parent->defining_env : env;
                auto parentCtor = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, parentCtorClosure, persisted->token);

                std::vector<Value> args;
                args.reserve(se->arguments.size());
                for (auto& a : se->arguments) args.push_back(evaluate_expression(a.get(), env));

                // IMPORTANT: ensure nested super(...) calls inside the parent constructor
                // resolve to the parent's super. Temporarily set current_class_context to parent.
                ClassPtr saved_ctx = current_class_context;
                current_class_context = parent;
                try {
                    Value res = call_function_with_receiver(parentCtor, receiver, args, env, m->token);
                    current_class_context = saved_ctx;
                    return res;
                } catch (...) {
                    current_class_context = saved_ctx;
                    throw;
                }
            }
        }
        return std::monostate{};
    }

    if (auto de = dynamic_cast<DeleteExpressionNode*>(expr)) {
        Value v = evaluate_expression(de->target.get(), env);
        if (!std::holds_alternative<ObjectPtr>(v)) return std::monostate{};
        ObjectPtr obj = std::get<ObjectPtr>(v);

        // collect evaluated args
        std::vector<Value> args;
        args.reserve(de->arguments.size());
        for (auto& aexpr : de->arguments) {
            if (!aexpr)
                args.push_back(std::monostate{});
            else
                args.push_back(evaluate_expression(aexpr.get(), env));
        }

        // call destructor if class link exists
        auto it = obj->properties.find("__class__");
        if (it != obj->properties.end() && std::holds_alternative<ClassPtr>(it->second.value)) {
            ClassPtr cls = std::get<ClassPtr>(it->second.value);
            if (cls->body) {
                for (auto& m : cls->body->methods) {
                    if (m && m->is_destructor) {
                        auto persisted = std::make_shared<FunctionDeclarationNode>();
                        persisted->name = m->name;
                        persisted->token = m->token;
                        persisted->is_async = m->is_async;

                        // clone parameter descriptors from method node
                        persisted->parameters.reserve(m->params.size());
                        for (const auto& pp : m->params) {
                            if (pp)
                                persisted->parameters.push_back(pp->clone());
                            else
                                persisted->parameters.push_back(nullptr);
                        }

                        persisted->body.reserve(m->body.size());
                        for (const auto& s : m->body) persisted->body.push_back(s ? s->clone() : nullptr);

                        EnvPtr dtorClosure = cls->defining_env ? cls->defining_env : env;
                        auto dtorFn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, dtorClosure, persisted->token);

                        // forward evaluated args into destructor call
                        Value res = call_function_with_receiver(dtorFn, obj, args, env, m->token);
                        obj->properties.clear();
                        return res;
                    }
                }
            }
        }

        obj->properties.clear();
        return std::monostate{};
    }

    // Walrus operator: x ni <expr>
    if (auto ae = dynamic_cast<AssignmentExpressionNode*>(expr)) {
        // Evaluate the right-hand side expression
        Value result = evaluate_expression(ae->value.get(), env);

        // Create/update the variable in the current environment
        // Walrus always creates mutable variables (not constants)
        Environment::Variable var{result, false};
        env->set(ae->target_name, var);

        // Return the assigned value (so it can be used in conditions)
        return result;
    }

    throw SwaziError(
        "InternalError",
        "Unhandled expression node encountered in evaluator — this is likely a bug in the interpreter.",
        expr->token.loc  // if expr has a token; otherwise pass a default/fake location
    );
}