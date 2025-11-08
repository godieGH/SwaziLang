// src/evaluator/FunctionCall.cpp
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "Frame.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"

void Evaluator::execute_frame_until_await_or_return(CallFramePtr frame, PromisePtr promise) {
    if (!frame || !frame->function) return;

    auto& body = frame->function->body->body;

    // Resume from where we left off
    while (frame->next_statement_index < body.size()) {
        auto* stmt = body[frame->next_statement_index].get();

        try {
            Value ret_val;
            bool did_return = false;

            // evaluate the current statement — this may throw SuspendExecution to indicate suspension
            evaluate_statement(stmt, frame->env, &ret_val, &did_return);

            if (did_return) {
                if (promise) {
                    // Use the evaluator helper so resolution is always delivered via microtasks
                    this->fulfill_promise(promise, ret_val);
                }
                pop_frame();
                return;
            }

            // advance to next statement only on successful evaluation (no suspension)
            frame->next_statement_index++;
        } catch (const SuspendExecution&) {
            // Normal suspension — leave frame->next_statement_index unchanged so resume re-evaluates same statement.
            return;
        } catch (const std::exception& e) {
            // Fatal exception while executing an async function -> reject the promise
            if (promise) {
                this->reject_promise(promise, Value{std::string(e.what())});
            }
            pop_frame();
            return;
        } catch (...) {
            // unknown throw -> reject with a generic reason
            if (promise) {
                this->reject_promise(promise, Value{std::string("unknown exception")});
            }
            pop_frame();
            return;
        }
    }

    // Function completed without explicit return -> resolve undefined
    if (promise) {
        this->fulfill_promise(promise, std::monostate{});
    }
    pop_frame();
}
void Evaluator::execute_frame_until_return(CallFramePtr frame) {
    if (!frame || !frame->function) return;

    // Ensure frame->env exists
    if (!frame->env) {
        frame->env = std::make_shared<Environment>(frame->function->closure);
    }

    auto& body = frame->function->body->body;

    // Resume from stored index (0 for fresh)
    while (frame->next_statement_index < body.size()) {
        auto* stmt = body[frame->next_statement_index].get();

        Value ret_val;
        bool did_return = false;

        // Evaluate statement in the frame's environment
        evaluate_statement(stmt, frame->env, &ret_val, &did_return);

        if (did_return) {
            // store return into frame as a Value inside std::any (frame header uses std::any)
            frame->return_value = ret_val;
            frame->did_return = true;
            return;
        }

        // Advance to next statement
        frame->next_statement_index++;
    }

    // No explicit return; treat as returning undefined (std::monostate)
    frame->return_value = std::monostate{};
    frame->did_return = false;  // no explicit 'return' statement, but completion yields undefined
}

Value Evaluator::call_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken) {
    if (!fn) {
        throw SwaziError(
            "TypeError",
            "Attempt to call a null function.",
            callToken.loc);
    }

    // Native function: call the C++ implementation directly.
    if (fn->is_native) {
        if (!fn->native_impl) {
            throw SwaziError(
                "TypeError",
                "Native function has no implementation.",
                callToken.loc);
        }
        // Pass the dynamic caller environment (caller_env) so native_impl can inspect
        // and operate on the environment where the call happened (module/repl/main).
        return fn->native_impl(args, caller_env, callToken);
    }

    // --- compute minimum required arguments (including rest_required_count) ---
    size_t minRequired = 0;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        auto& p = fn->parameters[i];
        if (!p) {
            // defensive: treat missing param descriptor as required single slot
            minRequired += 1;
            continue;
        }
        if (p->is_rest) {
            minRequired += p->rest_required_count;
        } else if (!p->defaultValue) {
            minRequired += 1;
        }
    }

    if (args.size() < minRequired) {
        std::ostringstream ss;
        ss << "Function '" << (fn->name.empty() ? "<lambda>" : fn->name)
           << "' expects at least " << minRequired << " argument(s) but got " << args.size();
        throw SwaziError(
            "TypeError",
            ss.str(),
            callToken.loc);
    }

    auto frame = std::make_shared<CallFrame>();
    frame->function = fn;
    frame->env = nullptr;  // will set to local env after it's created
    frame->call_token = callToken;
    frame->label = fn->name.empty() ? "<lambda>" : fn->name;
    frame->is_async = fn->is_async;

    push_frame(frame);

    // create local environment whose parent is the function's closure
    auto local = std::make_shared<Environment>(fn->closure);
    frame->env = local;

    // Bind parameters left-to-right. Rest parameter (if any) collects appropriate args.
    size_t argIndex = 0;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        auto& p = fn->parameters[i];

        // defensive: if descriptor missing, bind positionally if available else undefined (skip binding name)
        if (!p) {
            if (argIndex < args.size()) argIndex++;
            continue;
        }

        if (p->is_rest) {
            auto arr = std::make_shared<ArrayValue>();
            if (p->rest_required_count > 0) {
                size_t remaining = (argIndex < args.size()) ? (args.size() - argIndex) : 0;
                if (remaining < p->rest_required_count) {
                    std::ostringstream ss;
                    ss << "Function '" << (fn->name.empty() ? "<lambda>" : fn->name)
                       << "' rest parameter '" << p->name << "' requires at least " << p->rest_required_count
                       << " elements but got " << remaining;
                    throw SwaziError(
                        "TypeError",
                        ss.str(),
                        callToken.loc);
                }
                for (size_t k = 0; k < p->rest_required_count; ++k) {
                    arr->elements.push_back(args[argIndex++]);
                }
            } else {
                while (argIndex < args.size()) {
                    arr->elements.push_back(args[argIndex++]);
                }
            }
            Environment::Variable var;
            var.value = arr;
            var.is_constant = false;
            local->set(p->name, var);
            continue;
        }

        Environment::Variable var;
        if (argIndex < args.size()) {
            var.value = args[argIndex++];
        } else {
            if (p->defaultValue) {
                var.value = evaluate_expression(p->defaultValue.get(), local);
            } else {
                std::ostringstream ss;
                ss << "Function '" << (fn->name.empty() ? "<lambda>" : fn->name)
                   << "' missing required argument '" << p->name << "'";
                throw SwaziError(
                    "TypeError",
                    ss.str(),
                    callToken.loc);
            }
        }
        var.is_constant = false;
        local->set(p->name, var);
    }

    Value ret_val = std::monostate{};
    bool did_return = false;

    // If async function, create and return a Promise immediately.
    // Start executing the async function synchronously on the calling thread
    // until the first await. If it suspends (SuspendExecution), we return
    // the pending promise. If it completes synchronously, the promise will
    // be fulfilled by execute_frame_until_await_or_return() before we return.
    if (fn->is_async) {
        auto promise = std::make_shared<PromiseValue>();
        frame->pending_promise = promise;

        try {
            // Execute right away until first await or completion.
            execute_frame_until_await_or_return(frame, promise);
            // If execution completed synchronously, execute_frame.. will have
            // fulfilled the promise (or rejected it) and popped the frame.
        } catch (const SuspendExecution&) {
            // Normal suspension — the frame remains on the call stack and
            // the promise is still pending. Resume will be scheduled by await code.
            return promise;
        } catch (const std::exception& e) {
            this->reject_promise(promise, Value{std::string(e.what())});
            // ensure frame popped if not already
            pop_frame();
            return promise;
        } catch (...) {
            this->reject_promise(promise, Value{std::string("unknown exception")});
            pop_frame();
            return promise;
        }

        // Return the promise (either pending because we suspended, or already fulfilled)
        return promise;
    }

    // Sync function: run directly
    try {
        execute_frame_until_return(frame);
    } catch (...) {
        pop_frame();
        throw;
    }

    // Extract Value result from opaque slot and pop frame
    Value result = std::monostate{};
    if (frame->did_return) {
        try {
            result = std::any_cast<Value>(frame->return_value);
        } catch (const std::bad_any_cast&) {
            // Defensive: if something else was stored, treat as undefined
            result = std::monostate{};
        }
    } else {
        // function completed without explicit return -> undefined
        try {
            // if someone filled return_value, try to read it, else undefined
            result = std::any_cast<Value>(frame->return_value);
        } catch (...) {
            result = std::monostate{};
        }
    }
    pop_frame();
    return result;
}

Value Evaluator::call_function_with_receiver(FunctionPtr fn, ObjectPtr receiver, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken) {
    if (!fn) {
        throw SwaziError(
            "TypeError",
            "Attempt to call a null function.",
            callToken.loc);
    }

    // Native function case: forward (native_impl receives caller_env so it can inspect $ if needed)
    if (fn->is_native) {
        if (!fn->native_impl) {
            throw SwaziError(
                "TypeError",
                "Native function has no implementation.",
                callToken.loc);
        }
        return fn->native_impl(args, caller_env, callToken);
    }

    // compute minimum required arguments (including rest_required_count)
    size_t minRequired = 0;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        auto& p = fn->parameters[i];
        if (!p) {
            minRequired += 1;
            continue;
        }
        if (p->is_rest)
            minRequired += p->rest_required_count;
        else if (!p->defaultValue)
            minRequired += 1;
    }

    if (args.size() < minRequired) {
        std::ostringstream ss;
        ss << "Function '" << (fn->name.empty() ? "<lambda>" : fn->name)
           << "' expects at least " << minRequired << " arguments but got " << args.size();
        throw SwaziError(
            "TypeError",
            ss.str(),
            callToken.loc);
    }

    auto frame = std::make_shared<CallFrame>();
    frame->function = fn;
    frame->env = nullptr;  // will set after creating local
    frame->call_token = callToken;
    frame->label = fn->name.empty() ? "<lambda>" : fn->name;
    frame->is_async = fn->is_async;
    frame->receiver = receiver;

    push_frame(frame);

    // create local environment whose parent is the function's closure
    auto local = std::make_shared<Environment>(fn->closure);
    frame->env = local;

    // Bind '$' to receiver so methods / getters can access private fields using existing checks
    Environment::Variable thisVar;
    thisVar.value = receiver;
    thisVar.is_constant = false;
    local->set("$", thisVar);

    // Bind parameters left-to-right...
    size_t argIndex = 0;
    for (size_t i = 0; i < fn->parameters.size(); ++i) {
        auto& p = fn->parameters[i];
        if (!p) {
            if (argIndex < args.size()) argIndex++;
            continue;
        }

        if (p->is_rest) {
            auto arr = std::make_shared<ArrayValue>();
            if (p->rest_required_count > 0) {
                size_t remaining = (argIndex < args.size()) ? (args.size() - argIndex) : 0;
                if (remaining < p->rest_required_count) {
                    std::ostringstream ss;
                    ss << "Function '" << (fn->name.empty() ? "<lamda>" : fn->name)
                       << "' rest parameter '" << p->name << "' requires at least " << p->rest_required_count
                       << " elements but got " << remaining;
                    throw SwaziError("TypeError", ss.str(), callToken.loc);
                }
                for (size_t k = 0; k < p->rest_required_count; ++k)
                    arr->elements.push_back(args[argIndex++]);
            } else {
                while (argIndex < args.size()) arr->elements.push_back(args[argIndex++]);
            }
            Environment::Variable var;
            var.value = arr;
            var.is_constant = false;
            local->set(p->name, var);
            continue;
        }

        Environment::Variable var;
        if (argIndex < args.size()) {
            var.value = args[argIndex++];
        } else {
            if (p->defaultValue) {
                var.value = evaluate_expression(p->defaultValue.get(), local);
            } else {
                std::ostringstream ss;
                ss << "Function '" << (fn->name.empty() ? "<anonymous>" : fn->name)
                   << "' missing required argument '" << p->name << "'";
                throw SwaziError("TypeError", ss.str(), callToken.loc);
            }
        }
        var.is_constant = false;
        local->set(p->name, var);
    }

    Value ret_val = std::monostate{};
    bool did_return = false;

    // If function is async: create promise and run synchronously until first await.
    if (fn->is_async) {
        auto promise = std::make_shared<PromiseValue>();
        frame->pending_promise = promise;
        try {
            execute_frame_until_await_or_return(frame, promise);
        } catch (const SuspendExecution&) {
            return promise;
        } catch (const std::exception& e) {
            promise->state = PromiseValue::State::REJECTED;
            promise->result = std::string(e.what());
            for (auto& cb : promise->catch_callbacks) {
                Value reason = promise->result;
                if (scheduler()) {
                    scheduler()->enqueue_microtask([cb, reason]() { try { cb(reason); } catch (...) {} });
                } else {
                    try {
                        cb(reason);
                    } catch (...) {}
                }
            }
            pop_frame();
            return promise;
        } catch (...) {
            promise->state = PromiseValue::State::REJECTED;
            promise->result = std::string("unknown exception");
            for (auto& cb : promise->catch_callbacks) {
                Value reason = promise->result;
                if (scheduler()) {
                    scheduler()->enqueue_microtask([cb, reason]() { try { cb(reason); } catch (...) {} });
                } else {
                    try {
                        cb(reason);
                    } catch (...) {}
                }
            }
            pop_frame();
            return promise;
        }
        return promise;
    }

    // Sync method: run to completion
    try {
        execute_frame_until_return(frame);
    } catch (...) {
        pop_frame();
        throw;
    }

    // Extract the return value (frame->return_value holds std::any)
    Value result = std::monostate{};
    if (frame->did_return) {
        try {
            result = std::any_cast<Value>(frame->return_value);
        } catch (...) {
            result = std::monostate{};
        }
    } else {
        try {
            result = std::any_cast<Value>(frame->return_value);
        } catch (...) {
            result = std::monostate{};
        }
    }

    pop_frame();
    return result;
}