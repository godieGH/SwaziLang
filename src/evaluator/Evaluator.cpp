// src/evaluator/Evaluator.cpp
#include "evaluator.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "Frame.hpp"
#include "Scheduler.hpp"
#include "colors.hpp"
#include "globals.hpp"
namespace fs = std::filesystem;
#include "AsyncBridge.hpp"

Evaluator::~Evaluator() = default;

Evaluator::Evaluator() : global_env(std::make_shared<Environment>(nullptr)), main_module_env(nullptr) {
    // Pass 'this' pointer into init_globals so some builtins (Promise constructor,
    // etc.) can synchronously call back into the evaluator.
    init_globals(global_env, this);

    scheduler_ = std::make_unique<Scheduler>();

    // Register scheduler runner that knows how to interpret the CallbackPayload.
    register_scheduler_runner(
        scheduler_.get(),
        [this](void* boxed) {
            if (!boxed) return;
            CallbackPayload* p = static_cast<CallbackPayload*>(boxed);
            if (!p) return;
            FunctionPtr cb = p->cb;
            std::vector<Value> args = p->args;
            // delete the payload now (we own it)
            delete p;
            if (!cb) return;
            try {
                this->call_function(cb, args, cb->closure, cb->token);
            } catch (const std::exception& e) {
                std::cerr << "Unhandled async callback exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unhandled async callback unknown exception" << std::endl;
            }
        });
}
void Evaluator::push_frame(CallFramePtr f) {
    if (!f) return;
    call_stack_.push_back(f);
}

void Evaluator::pop_frame() {
    if (!call_stack_.empty()) {
        call_stack_.pop_back();
    }
}

CallFramePtr Evaluator::current_frame() {
    if (call_stack_.empty()) return nullptr;
    return call_stack_.back();
}

void Evaluator::add_suspended_frame(CallFramePtr f) {
    if (!f) return;
    // keep one shared_ptr owning this suspended frame so it isn't destroyed
    suspended_frames_.push_back(f);
}

void Evaluator::remove_suspended_frame(CallFramePtr f) {
    if (!f) return;
    auto it = std::find(suspended_frames_.begin(), suspended_frames_.end(), f);
    if (it != suspended_frames_.end()) {
        suspended_frames_.erase(it);
    }
}

std::vector<CallFramePtr> Evaluator::get_call_stack_snapshot() {
    return call_stack_;
}

// Public wrapper so native builtins can invoke interpreter functions synchronously.
// This simply forwards to the private call_function implementation.
Value Evaluator::invoke_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken) {
    return call_function(fn, args, caller_env, callToken);
}

// ----------------- Program evaluation -----------------
void Evaluator::evaluate(ProgramNode* program) {
    if (!program) return;
    Value dummy_ret;
    bool did_return = false;

    // Choose run environment: use main_module_env if it was created by set_entry_point,
    // otherwise fall back to global_env (REPL mode or tests).
    EnvPtr run_env = main_module_env ? main_module_env : global_env;

    for (auto& stmt_uptr : program->body) {
        evaluate_statement(stmt_uptr.get(), run_env, &dummy_ret, &did_return);
        if (did_return) break;
    }
    try {
        run_event_loop();
    } catch (const std::exception& e) {
        std::cerr << "Error while running async callbacks: " << e.what() << std::endl;
    } catch (...) {
        std::cerr << "Unknown error while running async callbacks\n";
    }
    sweep_external_data();
}
void Evaluator::populate_module_metadata(EnvPtr env, const std::string& resolved_path, const std::string& module_name, bool is_main) {
    if (!env) return;

    // __main__ (bool)
    {
        Environment::Variable v;
        v.value = is_main;
        v.is_constant = true;
        env->set("__main__", v);
    }

    // __name__ (basename or provided name)
    {
        Environment::Variable v;
        v.value = module_name;
        v.is_constant = true;
        env->set("__name__", v);
    }

    // __file__ (absolute resolved path or empty)
    {
        Environment::Variable v;
        v.value = resolved_path;
        v.is_constant = true;
        env->set("__file__", v);
    }

    // __dir__ (parent directory of resolved_path or empty)
    std::string dirstr;
    if (!resolved_path.empty()) {
        try {
            dirstr = fs::weakly_canonical(fs::path(resolved_path)).parent_path().string();
        } catch (...) {
            dirstr = fs::path(resolved_path).parent_path().string();
        }
    }
    {
        Environment::Variable v;
        v.value = dirstr;
        v.is_constant = true;
        env->set("__dir__", v);
    }

    // --- Expose __builtins__ variable in module env pointing to global_env via an env-proxy object
    {
        auto builtins_proxy = std::make_shared<ObjectValue>();
        builtins_proxy->is_env_proxy = true;
        builtins_proxy->proxy_env = this->global_env;  // evaluator's global_env

        Environment::Variable v;
        v.value = builtins_proxy;
        v.is_constant = true;
        env->set("__builtins__", v);
    }
}
void Evaluator::set_entry_point(const std::string& filename) {
    // Resolve filename to canonical absolute path if provided; empty => REPL
    std::string resolved;
    if (!filename.empty()) {
        try {
            resolved = fs::weakly_canonical(fs::path(filename)).string();
        } catch (...) {
            resolved = fs::absolute(fs::path(filename)).string();
        }
    }

    std::string name;
    if (resolved.empty())
        name = "<repl>";
    else
        name = fs::path(resolved).filename().string();

    // Create a dedicated module environment for the main program. It should be a child of
    // global_env (which contains builtins and shared runtime). This prevents main top-level
    // variables from becoming the parent for imported modules.
    main_module_env = std::make_shared<Environment>(global_env);

    // Populate __main__/__name__/__file__/__dir__ in the main module env.
    populate_module_metadata(main_module_env, resolved, name, true);
}

void Evaluator::set_cli_args(const std::vector<std::string>& args) {
    // store copy locally (if you need it later)
    this->cli_args = args;

    // Build ArrayValue of strings
    auto arr = std::make_shared<ArrayValue>();
    arr->elements.reserve(args.size());
    for (const auto& s : args) {
        arr->elements.push_back(Value{std::string(s)});
    }

    // If global_env exists, set or replace its "argv" variable
    if (this->global_env) {
        Environment::Variable var;
        var.value = arr;
        var.is_constant = true;  // binding is constant
        this->global_env->set("argv", var);
    }
}

// --- Promise resolution helpers (deliver via microtasks and support unhandled rejection reporting) ---
void Evaluator::fulfill_promise(PromisePtr p, const Value& value) {
    if (!p) return;
    if (p->state != PromiseValue::State::PENDING) return;
    p->state = PromiseValue::State::FULFILLED;
    p->result = value;

    // Snapshot then callbacks to avoid races if callbacks mutate promise
    auto callbacks = p->then_callbacks;

    if (scheduler()) {
        scheduler()->enqueue_microtask([callbacks, value]() mutable {
            for (auto& cb : callbacks) {
                try {
                    cb(value);
                } catch (...) {}
            }
        });
    } else {
        // Fallback: execute directly (best-effort for no-scheduler mode)
        for (auto& cb : callbacks) {
            try {
                cb(value);
            } catch (...) {}
        }
    }
}

void Evaluator::reject_promise(PromisePtr p, const Value& reason) {
    if (!p) return;
    if (p->state != PromiseValue::State::PENDING) return;

    p->state = PromiseValue::State::REJECTED;
    p->result = reason;

    // Snapshot catch callbacks
    auto callbacks = p->catch_callbacks;

    if (scheduler()) {
        // Deliver catch callbacks in a microtask. After that microtask we schedule
        // a single additional microtask that checks whether the promise was handled;
        // we only schedule that "unhandled check" once per promise using the flag.
        scheduler()->enqueue_microtask([this, p, callbacks, reason]() mutable {
            // Run user catch callbacks (they may call .then/.catch on the promise)
            for (auto& cb : callbacks) {
                try {
                    cb(reason);
                } catch (...) {}
            }

            // Schedule the unhandled-rejection check in a subsequent microtask,
            // but only once for this promise.
            // This allows handlers attached in the same microtask to be considered.
            if (!p->unhandled_check_scheduled) {
                p->unhandled_check_scheduled = true;
                scheduler()->enqueue_microtask([this, p]() {
                    if (!p) return;
                    if (!p->handled && !p->unhandled_reported) {
                        report_unhandled_rejection(p);
                    }
                });
            }
        });
    } else {
        // No scheduler: run handlers immediately then report if unhandled (best-effort)
        for (auto& cb : callbacks) {
            try {
                cb(reason);
            } catch (...) {}
        }
        if (!p->handled && !p->unhandled_reported) {
            report_unhandled_rejection(p);
        }
    }
}

void Evaluator::report_unhandled_rejection(PromisePtr p) {
    if (!p) return;
    if (p->unhandled_reported) return;
    p->unhandled_reported = true;

    std::string reason_str = to_string_value(p->result);

    bool use_color = Color::supports_color();
    const std::string& gray = use_color ? Color::bright_black : "";
    const std::string& red = use_color ? Color::bright_red : "";
    const std::string& yellow = use_color ? Color::bright_yellow : "";
    const std::string& reset = use_color ? Color::reset : "";

    std::cerr
        << std::endl
        << red << "UnhandledPromiseRejectionError" << reset << ": "
        << reason_str << std::endl
        << gray << "    at: promise rejection (async)" << reset << std::endl
        << yellow << "⚠️  Tip:" << reset << " Use .catch(...) or try { await ... } catch (...) to handle this rejection." << std::endl
        << gray << "    (This will terminate in future versions if not handled)" << reset << std::endl
        << std::endl;
}
void Evaluator::mark_promise_and_ancestors_handled(PromisePtr p) {
    while (p) {
        if (p->handled) break;
        p->handled = true;
        auto wp = p->parent;
        p = wp.lock();
    }
}
