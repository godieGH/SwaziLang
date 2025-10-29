// src/evaluator/Evaluator.cpp
#include "evaluator.hpp"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "globals.hpp"
namespace fs = std::filesystem;

Evaluator::Evaluator() : global_env(std::make_shared<Environment>(nullptr)) {
    global_env = std::make_shared<Environment>();
    init_globals(global_env);
}

// ----------------- Program evaluation -----------------
void Evaluator::evaluate(ProgramNode* program) {
    if (!program) return;
    Value dummy_ret;
    bool did_return = false;
    for (auto& stmt_uptr : program->body) {
        evaluate_statement(stmt_uptr.get(), global_env, &dummy_ret, &did_return);
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

    populate_module_metadata(global_env, resolved, name, true);
}