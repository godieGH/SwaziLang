#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

#include <fstream>
#include <sstream>
#include <filesystem>
namespace fs = std::filesystem;

// Resolve the module specifier to an existing file path. Tries:
// - If spec has extension and exists -> use it.
// - Else try relative to requester file directory: spec, spec + ".sl", spec + ".swz"
// - If requester filename is "<repl>" or empty, resolve relative to current_path().
std::string Evaluator::resolve_module_path(const std::string &module_spec, const std::string &requester_filename, const Token &tok) {
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
            std::vector<std::string> exts = { ".sl", ".swz" };
            for (auto &e: exts) {
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
            std::vector<std::string> exts = { ".sl", ".swz" };
            for (auto &e: exts) {
                fs::path c2 = cand;
                c2 += e;
                if (fs::exists(c2)) return fs::weakly_canonical(c2).string();
            }
        }
        // As a last attempt, try current_path / spec
        cand = fs::current_path() / specPath;
        if (fs::exists(cand)) return fs::weakly_canonical(cand).string();
        if (!specPath.has_extension()) {
            std::vector<std::string> exts = { ".sl", ".swz" };
            for (auto &e: exts) {
                fs::path c2 = cand;
                c2 += e;
                if (fs::exists(c2)) return fs::weakly_canonical(c2).string();
            }
        }
    }

    throw std::runtime_error("Module not found for '" + module_spec + "' at " + tok.loc.to_string());
}

// import_module: loads, parses, evaluates a module file and returns its exports object.
// Handles caching and circular dependencies: a ModuleRecord is placed in module_cache
// in Loading state BEFORE evaluating the module so circular imports can receive a live
// (possibly partially-initialized) exports object.
ObjectPtr Evaluator::import_module(const std::string &module_spec, const Token &requesterTok, EnvPtr requesterEnv) {
    // Resolve filesystem path (throws if not found)
    std::string resolved = resolve_module_path(module_spec, requesterTok.loc.filename, requesterTok);

    // Use canonical string as cache key
    std::string key = resolved;

    // If cached
    auto it = module_cache.find(key);
    if (it != module_cache.end()) {
        auto rec = it->second;
        if (!rec) throw std::runtime_error("Internal module cache corruption for " + key);
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

    // Read file contents
    std::ifstream in(resolved);
    if (!in.is_open()) {
        module_cache.erase(key);
        throw std::runtime_error("Unable to open module file: " + resolved);
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
        for (auto &stmt : ast->body) {
            if (!stmt) continue;

            // If statement is an export declaration, materialize exports and stop executing subsequent code.
            if (auto ed = dynamic_cast<ExportDeclarationNode*>(stmt.get())) {
                // Default export: ruhusu app
                if (ed->is_default) {
                    if (ed->single_identifier.empty()) {
                        // nothing to export
                        rec->exports->properties["default"] = PropertyDescriptor{ std::monostate{}, false, false, false, ed->token };
                    } else {
                        if (!rec->module_env->has(ed->single_identifier)) {
                            throw std::runtime_error("Export name '" + ed->single_identifier + "' not defined in module " + key + " at " + ed->token.loc.to_string());
                        }
                        Environment::Variable &v = rec->module_env->get(ed->single_identifier);
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
                    for (const auto &nm : ed->names) {
                        if (!rec->module_env->has(nm)) {
                            // missing name -> export undefined
                            PropertyDescriptor pd;
                            pd.value = std::monostate{};
                            pd.token = ed->token;
                            rec->exports->properties[nm] = pd;
                        } else {
                            Environment::Variable &v = rec->module_env->get(nm);
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