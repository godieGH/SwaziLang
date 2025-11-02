
#include "builtins.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

#include "evaluator.hpp"

#ifdef __has_include
#if __has_include(<curl/curl.h>)
#include <curl/curl.h>
#define HAVE_LIBCURL 1
#endif
#endif

#ifndef _WIN32
#include <unistd.h>
#else
#include <processthreadsapi.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

// small helper to coerce Value -> string (same idea as earlier)
static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Helper: create a native FunctionValue from a lambda
template <typename F>
static FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    auto fn = std::make_shared<FunctionValue>(name, native_impl, env, Token());
    return fn;
}

#if defined(HAVE_LIBCURL)
// When libcurl is available we need a plain function pointer for the C callback.
// Define the context struct and callback at file scope so they can be referenced
// from the native lambda without needing captures.
struct CurlWriteCtx {
    std::string buf;
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    CurlWriteCtx* ctx = static_cast<CurlWriteCtx*>(userdata);
    if (ctx) ctx->buf.append(ptr, total);
    return total;
}
#endif

// ----------------- REGEX module (extended with optional flags & test) -----------------
static std::regex_constants::syntax_option_type parse_regex_flags(const std::string& flags) {
    using opt = std::regex_constants::syntax_option_type;
    opt opts = std::regex_constants::ECMAScript;  // default
    if (!flags.empty()) {
        if (flags.find('i') != std::string::npos) opts = static_cast<opt>(opts | std::regex_constants::icase);
        // future flags could be mapped here
    }
    return opts;
}

std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // Helper to compile regex with optional flags argument
    auto compile_re = [](const std::string& pat, const std::string& flags) -> std::regex {
        // std::regex doesn't have 'g' or 'm' compile-time flags.
        // Strip semantic-only flags before mapping to std::regex options.
        std::string compileFlags = flags;
        compileFlags.erase(std::remove(compileFlags.begin(), compileFlags.end(), 'g'), compileFlags.end());
        compileFlags.erase(std::remove(compileFlags.begin(), compileFlags.end(), 'm'), compileFlags.end());
        auto opts = parse_regex_flags(compileFlags);
        return std::regex(pat, opts);
    };

    // match(str, pattern [, flags]) -> array|null (JS-like behavior; supports i, g, m)
    {
        auto fn = make_native_fn("regex.match", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("regex.metch at " + token.loc.to_string() + "\nrequires atleast two arguments str and pattern. eg. \nmatch(str, pattern [, flags]) -> array|null (JS-like behavior; supports flags like i, g, m)");
            }

            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : std::string();

            bool global = flags.find('g') != std::string::npos;
            bool multiline = flags.find('m') != std::string::npos;

            try {
                std::regex re = compile_re(pat, flags);

                if (multiline) {
                    // Multiline: run regex over each line so ^/$ behave as line anchors.
                    if (global) {
                        auto arr = std::make_shared<ArrayValue>();
                        size_t pos = 0;
                        while (pos <= s.size()) {
                            size_t next = s.find('\n', pos);
                            size_t len = (next == std::string::npos) ? (s.size() - pos) : (next - pos);
                            std::string line = s.substr(pos, len);
                            std::smatch m;
                            std::string::const_iterator it = line.cbegin();
                            while (std::regex_search(it, line.cend(), m, re)) {
                                arr->elements.push_back(Value{ m.str(0) });
                                if (m.length(0) == 0) {
                                    if (it == line.cend()) break;
                                    ++it;
                                } else {
                                    it = m.suffix().first;
                                }
                            }
                            if (next == std::string::npos) break;
                            pos = next + 1;
                        }
                        if (arr->elements.empty()) return Value{ std::monostate{} };
                        return Value{ arr };
                    } else {
                        // Non-global: return first match (full + groups) found scanning lines
                        size_t pos = 0;
                        while (pos <= s.size()) {
                            size_t next = s.find('\n', pos);
                            size_t len = (next == std::string::npos) ? (s.size() - pos) : (next - pos);
                            std::string line = s.substr(pos, len);
                            std::smatch m;
                            if (std::regex_search(line, m, re)) {
                                auto arr = std::make_shared<ArrayValue>();
                                for (size_t i = 0; i < m.size(); ++i) arr->elements.push_back(Value{ m[i].str() });
                                return Value{ arr };
                            }
                            if (next == std::string::npos) break;
                            pos = next + 1;
                        }
                        return Value{ std::monostate{} };
                    }
                } else {
                    // Not multiline: operate on whole string
                    if (global) {
                        auto arr = std::make_shared<ArrayValue>();
                        std::smatch m;
                        std::string::const_iterator searchStart = s.cbegin();
                        while (std::regex_search(searchStart, s.cend(), m, re)) {
                            arr->elements.push_back(Value{ m.str(0) });
                            if (m.length(0) == 0) {
                                if (searchStart == s.cend()) break;
                                ++searchStart;
                            } else {
                                searchStart = m.suffix().first;
                            }
                        }
                        if (arr->elements.empty()) return Value{ std::monostate{} };
                        return Value{ arr };
                    } else {
                        std::smatch m;
                        if (!std::regex_search(s, m, re)) return Value{ std::monostate{} };
                        auto arr = std::make_shared<ArrayValue>();
                        for (size_t i = 0; i < m.size(); ++i) arr->elements.push_back(Value{ m[i].str() });
                        return Value{ arr };
                    }
                }
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["match"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // test(str, pattern [, flags]) -> bool ( supports m)
    {
        auto fn = make_native_fn("regex.test", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("regex.test at " + token.loc.to_string() + "\nrequires atleast two arguments str and pattern. eg. \ntest(str, pattern [, flags]) -> bool ( supports m)");
            }
            
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : std::string();

            bool multiline = flags.find('m') != std::string::npos;

            try {
                std::regex re = compile_re(pat, flags);
                if (!multiline) {
                    std::smatch m;
                    return Value{ static_cast<bool>(std::regex_search(s, m, re)) };
                } else {
                    size_t pos = 0;
                    while (pos <= s.size()) {
                        size_t next = s.find('\n', pos);
                        size_t len = (next == std::string::npos) ? (s.size() - pos) : (next - pos);
                        std::string line = s.substr(pos, len);
                        std::smatch m;
                        if (std::regex_search(line, m, re)) return Value{ true };
                        if (next == std::string::npos) break;
                        pos = next + 1;
                    }
                    return Value{ false };
                }
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["test"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // fullMatch(str, pattern [, flags]) -> bool
    {
        auto fn = make_native_fn("regex.fullMatch", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("regex.fullMatch at " + token.loc.to_string() + "\nrequires two arguments str and pattern. eg. \nfullMatch(str, pattern [, flags]) -> bool");
            }
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : std::string();
            try {
                std::regex re = compile_re(pat, flags);
                bool res = std::regex_match(s, re);
                return Value{ res };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["fullMatch"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // search(str, pattern [, flags]) -> number (first match position) or -1; supports m
    {
        auto fn = make_native_fn("regex.search", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("regex.search at " + token.loc.to_string() + "\nrequires two arguments str and pattern. eg. \nsearch(str, pattern [, flags]) -> number (first match position) or -1; supports m");
            }
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : std::string();

            bool multiline = flags.find('m') != std::string::npos;

            try {
                std::regex re = compile_re(pat, flags);
                if (!multiline) {
                    std::smatch m;
                    if (std::regex_search(s, m, re)) {
                        return Value{ static_cast<double>(m.position()) };
                    }
                    return Value{ static_cast<double>(-1) };
                } else {
                    // scan lines and return absolute position of first match
                    size_t pos = 0;
                    size_t base = 0;
                    while (pos <= s.size()) {
                        size_t next = s.find('\n', pos);
                        size_t len = (next == std::string::npos) ? (s.size() - pos) : (next - pos);
                        std::string line = s.substr(pos, len);
                        std::smatch m;
                        if (std::regex_search(line, m, re)) {
                            return Value{ static_cast<double>(base + m.position()) };
                        }
                        if (next == std::string::npos) break;
                        base += (len + 1);
                        pos = next + 1;
                    }
                    return Value{ static_cast<double>(-1) };
                }
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["search"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // replace(str, pattern, replacement [, flags]) -> string
    // Behavior: by default replaces only the first match unless flags contains 'g' (global).
    // 'm' causes ^/$ to behave as line anchors by running replacements per-line.
    {
        auto fn = make_native_fn("regex.replace", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3) {
              throw std::runtime_error("regex.replace at" + token.loc.to_string() + "\nrequires 3 arguments str, pattern and replacement. eg. \nreplace(str, pattern, replacement [, flags]) -> string \n=> Behavior: by default replaces only the first match unless flags contains 'g' (global). \n=>'m' causes ^/$ to behave as line anchors by running replacements per-line. ");
            }
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string repl = value_to_string_simple(args[2]);
            std::string flags = args.size() >= 4 ? value_to_string_simple(args[3]) : std::string();

            bool global = flags.find('g') != std::string::npos;
            bool multiline = flags.find('m') != std::string::npos;

            try {
                std::regex re = compile_re(pat, flags);

                if (!multiline) {
                    if (global) {
                        std::string out = std::regex_replace(s, re, repl);
                        return Value{ out };
                    } else {
                        std::smatch m;
                        if (std::regex_search(s, m, re)) {
                            std::string before = s.substr(0, m.position());
                            std::string matched = s.substr(m.position(), m.length());
                            std::string replaced = std::regex_replace(matched, re, repl);
                            std::string out = before + replaced + s.substr(m.position() + m.length());
                            return Value{ out };
                        }
                        return Value{ s };
                    }
                } else {
                    // Multiline mode: operate on each line to emulate ^/$ behavior.
                    if (global) {
                        std::string out;
                        size_t pos = 0;
                        while (pos <= s.size()) {
                            size_t next = s.find('\n', pos);
                            size_t len = (next == std::string::npos) ? (s.size() - pos) : (next - pos);
                            std::string line = s.substr(pos, len);
                            std::string replacedLine = std::regex_replace(line, re, repl);
                            out += replacedLine;
                            if (next == std::string::npos) break;
                            out += '\n';
                            pos = next + 1;
                        }
                        return Value{ out };
                    } else {
                        // Non-global: find first match across lines and replace only that match
                        size_t pos = 0;
                        while (pos <= s.size()) {
                            size_t next = s.find('\n', pos);
                            size_t len = (next == std::string::npos) ? (s.size() - pos) : (next - pos);
                            std::string line = s.substr(pos, len);
                            std::smatch m;
                            if (std::regex_search(line, m, re)) {
                                // rebuild full string with replaced portion in this line
                                std::string beforeAll = s.substr(0, pos);
                                std::string beforeMatch = line.substr(0, m.position());
                                std::string matched = line.substr(m.position(), m.length());
                                std::string replaced = std::regex_replace(matched, re, repl);
                                std::string afterMatch = line.substr(m.position() + m.length());
                                std::string afterAll = (next == std::string::npos) ? std::string() : s.substr(next);
                                std::string out = beforeAll + beforeMatch + replaced + afterMatch + afterAll;
                                return Value{ out };
                            }
                            if (next == std::string::npos) break;
                            pos = next + 1;
                        }
                        return Value{ s };
                    }
                }
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["replace"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // split(str, pattern [, flags]) -> Array
    {
        auto fn = make_native_fn("regex.split", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("regex.split at " + token.loc.to_string() + "\nrequres two arguments str and pattern. eg. \nsplit(str, pattern [, flags]) -> Array");
            }
            std::string s = value_to_string_simple(args[0]);
            std::string pat = args.size() >= 2 ? value_to_string_simple(args[1]) : std::string();
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : std::string();
            auto arr = std::make_shared<ArrayValue>();
            if (pat.empty()) {
                for (char c : s) arr->elements.push_back(Value{ std::string(1, c) });
                return Value{ arr };
            }
            try {
                std::regex re = compile_re(pat, flags);
                std::sregex_token_iterator it(s.begin(), s.end(), re, -1), end;
                for (; it != end; ++it) arr->elements.push_back(Value{ it->str() });
                return Value{ arr };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["split"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

// ----------------- FS module (extended) -----------------
std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // readFile(path) -> string | null
    {
        auto fn = make_native_fn("fs.readFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("fs.readFile at " + token.loc.to_string() + "\nrequires a path as an argument, \nreadFile(path) -> string | null");
            }
            std::string path = value_to_string_simple(args[0]);
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) return Value{ std::monostate{} };
            std::ostringstream ss; ss << in.rdbuf();
            return Value{ ss.str() }; }, env);
        obj->properties["readFile"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // writeFile(path, content, [binary=false]) -> bool
    {
        auto fn = make_native_fn("fs.writeFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("fs.writeFile at " + token.loc.to_string() + "\nrequires two arguments, path and content, and a third optional bool [binary=false], \nfs.writeFile(path, content, [binary=false]) -> bool");
            }
            std::string path = value_to_string_simple(args[0]);
            std::string content = value_to_string_simple(args[1]);
            bool binary = false;
            if (args.size() >= 3) binary = std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;
            std::ofstream out;
            if (binary) out.open(path, std::ios::binary);
            else out.open(path);
            if (!out.is_open()) return Value{ false };
            out << content;
            return Value{ true }; }, env);
        obj->properties["writeFile"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // exists(path) -> bool
    {
        auto fn = make_native_fn("fs.exists", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("fs.exists at " + token.loc.to_string() + "\nrequires a path as an argument, exists(path) -> bool");
            }
            std::string path = value_to_string_simple(args[0]);
            return Value{ std::filesystem::exists(path) }; }, env);
        obj->properties["exists"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // listDir(path) -> array of entries (strings)
    {
        auto fn = make_native_fn("fs.listDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            std::string path = ".";
            if (!args.empty()) path = value_to_string_simple(args[0]);
            auto arr = std::make_shared<ArrayValue>();
            try {
                for (auto &p : std::filesystem::directory_iterator(path)) {
                    arr->elements.push_back(Value{ p.path().filename().string() });
                }
            } catch (...) {
                // return empty array on error
            }
            return Value{ arr }; }, env);
        obj->properties["listDir"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // copy(src, dest, [overwrite=false]) -> bool
    {
        auto fn = make_native_fn("fs.copy", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("fs.copy at " + token.loc.to_string() + "\nrequires atleast two arguments src and dest and a third optional overwrite flag, copy(src, dest, [overwrite=false]) -> bool");
            };
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = false;
            if (args.size() >= 3) overwrite = std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;
            try {
                std::filesystem::copy_options opts = std::filesystem::copy_options::none;
                if (overwrite) opts = static_cast<std::filesystem::copy_options>(opts | std::filesystem::copy_options::overwrite_existing);
                std::filesystem::copy(src, dest, opts);
                return Value{ true };
            } catch (const std::filesystem::filesystem_error &e) {
                throw std::runtime_error(std::string("fs.copy failed at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["copy"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // move(src, dest, [overwrite=false]) -> bool
    {
        auto fn = make_native_fn("fs.move", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw std::runtime_error("fs.move at " + token.loc.to_string() + "\nrequires two arguments src and dest and an optional overwrite flag, move(src, dest, [overwrite=false]) -> bool");
            }
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = false;
            if (args.size() >= 3) overwrite = std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;
            try {
                if (std::filesystem::exists(dest)) {
                    if (!overwrite) return Value{ false };
                    std::filesystem::remove_all(dest);
                }
                std::filesystem::rename(src, dest);
                return Value{ true };
            } catch (const std::filesystem::filesystem_error &e) {
                // fallback to copy+remove for cross-device moves
                try {
                    std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive);
                    std::filesystem::remove_all(src);
                    return Value{ true };
                } catch (const std::filesystem::filesystem_error &e2) {
                    throw std::runtime_error(std::string("fs.move failed at ") + token.loc.to_string() + ": " + e.what() + " / " + e2.what());
                }
            } }, env);
        obj->properties["move"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // remove(path) -> bool  (files or directories; directories removed recursively)
    {
        auto fn = make_native_fn("fs.remove", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("fs.remove at " +token.loc.to_string() + "\nrequires a path as argument, remove(path) -> bool  (files or directories; directories removed recursively)");
            }
            std::string path = value_to_string_simple(args[0]);
            try {
                if (!std::filesystem::exists(path)) return Value{ false };
                std::uintmax_t removed = std::filesystem::remove_all(path);
                return Value{ removed > 0 };
            } catch (const std::filesystem::filesystem_error &e) {
                throw std::runtime_error(std::string("fs.remove failed at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["remove"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // makeDir(path, [recursive=true]) -> bool (does not error if dir already exists)
    {
        auto fn = make_native_fn("fs.makeDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("fs.makeDir at" + token.loc.to_string() + "\nrequires a dir path as an argument and an optional recursive flag,  makeDir(path, [recursive=true]) -> bool (does not error if dir already exists)");
            }
            std::string path = value_to_string_simple(args[0]);
            bool recursive = true;
            if (args.size() >= 2) recursive = std::holds_alternative<bool>(args[1]) ? std::get<bool>(args[1]) : true;
            try {
                if (std::filesystem::exists(path)) return Value{ std::filesystem::is_directory(path) };
                bool ok = recursive ? std::filesystem::create_directories(path) : std::filesystem::create_directory(path);
                return Value{ ok };
            } catch (const std::filesystem::filesystem_error &e) {
                throw std::runtime_error(std::string("fs.makeDir failed at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["makeDir"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // stat(path) -> object { exists, isFile, isDir, size, modifiedAt (ISO8601), permissions }
    {
        auto fn = make_native_fn("fs.stat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            auto obj = std::make_shared<ObjectValue>();
            if (args.empty()) {
              throw std::runtime_error("fs.stat at " + token.loc.to_string() + "\nrequires a path as an argument, stat(path) -> object { exists, isFile, isDir, size, modifiedAt (ISO8601), permissions }");
            }
            std::string path = value_to_string_simple(args[0]);
            try {
                if (!std::filesystem::exists(path)) {
                    obj->properties["exists"] = PropertyDescriptor{ Value{ false }, false, false, true, Token() };
                    return Value{ obj };
                }
                auto status = std::filesystem::status(path);
                bool isFile = std::filesystem::is_regular_file(status);
                bool isDir = std::filesystem::is_directory(status);
                uintmax_t size = 0;
                if (isFile) size = std::filesystem::file_size(path);
                // modified time
                std::string modifiedStr;
                try {
                    auto ftime = std::filesystem::last_write_time(path);
                    auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - decltype(ftime)::clock::now()
                        + std::chrono::system_clock::now());
                    std::time_t tt = std::chrono::system_clock::to_time_t(st);
                    std::tm tm{};
#ifdef _MSC_VER
                    gmtime_s(&tm, &tt);
#else
                    gmtime_r(&tt, &tm);
#endif
                    char buf[64];
                    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                    modifiedStr = buf;
                } catch (...) {
                    modifiedStr = "";
                }

                // permissions summary (simple)
                auto perms = status.permissions();
                std::string permSummary;
                permSummary += ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? "x" : "-";

                obj->properties["exists"] = PropertyDescriptor{ Value{ true }, false, false, true, Token() };
                obj->properties["isFile"] = PropertyDescriptor{ Value{ isFile }, false, false, true, Token() };
                obj->properties["isDir"] = PropertyDescriptor{ Value{ isDir }, false, false, true, Token() };
                obj->properties["size"] = PropertyDescriptor{ Value{ static_cast<double>(size) }, false, false, true, Token() };
                obj->properties["modifiedAt"] = PropertyDescriptor{ Value{ modifiedStr }, false, false, true, Token() };
                obj->properties["permissions"] = PropertyDescriptor{ Value{ permSummary }, false, false, true, Token() };
                return Value{ obj };
            } catch (const std::filesystem::filesystem_error &e) {
                throw std::runtime_error(std::string("fs.stat failed at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["stat"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    return obj;
}

// ----------------- HTTP module (uses libcurl if available; supports GET and POST) -----------------
std::shared_ptr<ObjectValue> make_http_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

#if defined(HAVE_LIBCURL)
    // http.get(url, [headers_array]) -> string body
    {
        auto fn = make_native_fn("http.get", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value{ std::monostate{} };
            std::string url = value_to_string_simple(args[0]);

            CURL* c = curl_easy_init();
            if (!c) throw std::runtime_error("curl_easy_init failed");
            
            CurlWriteCtx ctx;  // Use the struct defined at file scope
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);  // Use global function
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);

            // optional headers array (array of strings)
            struct curl_slist *headers = nullptr;
            if (args.size() >= 2 && std::holds_alternative<ArrayPtr>(args[1])) {
                ArrayPtr headerArr = std::get<ArrayPtr>(args[1]);
                for (auto &hv : headerArr->elements) {
                    std::string hs = value_to_string_simple(hv);
                    headers = curl_slist_append(headers, hs.c_str());
                }
                if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
            }

            CURLcode res = curl_easy_perform(c);
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(c);
            
            if (res != CURLE_OK) {
                throw std::runtime_error(std::string("http.get failed: ") + curl_easy_strerror(res) + " at " + token.loc.to_string());
            }
            return Value{ ctx.buf };
        }, env);
        obj->properties["get"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // http.post(url, body, [contentType="application/json"], [headers_array]) -> string body
    {
        auto fn = make_native_fn("http.post", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) return Value{ std::monostate{} };
            std::string url = value_to_string_simple(args[0]);
            std::string body = value_to_string_simple(args[1]);
            std::string contentType = "application/json";
            if (args.size() >= 3) contentType = value_to_string_simple(args[2]);

            CURL* c = curl_easy_init();
            if (!c) throw std::runtime_error("curl_easy_init failed");
            
            CurlWriteCtx ctx;  // Use the struct defined at file scope
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_POST, 1L);
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);  // Use global function
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);

            struct curl_slist *headers = nullptr;
            std::string cth = "Content-Type: " + contentType;
            headers = curl_slist_append(headers, cth.c_str());
            if (args.size() >= 4 && std::holds_alternative<ArrayPtr>(args[3])) {
                ArrayPtr headerArr = std::get<ArrayPtr>(args[3]);
                for (auto &hv : headerArr->elements) {
                    std::string hs = value_to_string_simple(hv);
                    headers = curl_slist_append(headers, hs.c_str());
                }
            }
            if (headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);

            CURLcode res = curl_easy_perform(c);
            if (headers) curl_slist_free_all(headers);
            curl_easy_cleanup(c);
            
            if (res != CURLE_OK) {
                throw std::runtime_error(std::string("http.post failed: ") + curl_easy_strerror(res) + " at " + token.loc.to_string());
            }
            return Value{ ctx.buf };
        }, env);
        obj->properties["post"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // http.createServer(handler) -> throws (not implemented in builtin). We provide a clear error.
    {
        auto fn = make_native_fn("http.createServer", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
            throw std::runtime_error("http.createServer is not provided by the builtin module. For running servers, please use an external server module or bind to a library (e.g., cpp-httplib, boost::asio or similar). Called at " + token.loc.to_string());
        }, env);
        obj->properties["createServer"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

#else
    // Stubs when libcurl is not available
    auto fn_get = make_native_fn("http.get", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
        throw std::runtime_error("http.get native module requires libcurl support. Build with libcurl or provide an external http module. (called at " + token.loc.to_string() + ")");
    }, env);
    obj->properties["get"] = PropertyDescriptor{fn_get, false, false, false, Token()};

    auto fn_post = make_native_fn("http.post", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
        throw std::runtime_error("http.post native module requires libcurl support. Build with libcurl or provide an external http module. (called at " + token.loc.to_string() + ")");
    }, env);
    obj->properties["post"] = PropertyDescriptor{fn_post, false, false, false, Token()};

    auto fn_server = make_native_fn("http.createServer", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
        throw std::runtime_error("http.createServer is not available in builtin without a server backend. Called at " + token.loc.to_string());
    }, env);
    obj->properties["createServer"] = PropertyDescriptor{fn_server, false, false, false, Token()};
#endif

    return obj;
}
// ----------------- JSON module (parse & stringify) -----------------
namespace {
// Minimal JSON parser / stringify routines
struct JsonParser {
    const std::string& s;
    size_t i = 0;
    JsonParser(const std::string& str) : s(str), i(0) {
    }

    void skip() {
        while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    }

    bool match(char c) {
        skip();
        if (i < s.size() && s[i] == c) {
            ++i;
            return true;
        }
        return false;
    }

    Value parseValue() {
        skip();
        if (i >= s.size()) return Value{std::monostate{}};
        char c = s[i];
        if (c == '{') return parseObject();
        if (c == '[') return parseArray();
        if (c == '"') return Value{parseString()};
        if (c == 'n' && s.compare(i, 4, "null") == 0) {
            i += 4;
            return Value{std::monostate{}};
        }
        if (c == 't' && s.compare(i, 4, "true") == 0) {
            i += 4;
            return Value{true};
        }
        if (c == 'f' && s.compare(i, 5, "false") == 0) {
            i += 5;
            return Value{false};
        }
        // number
        return parseNumber();
    }

    std::string parseString() {
        // assumes starting quote is at s[i]
        ++i;  // skip "
        std::ostringstream out;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') break;
            if (c == '\\' && i < s.size()) {
                char esc = s[i++];
                switch (esc) {
                    case '"':
                        out << '"';
                        break;
                    case '\\':
                        out << '\\';
                        break;
                    case '/':
                        out << '/';
                        break;
                    case 'b':
                        out << '\b';
                        break;
                    case 'f':
                        out << '\f';
                        break;
                    case 'n':
                        out << '\n';
                        break;
                    case 'r':
                        out << '\r';
                        break;
                    case 't':
                        out << '\t';
                        break;
                    // Unicode escapes \uXXXX are ignored/naively handled
                    default:
                        out << esc;
                        break;
                }
            } else {
                out << c;
            }
        }
        return out.str();
    }

    Value parseNumber() {
        skip();
        size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        if (i < s.size() && s[i] == '.') {
            ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            while (i < s.size() && std::isdigit((unsigned char)s[i])) ++i;
        }
        double val = 0.0;
        try {
            val = std::stod(s.substr(start, i - start));
        } catch (...) {
            val = 0.0;
        }
        return Value{val};
    }

    Value parseArray() {
        // assume '['
        ++i;
        auto arr = std::make_shared<ArrayValue>();
        skip();
        if (match(']')) return Value{arr};
        while (i < s.size()) {
            Value v = parseValue();
            arr->elements.push_back(v);
            skip();
            if (match(',')) continue;
            if (match(']')) break;
        }
        return Value{arr};
    }

    Value parseObject() {
        ++i;  // skip {
        auto obj = std::make_shared<ObjectValue>();
        skip();
        if (match('}')) return Value{obj};
        while (i < s.size()) {
            skip();
            if (i >= s.size() || s[i] != '"') break;
            std::string key = parseString();
            skip();
            match(':');
            Value val = parseValue();
            PropertyDescriptor pd;
            pd.value = val;
            pd.is_private = false;
            pd.is_readonly = false;
            pd.is_locked = false;
            obj->properties[key] = pd;
            skip();
            if (match(',')) continue;
            if (match('}')) break;
        }
        return Value{obj};
    }
};

// stringify Value back to JSON
static std::string json_stringify_value(const Value& v);

static std::string json_stringify_string(const std::string& s) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c << std::dec;
                } else
                    out << c;
        }
    }
    out << '"';
    return out.str();
}

static std::string json_stringify_value(const Value& v, std::unordered_set<const ObjectValue*> objvisited = {}, std::unordered_set<const ArrayValue*> arrvisited = {}, Token token = {}) {
    if (std::holds_alternative<std::monostate>(v)) return "null";
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<std::string>(v)) {
        return json_stringify_string(std::get<std::string>(v));
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    if (std::holds_alternative<ArrayPtr>(v)) {
        ArrayPtr a = std::get<ArrayPtr>(v);
        
        ArrayValue* p = a.get();
        if(arrvisited.count(p)) {
          throw std::runtime_error("Converting circular structure to JSON at" + token.loc.to_string() + "\n--> Traced at \n * 12 | chapisha(JSON.stringify(ob)) \n" + std::string(token.loc.col + 8, ' ') + "^");
        };
        arrvisited.insert(p);
        
        std::ostringstream ss;
        ss << '[';
        bool first = true;
        for (auto& el : a->elements) {
            if (!first) ss << ',';
            first = false;
            ss << json_stringify_value(el, objvisited, arrvisited, token);
        }
        ss << ']';
        return ss.str();
    }
    if (std::holds_alternative<ObjectPtr>(v)) {
        ObjectPtr o = std::get<ObjectPtr>(v);
        
        ObjectValue* p = o.get();
        if(objvisited.count(p)) {
          throw std::runtime_error("Converting circular structure to JSON at" + token.loc.to_string() + "\n--> Traced at \n * 12 |  code.in.this.line() \n");
        };
        objvisited.insert(p);
        
        std::ostringstream ss;
        ss << '{';
        bool first = true;
        for (auto& kv : o->properties) {
            if (!first) ss << ',';
            first = false;
            ss << json_stringify_string(kv.first) << ':' << json_stringify_value(kv.second.value, objvisited, arrvisited, token);
        }
        ss << '}';
        return ss.str();
    }
    // functions / classes -> null
    return "null";
}
}  // namespace

std::shared_ptr<ObjectValue> make_json_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // json.parse(str) -> Value
    {
        auto fn = make_native_fn("json.parse", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value{ std::monostate{} };
            std::string txt = value_to_string_simple(args[0]);
            try {
                JsonParser p(txt);
                Value v = p.parseValue();
                return v;
            } catch (const std::exception &e) {
                throw std::runtime_error(std::string("json.parse failed at ") + token.loc.to_string() + ": " + e.what());
            } }, env);
        obj->properties["parse"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // json.stringify(val) -> string
    {
        auto fn = make_native_fn("json.stringify", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value();
            std::unordered_set<const ObjectValue*> objvisited;
            std::unordered_set<const ArrayValue*> arrvisited;
            return Value{ json_stringify_value(args[0], objvisited, arrvisited, token) }; }, env);
        obj->properties["stringify"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

// ----------------- PATH module -----------------
std::shared_ptr<ObjectValue> make_path_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // join(...segments) -> string
    {
        auto fn = make_native_fn("path.join", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("path.join at " + token.loc.to_string() + "\nrequires at least one path segments to join, join(...segments) -> string");
            }
            fs::path p;
            for (const auto &a : args) {
                p /= value_to_string_simple(a);
            }
            return Value{ p.lexically_normal().string() }; }, env);
        obj->properties["join"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
    // basename(path)
    {
        auto fn = make_native_fn("path.basename", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("path.basename at " + token.loc.to_string() + "\nrequires a path argument to extract basename from, basename(path) -> string");
            }
            fs::path p = value_to_string_simple(args[0]);
            return Value{ p.filename().string() }; }, env);
        obj->properties["basename"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
    // dirname(path)
    {
        auto fn = make_native_fn("path.dirname", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw std::runtime_error("path.dirname at " + token.loc.to_string() + "\nrequires a path to extract dirname from, dirname(path)");
            }
            fs::path p = value_to_string_simple(args[0]);
            return Value{ p.parent_path().string() }; }, env);
        obj->properties["dirname"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
    // extname(path)
    {
        auto fn = make_native_fn("path.extname", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.empty()) return Value{ std::string() };
            fs::path p = value_to_string_simple(args[0]);
            return Value{ p.extension().string() }; }, env);
        obj->properties["extname"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
    // resolve(...segments)
    {
        auto fn = make_native_fn("path.resolve", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.empty()) return Value{ std::string() };
            fs::path p;
            for (const auto &a : args) {
                p /= value_to_string_simple(a);
            }
            try {
                p = fs::weakly_canonical(p);
                return Value{ p.string() };
            } catch (...) {
                return Value{ p.lexically_normal().string() };
            } }, env);
        obj->properties["resolve"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

// ----------------- OS module -----------------
std::shared_ptr<ObjectValue> make_os_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // os.platform() -> string
    {
        auto fn = make_native_fn("os.platform", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
#ifdef _WIN32
            return Value{std::string("windows")};
#elif __APPLE__
            return Value{std::string("macos")};
#elif __linux__
            return Value{std::string("linux")};
#else
            return Value{std::string("unknown")};
#endif
        },
            env);
        obj->properties["platform"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // os.cwd() -> string
    {
        auto fn = make_native_fn("os.cwd", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value { return Value{fs::current_path().string()}; }, env);
        obj->properties["cwd"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // os.hostname() -> string
    {
        auto fn = make_native_fn("os.hostname", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            char buf[256] = {0};
#ifdef _WIN32
            DWORD len = sizeof(buf);
            if (GetComputerNameA(buf, &len)) return Value{ std::string(buf) };
#else
            if (gethostname(buf, sizeof(buf)) == 0) return Value{ std::string(buf) };
#endif
            return Value{ std::string() }; }, env);
        obj->properties["hostname"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // os.tmpdir() -> string
    {
        auto fn = make_native_fn("os.tmpdir", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            const char* tmp = std::getenv("TMPDIR");
            if (!tmp) tmp = std::getenv("TMP");
            if (!tmp) tmp = std::getenv("TEMP");
            if (tmp) return Value{std::string(tmp)};
#ifdef _WIN32
            return Value{std::string("C:\\Windows\\Temp")};
#else
            return Value{std::string("/tmp")};
#endif
        },
            env);
        obj->properties["tmpdir"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // os.cpus() -> number (count)
    {
        auto fn = make_native_fn("os.cpus", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            unsigned int n = std::thread::hardware_concurrency();
            if (n == 0) n = 1;
            return Value{ static_cast<double>(n) }; }, env);
        obj->properties["cpus"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

// ----------------- PROCESS module -----------------
std::shared_ptr<ObjectValue> make_process_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // process.getEnv(name) -> string|null
    {
        auto fn = make_native_fn("process.getEnv", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.empty()) return Value{ std::monostate{} };
            std::string name = value_to_string_simple(args[0]);
            const char* v = std::getenv(name.c_str());
            if (!v) return Value{ std::monostate{} };
            return Value{ std::string(v) }; }, env);
        obj->properties["getEnv"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // process.setEnv(name, value) -> bool
    {
        auto fn = make_native_fn("process.setEnv", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.size() < 2) return Value{false};
            std::string name = value_to_string_simple(args[0]);
            std::string val = value_to_string_simple(args[1]);
#ifdef _WIN32
            std::string setting = name + "=" + val;
            int res = _putenv(setting.c_str());
            return Value{res == 0};
#else
            int res = setenv(name.c_str(), val.c_str(), 1);
            return Value{res == 0};
#endif
        },
            env);
        obj->properties["setEnv"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // process.unsetEnv(name) -> bool
    {
        auto fn = make_native_fn("process.unsetEnv", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.empty()) return Value{false};
            std::string name = value_to_string_simple(args[0]);
#ifdef _WIN32
            std::string setting = name + "=";
            int res = _putenv(setting.c_str());
            return Value{res == 0};
#else
            int res = unsetenv(name.c_str());
            return Value{res == 0};
#endif
        },
            env);
        obj->properties["unsetEnv"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // process.pid() -> number
    {
        auto fn = make_native_fn("process.pid", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
#ifdef _WIN32
            return Value{static_cast<double>(GetCurrentProcessId())};
#else
            return Value{static_cast<double>(getpid())};
#endif
        },
            env);
        obj->properties["pid"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}