// src/evaluator/FunctionCall.cpp
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "SwaziError.hpp"
#include "ClassRuntime.hpp"
#include "evaluator.hpp"

Value Evaluator::call_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken) {
    if (!fn) {
        throw SwaziError(
            "TypeError",
            "Attempt to call a null function.",
            callToken.loc
        );
    }

    // Native function: call the C++ implementation directly.
    if (fn->is_native) {
        if (!fn->native_impl) {
            throw SwaziError(
                "TypeError",
                "Native function has no implementation.",
                callToken.loc
            );
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
            callToken.loc
        );
    }

    // create local environment whose parent is the function's closure
    auto local = std::make_shared<Environment>(fn->closure);

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
                        callToken.loc
                    );
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
                    callToken.loc
                );
            }
        }
        var.is_constant = false;
        local->set(p->name, var);
    }

    Value ret_val = std::monostate{};
    bool did_return = false;

    for (auto& stmt_uptr : fn->body->body) {
        evaluate_statement(stmt_uptr.get(), local, &ret_val, &did_return);
        if (did_return) break;
    }

    return did_return ? ret_val : std::monostate{};
}

Value Evaluator::call_function_with_receiver(FunctionPtr fn, ObjectPtr receiver, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken) {
    if (!fn) {
        throw SwaziError(
            "TypeError",
            "Attempt to call a null function.",
            callToken.loc
        );
    }

    // Native function case: forward (native_impl receives caller_env so it can inspect $ if needed)
    if (fn->is_native) {
        if (!fn->native_impl) {
            throw SwaziError(
                "TypeError",
                "Native function has no implementation.",
                callToken.loc
            );
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
            callToken.loc
        );
    }

    // create local environment whose parent is the function's closure
    auto local = std::make_shared<Environment>(fn->closure);

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

    for (auto& stmt_uptr : fn->body->body) {
        evaluate_statement(stmt_uptr.get(), local, &ret_val, &did_return);
        if (did_return) break;
    }

    return did_return ? ret_val : std::monostate{};
}