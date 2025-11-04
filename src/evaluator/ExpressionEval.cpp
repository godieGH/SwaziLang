#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"

Value Evaluator::evaluate_expression(ExpressionNode* expr, EnvPtr env) {
    if (!expr) return std::monostate{};

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
                return call_function(getter, {}, desc.token);
            }

            // Normal return of the value (from the class where it was found)
            return desc.value;
        }

        // --- Universal properties ---
        const std::string& prop = mem->property;

        // aina -> type name
        if (prop == "aina" || prop == "type") {
            std::string t = "unknown";  // default
            if (std::holds_alternative<std::monostate>(objVal)) t = "null";
            if (std::holds_alternative<double>(objVal))
                t = "namba";
            else if (std::holds_alternative<std::string>(objVal))
                t = "neno";
            else if (std::holds_alternative<bool>(objVal))
                t = "bool";
            else if (std::holds_alternative<ArrayPtr>(objVal))
                t = "orodha";
            else if (std::holds_alternative<FunctionPtr>(objVal))
                t = "kazi";
            else if (std::holds_alternative<ObjectPtr>(objVal))
                t = "object";
            else if (std::holds_alternative<ClassPtr>(objVal))
                t = "muundo";
            return Value{
                t};
        }

        // type-checking booleans
        if (prop == "ninamba") return Value{
            std::holds_alternative<double>(objVal)};
        if (prop == "nineno") return Value{
            std::holds_alternative<std::string>(objVal)};
        if (prop == "nibool") return Value{
            std::holds_alternative<bool>(objVal)};
        if (prop == "niorodha") return Value{
            std::holds_alternative<ArrayPtr>(objVal)};
        if (prop == "nikazi") return Value{
            std::holds_alternative<FunctionPtr>(objVal)};
        if (prop == "niobject") return Value{
            std::holds_alternative<ObjectPtr>(objVal)};

        // String property 'herufi' (length)
        if (std::holds_alternative<std::string>(objVal) && mem->property == "herufi") {
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
            if (prop == "tafuta") {
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

            // No matching string property -> fall through to unknown property error below
        }

        // --- Number methods & properties (place this after string methods, before array methods) ---
        if (std::holds_alternative<double>(objVal)) {
            double num = std::get<double>(objVal);
            const std::string& prop = mem->property;

            // ---------- Properties ----------
            if (prop == "siSahihi" || prop == "isNaN") {
                return Value{std::isnan(num)};
            }
            if (prop == "isInf") {
                return Value{!std::isfinite(num)};
            }
            if (prop == "niInt" || prop == "isInt") {
                return Value{std::isfinite(num) && std::floor(num) == num};
            }
            if (prop == "niDesimali" || prop == "isDecimal") {
                return Value{std::isfinite(num) && std::floor(num) != num};
            }
            if (prop == "niChanya") {
                return Value{num > 0};
            }
            if (prop == "niHasi") {
                return Value{num < 0};
            }

            // boolean "is" properties: odd, even, prime
            if (prop == "niWitiri" || prop == "niShufwa" || prop == "niTasa") {
                if (!std::isfinite(num) || std::floor(num) != num) {
                    return Value{false};
                }
                if (num > static_cast<double>(LLONG_MAX) || num < static_cast<double>(LLONG_MIN)) {
                    return Value{false};
                }

                long long n = static_cast<long long>(std::llround(num));

                if (prop == "niWitiri") {
                    return Value{(n % 2) != 0};
                }
                if (prop == "niShufwa") {
                    return Value{(n % 2) == 0};
                }
                if (prop == "niTasa") {
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
            if (mem->property == "idadi") {
                return Value{static_cast<double>(arr ? arr->elements.size() : 0)};
            }

            const std::string& prop = mem->property;

            // Recognized array method names
            if (prop == "join" || prop == "reduce" || prop == "filter" || prop == "map" ||
                prop == "slice" || prop == "splice" || prop == "includes" || prop == "sort" ||
                prop == "reverse" || prop == "extend" || prop == "unshift" || prop == "insert" ||
                prop == "shift" || prop == "removeAll" || prop == "pop" || prop == "push" ||
                prop == "urefu" || prop == "indexOf" || prop == "indexYa" || prop == "tafutaIndex" ||
                prop == "ongeza" || prop == "toa" || prop == "ondoa" || prop == "ondoaMwanzo" ||
                prop == "ongezaMwanzo" || prop == "ingiza" || prop == "slesi" || prop == "panua" ||
                prop == "badili" || prop == "tafuta" || prop == "kuna" || prop == "panga" ||
                prop == "geuza" || prop == "futa" || prop == "chambua" || prop == "punguza" ||
                prop == "unganisha" || prop == "ondoaZote" || prop == "pachika" || prop == "kwaKila" || prop == "forEach") {
                auto native_impl = [this, arr, prop](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (!arr) return std::monostate{};

                    // urefu() -> returns array length (same as .idadi property)
                    if (prop == "urefu") {
                        return Value{static_cast<double>(arr->elements.size())};
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

                    // clear: futa()
                    if (prop == "futa") {
                        arr->elements.clear();
                        return std::monostate{};
                    }

                    // extend: panua(otherArray)
                    if (prop == "panua" || prop == "extend") {
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
                                Value res = call_function(cmp, {A, B}, token);
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
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);
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
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);
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
                            Value res = call_function(mapper, {arr->elements[i], Value{static_cast<double>(i)}}, token);
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
                            call_function(fn, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);
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
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);
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
                            acc = call_function(reducer, {acc, arr->elements[i], Value{static_cast<double>(i)}}, token);
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

                    return std::monostate{};
                };

                auto fn = std::make_shared<FunctionValue>(std::string("native:array.") + prop, native_impl, env, mem->token);
                return Value{fn};
            }
        }

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
            return get_object_property(op, mem->property, env);
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

        // Array indexing uses numeric interpretation of indexVal
        if (std::holds_alternative<ArrayPtr>(objVal)) {
            ArrayPtr arr = std::get<ArrayPtr>(objVal);
            if (!arr) return std::monostate{};

            long long rawIndex = static_cast<long long>(to_number(indexVal, idx->token));
            if (rawIndex < 0 || (size_t)rawIndex >= arr->elements.size()) {
                return std::monostate{};
            }
            return arr->elements[(size_t)rawIndex];
        }

        // String indexing: return single-char string (optional)
        if (std::holds_alternative<std::string>(objVal)) {
            std::string s = std::get<std::string>(objVal);
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
            std::string key = to_string_value(indexVal);
            return get_object_property(op, key, env);
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

        // new: 'aina' unary operator -> returns runtime type name string (same semantics as obj.aina)
        if (u->op == "aina") {
            std::string t = "unknown";
            if (std::holds_alternative<std::monostate>(operand))
                t = "null";
            else if (std::holds_alternative<double>(operand))
                t = "namba";
            else if (std::holds_alternative<std::string>(operand))
                t = "neno";
            else if (std::holds_alternative<bool>(operand))
                t = "bool";
            else if (std::holds_alternative<ArrayPtr>(operand))
                t = "orodha";
            else if (std::holds_alternative<FunctionPtr>(operand))
                t = "kazi";
            else if (std::holds_alternative<ObjectPtr>(operand))
                t = "object";
            else if (std::holds_alternative<ClassPtr>(operand))
                t = "muundo";
            return Value{t};
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

                    auto it = op->properties.find(prop);

                    // If property exists, enforce privacy/read-only rules
                    if (it != op->properties.end()) {
                        if (it->second.is_private) {
                            bool allowed = false;
                            if (env) {
                                if (env->has("$")) {
                                    Value thisVal = env->get("$").value;
                                    if (std::holds_alternative<ObjectPtr>(thisVal) && std::get<ObjectPtr>(thisVal) == op) allowed = true;
                                }
                                if (!allowed && env->has("$this")) {
                                    Value thisVal = env->get("$this").value;
                                    if (std::holds_alternative<ObjectPtr>(thisVal) && std::get<ObjectPtr>(thisVal) == op) allowed = true;
                                }
                            }
                            if (!allowed) {
                                throw std::runtime_error(
                                    "PermissionError at " + idx->token.loc.to_string() +
                                    "\nCannot assign to private property '" + prop + "' from outside the owning object." +
                                    "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                            }
                        }

                        if (it->second.is_readonly) {
                            throw std::runtime_error(
                                "TypeError at " + idx->token.loc.to_string() +
                                "\nCannot assign to read-only property '" + prop + "'." +
                                "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                        }

                        // Avoid converting to number until numeric path is chosen.
                        if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                            double oldv = to_number(it->second.value, b->token);
                            double newv = oldv;
                            if (b->token.type == TokenType::INCREMENT)
                                newv = oldv + 1.0;
                            else
                                newv = oldv - 1.0;
                            it->second.value = Value{newv};
                            it->second.token = idx->token;
                            return Value{newv};
                        }

                        if (b->token.type == TokenType::PLUS_ASSIGN) {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            if (std::holds_alternative<std::string>(it->second.value) ||
                                std::holds_alternative<std::string>(rightVal)) {
                                std::string out = to_string_value(it->second.value) + to_string_value(rightVal);
                                it->second.value = Value{out};
                                it->second.token = idx->token;
                                return Value{out};
                            }
                            double oldv = to_number(it->second.value, b->token);
                            double rv = to_number(rightVal, b->token);
                            double newv = oldv + rv;
                            it->second.value = Value{newv};
                            it->second.token = idx->token;
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
                                newv = oldv + rv;
                            it->second.value = Value{newv};
                            it->second.token = idx->token;
                            return Value{newv};
                        }
                    }

                    // Property does not exist -> create public property:
                    // If += with string RHS create string property; otherwise create numeric property
                    if (b->token.type == TokenType::PLUS_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        PropertyDescriptor desc;
                        if (std::holds_alternative<std::string>(rightVal)) {
                            desc.value = Value{to_string_value(rightVal)};
                        } else {
                            desc.value = Value{to_number(rightVal, b->token)};
                        }
                        desc.is_private = false;
                        desc.is_readonly = false;
                        desc.token = idx->token;
                        op->properties[prop] = std::move(desc);
                        return op->properties[prop].value;
                    }

                    // Create numeric property starting from 0 and apply op
                    double oldv = 0.0;
                    double newv = oldv;
                    if (b->token.type == TokenType::INCREMENT)
                        newv = oldv + 1.0;
                    else if (b->token.type == TokenType::DECREMENT)
                        newv = oldv - 1.0;
                    else if (b->token.type == TokenType::TIMES_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double rv = to_number(rightVal, b->token);
                        newv = oldv * rv;
                    } else {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double rv = to_number(rightVal, b->token);
                        newv = oldv + ((b->token.type == TokenType::MINUS_ASSIGN) ? -rv : rv);
                    }

                    PropertyDescriptor desc;
                    desc.value = Value{newv};
                    desc.is_private = false;
                    desc.is_readonly = false;
                    desc.token = idx->token;
                    op->properties[prop] = std::move(desc);
                    return Value{newv};
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

        // Logical OR: short-circuit; return operand (left or right).
        if (op == "||" || op == "au") {
            // If left is truthy -> return left (no RHS evaluation).
            if (to_bool(left)) return left;
            // Otherwise evaluate RHS and return it.
            return evaluate_expression(b->right.get(), env);
        }

        // For all other binary operators evaluate RHS now (both operands required)
        Value right = evaluate_expression(b->right.get(), env);

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

            return call_function(fn, args, effectiveTok);
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
                call_function_with_receiver(constructorFn, instance, ctorArgs, persisted->token);
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
                    Value res = call_function_with_receiver(parentCtor, receiver, args, m->token);
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
                        Value res = call_function_with_receiver(dtorFn, obj, args, m->token);
                        obj->properties.clear();
                        return res;
                    }
                }
            }
        }

        obj->properties.clear();
        return std::monostate{};
    }

    throw SwaziError(
        "InternalError",
        "Unhandled expression node encountered in evaluator — this is likely a bug in the interpreter.",
        expr->token.loc  // if expr has a token; otherwise pass a default/fake location
    );
}