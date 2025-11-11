#include <filesystem>
#include <fstream>
#include <sstream>

#include "SwaziError.hpp"
#include "builtin_sl.h"
#include "builtins.hpp"  //built-in module factories (regex, fs, http)
#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"
namespace fs = std::filesystem;

// Resolve the module specifier to an existing file path. Tries:
// - If spec has extension and exists -> use it.
// - Else try relative to requester file directory: spec, spec + ".sl", spec + ".swz"
// - If requester filename is "<repl>" or empty, resolve relative to current_path().
std::string Evaluator::resolve_module_path(const std::string& module_spec, const std::string& requester_filename, const Token& tok) {
    fs::path specPath(module_spec);
    fs::path baseDir;

    if (requester_filename.empty() || requester_filename == "<repl>") {
        baseDir = fs::current_path();
    } else {
        baseDir = fs::path(requester_filename).parent_path();
    }

    // If spec is absolute, try it directly
    if (specPath.is_absolute()) {
        if (fs::exists(specPath)) return fs::weakly_canonical(specPath).string();
        // if no extension, try .sl/.swz
        if (!specPath.has_extension()) {
            std::vector<std::string> exts = {
                ".sl",
                ".swz"};
            for (auto& e : exts) {
                fs::path cand = specPath;
                cand += e;
                if (fs::exists(cand)) return fs::weakly_canonical(cand).string();
            }
        }
    } else {
        // try relative to baseDir
        fs::path cand = baseDir / specPath;
        if (fs::exists(cand)) return fs::weakly_canonical(cand).string();

        // if extension present but not found, try exact candidate anyway (maybe caller used relative without ext)
        if (!specPath.has_extension()) {
            std::vector<std::string> exts = {
                ".sl",
                ".swz"};
            for (auto& e : exts) {
                fs::path c2 = cand;
                c2 += e;
                if (fs::exists(c2)) return fs::weakly_canonical(c2).string();
            }
        }
        // As a last attempt, try current_path / spec
        cand = fs::current_path() / specPath;
        if (fs::exists(cand)) return fs::weakly_canonical(cand).string();
        if (!specPath.has_extension()) {
            std::vector<std::string> exts = {
                ".sl",
                ".swz"};
            for (auto& e : exts) {
                fs::path c2 = cand;
                c2 += e;
                if (fs::exists(c2)) return fs::weakly_canonical(c2).string();
            }
        }
    }

    throw SwaziError(
        "ReferenceError",
        "Module not found for '" + module_spec + "'.",
        tok.loc);
}

// import_module: loads, parses, evaluates a module file and returns its exports object.
// Handles caching and circular dependencies: a ModuleRecord is placed in module_cache
// in Loading state BEFORE evaluating the module so circular imports can receive a live
// (possibly partially-initialized) exports object.
ObjectPtr Evaluator::import_module(const std::string& module_spec, const Token& requesterTok, EnvPtr requesterEnv) {
    if (has_embedded_module(module_spec)) {
        const std::string key = std::string("__embedded__:") + module_spec;
        auto it_cache = module_cache.find(key);
        if (it_cache != module_cache.end()) return it_cache->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;

        populate_module_metadata(rec->module_env, rec->path, std::string("<embedded:") + module_spec + ">", false);

        auto srcOpt = get_embedded_module_source(module_spec);
        if (!srcOpt.has_value()) {
            module_cache.erase(key);
            throw SwaziError(
                "ReferenceError",
                "Embedded module found but source is missing for '" + module_spec + "'.",
                requesterTok.loc);
        }

        std::string src = srcOpt.value();
        if (src.empty() || src.back() != '\n') src.push_back('\n');

        Lexer lexer(src, std::string("<embedded:") + module_spec + ">");
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(tokens);
        std::unique_ptr<ProgramNode> ast = parser.parse();

        try {
            for (auto& stmt : ast->body) {
                if (!stmt) continue;
                if (auto ed = dynamic_cast<ExportDeclarationNode*>(stmt.get())) {
                    // default export
                    if (ed->is_default) {
                        if (ed->single_identifier.empty()) {
                            rec->exports->properties["default"] = PropertyDescriptor{std::monostate{}, false, false, false, ed->token};
                        } else {
                            if (!rec->module_env->has(ed->single_identifier)) {
                                throw SwaziError(
                                    "ReferenceError",
                                    "Export name '" + ed->single_identifier + "' not defined in embedded module '" + module_spec + "'.",
                                    ed->token.loc);
                            }
                            Environment::Variable& v = rec->module_env->get(ed->single_identifier);
                            PropertyDescriptor pd{v.value, false, false, false, ed->token};
                            rec->exports->properties["default"] = pd;
                        }
                    } else {
                        // named exports
                        for (const auto& nm : ed->names) {
                            if (!rec->module_env->has(nm)) {
                                PropertyDescriptor pd;
                                pd.value = std::monostate{};
                                pd.token = ed->token;
                                rec->exports->properties[nm] = pd;
                            } else {
                                Environment::Variable& v = rec->module_env->get(nm);
                                PropertyDescriptor pd{v.value, false, false, false, ed->token};
                                rec->exports->properties[nm] = pd;
                            }
                        }
                    }
                    break;  // stop executing after 'ruhusu' as modules do
                }
                evaluate_statement(stmt.get(), rec->module_env);
            }
        } catch (...) {
            module_cache.erase(key);
            throw;
        }

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    // --- built-in modules short-circuit ---
    // Handle built-ins first (so "regex", "fs", "http" refer to native factories).
    // This must run before resolving filesystem paths.
    if (module_spec == "regex" || module_spec == "swazi:regex") {
        const std::string key = "__builtin__:regex";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "regex", false);

        rec->exports = make_regex_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "fs" || module_spec == "swazi:fs") {
        const std::string key = "__builtin__:fs";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "fs", false);

        rec->exports = make_fs_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "http" || module_spec == "swazi:http") {
        const std::string key = "__builtin__:http";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "http", false);

        rec->exports = make_http_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "json" || module_spec == "swazi:json") {
        const std::string key = "__builtin__:json";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "json", false);

        rec->exports = make_json_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "path" || module_spec == "swazi:path") {
        const std::string key = "__builtin__:path";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "path", false);

        rec->exports = make_path_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "os" || module_spec == "swazi:os") {
        const std::string key = "__builtin__:os";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "os", false);

        rec->exports = make_os_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "process" || module_spec == "swazi:process") {
        const std::string key = "__builtin__:process";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "process", false);

        rec->exports = make_process_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    // child_process builtin
    if (module_spec == "child_process" || module_spec == "swazi:child_process") {
        const std::string key = "__builtin__:child_process";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "child_process", false);

        rec->exports = make_child_process_exports(rec->module_env, this);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }

    if (module_spec == "timers" || module_spec == "swazi:timers") {
        const std::string key = "__builtin__:timers";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "timers", false);

        rec->exports = make_timers_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }
    
    if (module_spec == "threads" || module_spec == "swazi:threads") {
        const std::string key = "__builtin__:threads";
        auto it = module_cache.find(key);
        if (it != module_cache.end()) return it->second->exports;

        auto rec = std::make_shared<ModuleRecord>();
        rec->state = ModuleRecord::State::Loading;
        rec->exports = std::make_shared<ObjectValue>();
        rec->path = key;
        rec->module_env = std::make_shared<Environment>(global_env);
        module_cache[key] = rec;
        populate_module_metadata(rec->module_env, rec->path, "threads", false);

        rec->exports = make_threads_exports(rec->module_env);

        rec->state = ModuleRecord::State::Loaded;
        return rec->exports;
    }
    
    
    

    // --- end built-in short-circuit ---

    // Resolve filesystem path (throws if not found)
    std::string resolved = resolve_module_path(module_spec, requesterTok.loc.filename, requesterTok);

    // Use canonical string as cache key
    std::string key = resolved;

    // If cached
    auto it = module_cache.find(key);
    if (it != module_cache.end()) {
        auto rec = it->second;
        if (!rec) {
            throw SwaziError(
                "InternalError",
                "Internal module cache corruption for '" + key + "'.",
                requesterTok.loc);
        }
        // If loading, return the mounts.exports for circular dependency (may be incomplete)
        if (rec->state == ModuleRecord::State::Loading) {
            return rec->exports;
        }
        // Loaded -> return exports
        return rec->exports;
    }

    // Create a new record and insert into cache BEFORE evaluating to support cycles.
    auto rec = std::make_shared<ModuleRecord>();
    rec->state = ModuleRecord::State::Loading;
    rec->exports = std::make_shared<ObjectValue>();
    rec->path = key;
    // Module environment: parent is global_env so modules can access builtin/globals.
    rec->module_env = std::make_shared<Environment>(global_env);
    module_cache[key] = rec;
    std::string module_basename;
    try {
        module_basename = fs::path(resolved).filename().string();
    } catch (...) {
        module_basename = resolved;
    }
    populate_module_metadata(rec->module_env, resolved, module_basename, false);

    // Read file contents
    std::ifstream in(resolved);
    if (!in.is_open()) {
        module_cache.erase(key);
        throw SwaziError(
            "IOError",
            "Unable to open module file: '" + resolved + "'.",
            requesterTok.loc);
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string src = buf.str();
    if (src.empty() || src.back() != '\n') src.push_back('\n');

    // Lex + parse
    Lexer lexer(src, resolved);
    std::vector<Token> tokens = lexer.tokenize();
    Parser parser(tokens);
    std::unique_ptr<ProgramNode> ast = parser.parse();

    // Evaluate program statements in module environment.
    // Behavior: normal execution until an ExportDeclarationNode is encountered.
    // When export node is seen, collect exports (default or named) from module_env and stop executing further statements.
    try {
        for (auto& stmt : ast->body) {
            if (!stmt) continue;

            // If statement is an export declaration, materialize exports and stop executing subsequent code.
            if (auto ed = dynamic_cast<ExportDeclarationNode*>(stmt.get())) {
                // Default export: ruhusu app
                if (ed->is_default) {
                    if (ed->single_identifier.empty()) {
                        // nothing to export
                        rec->exports->properties["default"] = PropertyDescriptor{
                            std::monostate{},
                            false,
                            false,
                            false,
                            ed->token};
                    } else {
                        if (!rec->module_env->has(ed->single_identifier)) {
                            throw SwaziError(
                                "ReferenceError",
                                "Export name '" + ed->single_identifier + "' not defined in module '" + key + "'.",
                                ed->token.loc);
                        }
                        Environment::Variable& v = rec->module_env->get(ed->single_identifier);
                        PropertyDescriptor pd;
                        pd.value = v.value;
                        pd.is_private = false;
                        pd.is_readonly = false;
                        pd.is_locked = false;
                        pd.token = ed->token;
                        rec->exports->properties["default"] = pd;
                    }
                } else {
                    // named exports list
                    for (const auto& nm : ed->names) {
                        if (!rec->module_env->has(nm)) {
                            // missing name -> export undefined
                            PropertyDescriptor pd;
                            pd.value = std::monostate{};
                            pd.token = ed->token;
                            rec->exports->properties[nm] = pd;
                        } else {
                            Environment::Variable& v = rec->module_env->get(nm);
                            PropertyDescriptor pd;
                            pd.value = v.value;
                            pd.is_private = false;
                            pd.is_readonly = false;
                            pd.is_locked = false;
                            pd.token = ed->token;
                            rec->exports->properties[nm] = pd;
                        }
                    }
                }
                // honor user's rule: ruhusu ends execution of module (stop here)
                break;
            }

            // Otherwise evaluate statement in module env. This will allow nested imports, functions, classes, etc.
            evaluate_statement(stmt.get(), rec->module_env);
        }
    } catch (...) {
        // On exception during evaluation, remove cache entry to avoid broken modules being reused
        module_cache.erase(key);
        throw;
    }

    // Mark loaded
    rec->state = ModuleRecord::State::Loaded;
    return rec->exports;
}