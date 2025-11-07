// src/evaluator/Evaluator.cpp
#include "evaluator.hpp"

#include "Scheduler.hpp"
#include "Frame.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "globals.hpp"
namespace fs = std::filesystem;
#include "AsyncBridge.hpp"

Evaluator::~Evaluator() = default;

Evaluator::Evaluator() : global_env(std::make_shared<Environment>(nullptr)), main_module_env(nullptr) {
    init_globals(global_env);

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