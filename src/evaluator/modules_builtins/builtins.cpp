#include "builtins.hpp"

#include <algorithm>
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

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "uv.h"

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
// Replace the existing CurlWriteCtx and add a header callback right after curl_write_cb:

struct CurlWriteCtx {
    std::string buf;      // response body
    std::string headers;  // raw header data (including status line)
};

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    CurlWriteCtx* ctx = static_cast<CurlWriteCtx*>(userdata);
    if (ctx) ctx->buf.append(ptr, total);
    return total;
}

// New: header callback to collect headers (including the status line)
static size_t curl_header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    CurlWriteCtx* ctx = static_cast<CurlWriteCtx*>(userdata);
    if (ctx) ctx->headers.append(buffer, total);
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
              throw SwaziError("RuntimeError",
                "regex.match requires at least two arguments: str and pattern. Usage: match(str, pattern [, flags]) -> array|null (supports flags like i, g, m)",
                token.loc);
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
                throw SwaziError("RegexError", std::string("regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["match"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // test(str, pattern [, flags]) -> bool ( supports m)
    {
        auto fn = make_native_fn("regex.test", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw SwaziError("RuntimeError",
                "regex.test requires at least two arguments: str and pattern. Usage: test(str, pattern [, flags]) -> bool (supports m)",
                token.loc);
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
                throw SwaziError("RegexError", std::string("regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["test"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // fullMatch(str, pattern [, flags]) -> bool
    {
        auto fn = make_native_fn("regex.fullMatch", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw SwaziError("RuntimeError",
                "regex.fullMatch requires two arguments: str and pattern. Usage: fullMatch(str, pattern [, flags]) -> bool",
                token.loc);
            }
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : std::string();
            try {
                std::regex re = compile_re(pat, flags);
                bool res = std::regex_match(s, re);
                return Value{ res };
            } catch (const std::regex_error &e) {
                throw SwaziError("RegexError", std::string("regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["fullMatch"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // search(str, pattern [, flags]) -> number (first match position) or -1; supports m
    {
        auto fn = make_native_fn("regex.search", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw SwaziError("RuntimeError",
                "regex.search requires two arguments: str and pattern. Usage: search(str, pattern [, flags]) -> number (first match position) or -1; supports m",
                token.loc);
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
                throw SwaziError("RegexError", std::string("regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["search"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // replace(str, pattern, replacement [, flags]) -> string
    // Behavior: by default replaces only the first match unless flags contains 'g' (global).
    // 'm' causes ^/$ to behave as line anchors by running replacements per-line.
    {
        auto fn = make_native_fn("regex.replace", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3) {
              throw SwaziError("RuntimeError",
                "regex.replace requires 3 arguments: str, pattern and replacement. Usage: replace(str, pattern, replacement [, flags]) -> string. Behavior: by default replaces only the first match unless flags contains 'g' (global). 'm' causes ^/$ to behave as line anchors by running replacements per-line.",
                token.loc);
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
                throw SwaziError("RegexError", std::string("regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["replace"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // split(str, pattern [, flags]) -> Array
    {
        auto fn = make_native_fn("regex.split", [compile_re](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError",
                "regex.split requires two arguments: str and pattern. Usage: split(str, pattern [, flags]) -> Array",
                token.loc);
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
                throw SwaziError("RegexError", std::string("regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["split"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

// ----------------- FS module (extended) -----------------
std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // fs.readFile(path, options?) -> string | Buffer
    // options can be: { encoding: "utf8" | "binary" | null, flag: "r" }
    // If encoding is null or "binary", returns Buffer. Otherwise returns string.
    {
        auto fn = make_native_fn("fs.readFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.readFile requires a path as an argument. Usage: readFile(path, options?) -> string | Buffer", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            
            // Parse options
            std::string encoding = "utf8";  // default
            if (args.size() >= 2) {
                if (std::holds_alternative<std::string>(args[1])) {
                    // Simple string encoding
                    encoding = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<ObjectPtr>(args[1])) {
                    // Options object
                    ObjectPtr opts = std::get<ObjectPtr>(args[1]);
                    auto enc_it = opts->properties.find("encoding");
                    if (enc_it != opts->properties.end()) {
                        if (std::holds_alternative<std::monostate>(enc_it->second.value)) {
                            encoding = "binary";  // null means binary
                        } else if (std::holds_alternative<std::string>(enc_it->second.value)) {
                            encoding = std::get<std::string>(enc_it->second.value);
                        }
                    }
                }
            }
            
            // Read file in binary mode always
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                throw SwaziError("IOError", "Failed to open file: " + path, token.loc);
            }
            
            // Read all bytes
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
            
            // Return based on encoding
            if (encoding == "binary" || encoding == "null") {
                auto buf = std::make_shared<BufferValue>();
                buf->data = std::move(data);
                buf->encoding = "binary";
                return Value{buf};
            } else {
                // Convert to string (utf8 or other encoding)
                return Value{std::string(data.begin(), data.end())};
            } }, env);
        obj->properties["readFile"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // fs.writeFile(path, content, options?) -> bool
    // content can be string or Buffer
    // options can be: { encoding: "utf8", flag: "w", mode: 0o666 }
    // If content is Buffer, writes in binary mode automatically
    {
        auto fn = make_native_fn("fs.writeFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.writeFile requires two arguments: path and content, and an optional options object. Usage: fs.writeFile(path, content, options?) -> bool", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            
            // Determine if we're writing buffer or string
            bool is_buffer = std::holds_alternative<BufferPtr>(args[1]);
            std::string encoding = "utf8";
            std::string flag = "w";
            
            // Parse options
            if (args.size() >= 3) {
                if (std::holds_alternative<std::string>(args[2])) {
                    // Simple string encoding (legacy support)
                    encoding = std::get<std::string>(args[2]);
                } else if (std::holds_alternative<ObjectPtr>(args[2])) {
                    ObjectPtr opts = std::get<ObjectPtr>(args[2]);
                    auto enc_it = opts->properties.find("encoding");
                    if (enc_it != opts->properties.end() && std::holds_alternative<std::string>(enc_it->second.value)) {
                        encoding = std::get<std::string>(enc_it->second.value);
                    }
                    auto flag_it = opts->properties.find("flag");
                    if (flag_it != opts->properties.end() && std::holds_alternative<std::string>(flag_it->second.value)) {
                        flag = std::get<std::string>(flag_it->second.value);
                    }
                }
            }
            
            // Determine open mode
            std::ios_base::openmode mode = std::ios::binary;
            if (flag == "a" || flag == "a+") {
                mode |= std::ios::app;
            } else if (flag == "r+") {
                mode |= std::ios::in | std::ios::out;
            }
            // "w" is default (truncate)
            
            std::ofstream out(path, mode);
            if (!out.is_open()) {
                throw SwaziError("IOError", "Failed to open file for writing: " + path, token.loc);
            }
            
            if (is_buffer) {
                // Write buffer directly as binary
                BufferPtr buf = std::get<BufferPtr>(args[1]);
                out.write(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
            } else {
                // Write string (respect encoding if needed)
                std::string content = value_to_string_simple(args[1]);
                out.write(content.data(), content.size());
            }
            
            return Value{true}; }, env);
        obj->properties["writeFile"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // exists(path) -> bool
    {
        auto fn = make_native_fn("fs.exists", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.exists requires a path as an argument. Usage: exists(path) -> bool", token.loc);
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
              throw SwaziError("RuntimeError", "fs.copy requires at least two arguments: src and dest, and an optional overwrite flag. Usage: copy(src, dest, [overwrite=false]) -> bool", token.loc);
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
                throw SwaziError("FilesystemError", std::string("fs.copy failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["copy"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // move(src, dest, [overwrite=false]) -> bool
    {
        auto fn = make_native_fn("fs.move", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw SwaziError("RuntimeError", "fs.move requires two arguments: src and dest, and an optional overwrite flag. Usage: move(src, dest, [overwrite=false]) -> bool", token.loc);
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
                    throw SwaziError("FilesystemError", std::string("fs.move failed: ") + e.what() + " / " + e2.what(), token.loc);
                }
            } }, env);
        obj->properties["move"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // remove(path) -> bool  (files or directories; directories removed recursively)
    {
        auto fn = make_native_fn("fs.remove", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.remove requires a path as argument. Usage: remove(path) -> bool  (files or directories; directories removed recursively)", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            try {
                if (!std::filesystem::exists(path)) return Value{ false };
                std::uintmax_t removed = std::filesystem::remove_all(path);
                return Value{ removed > 0 };
            } catch (const std::filesystem::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.remove failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["remove"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // makeDir(path, [recursive=true]) -> bool (does not error if dir already exists)
    {
        auto fn = make_native_fn("fs.makeDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.makeDir requires a dir path as an argument and an optional recursive flag. Usage: makeDir(path, [recursive=true]) -> bool (does not error if dir already exists)", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            bool recursive = true;
            if (args.size() >= 2) recursive = std::holds_alternative<bool>(args[1]) ? std::get<bool>(args[1]) : true;
            try {
                if (std::filesystem::exists(path)) return Value{ std::filesystem::is_directory(path) };
                bool ok = recursive ? std::filesystem::create_directories(path) : std::filesystem::create_directory(path);
                return Value{ ok };
            } catch (const std::filesystem::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.makeDir failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["makeDir"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // stat(path) -> object { exists, isFile, isDir, size, modifiedAt (ISO8601), permissions }
    {
        auto fn = make_native_fn("fs.stat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            auto obj = std::make_shared<ObjectValue>();
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.stat requires a path as an argument. Usage: stat(path) -> object { exists, isFile, isDir, size, modifiedAt (ISO8601), permissions }", token.loc);
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
                throw SwaziError("FilesystemError", std::string("fs.stat failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["stat"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    {  // ============= fs.promises API =============
        // Create promises sub-object for async versions
        auto promises_obj = std::make_shared<ObjectValue>();

        // promises.readFile(path, options?) -> Promise<string | Buffer>
        {
            auto fn = make_native_fn("fs.promises.readFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("RuntimeError", "fs.promises.readFile requires a path argument", token.loc);
                }
                std::string path = value_to_string_simple(args[0]);
                
                // Parse options
                std::string encoding = "utf8";
                if (args.size() >= 2) {
                    if (std::holds_alternative<std::string>(args[1])) {
                        encoding = std::get<std::string>(args[1]);
                    } else if (std::holds_alternative<ObjectPtr>(args[1])) {
                        ObjectPtr opts = std::get<ObjectPtr>(args[1]);
                        auto enc_it = opts->properties.find("encoding");
                        if (enc_it != opts->properties.end()) {
                            if (std::holds_alternative<std::monostate>(enc_it->second.value)) {
                                encoding = "binary";
                            } else if (std::holds_alternative<std::string>(enc_it->second.value)) {
                                encoding = std::get<std::string>(enc_it->second.value);
                            }
                        }
                    }
                }

                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;

                // Schedule async read on loop thread
                scheduler_run_on_loop([promise, path, encoding]() {
                    try {
                        std::ifstream in(path, std::ios::binary);
                        if (!in.is_open()) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Failed to open file: ") + path};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        // Read all bytes
                        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
                        
                        // Return based on encoding
                        Value result;
                        if (encoding == "binary" || encoding == "null") {
                            auto buf = std::make_shared<BufferValue>();
                            buf->data = std::move(data);
                            buf->encoding = "binary";
                            result = Value{buf};
                        } else {
                            result = Value{std::string(data.begin(), data.end())};
                        }
                        
                        // Fulfill promise
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = result;
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Read error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });

                return Value{promise}; }, env);
            promises_obj->properties["readFile"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.writeFile(path, content, options?) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.writeFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.size() < 2) {
                    throw SwaziError("RuntimeError", "fs.promises.writeFile requires path and content arguments", token.loc);
                }
                std::string path = value_to_string_simple(args[0]);
                
                // Capture content (could be string or buffer)
                Value content = args[1];
                bool is_buffer = std::holds_alternative<BufferPtr>(content);
                
                std::string encoding = "utf8";
                std::string flag = "w";
                
                // Parse options
                if (args.size() >= 3) {
                    if (std::holds_alternative<std::string>(args[2])) {
                        encoding = std::get<std::string>(args[2]);
                    } else if (std::holds_alternative<ObjectPtr>(args[2])) {
                        ObjectPtr opts = std::get<ObjectPtr>(args[2]);
                        auto enc_it = opts->properties.find("encoding");
                        if (enc_it != opts->properties.end() && std::holds_alternative<std::string>(enc_it->second.value)) {
                            encoding = std::get<std::string>(enc_it->second.value);
                        }
                        auto flag_it = opts->properties.find("flag");
                        if (flag_it != opts->properties.end() && std::holds_alternative<std::string>(flag_it->second.value)) {
                            flag = std::get<std::string>(flag_it->second.value);
                        }
                    }
                }

                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;

                scheduler_run_on_loop([promise, path, content, is_buffer, flag]() {
                    try {
                        std::ios_base::openmode mode = std::ios::binary;
                        if (flag == "a" || flag == "a+") {
                            mode |= std::ios::app;
                        } else if (flag == "r+") {
                            mode |= std::ios::in | std::ios::out;
                        }
                        
                        std::ofstream out(path, mode);
                        if (!out.is_open()) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Failed to open file for writing: ") + path};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        if (is_buffer) {
                            BufferPtr buf = std::get<BufferPtr>(content);
                            out.write(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
                        } else {
                            std::string str = value_to_string_simple(content);
                            out.write(str.data(), str.size());
                        }
                        
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{true};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Write error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });

                return Value{promise}; }, env);
            promises_obj->properties["writeFile"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.exists(path) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.exists", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.exists requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                bool exists = std::filesystem::exists(path);
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{exists};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["exists"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.listDir(path) -> Promise<array>
        {
            auto fn = make_native_fn("fs.promises.listDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            std::string path = args.empty() ? "." : value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                try {
                    auto arr = std::make_shared<ArrayValue>();
                    for (auto& p : std::filesystem::directory_iterator(path)) {
                        arr->elements.push_back(Value{p.path().filename().string()});
                    }
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{arr};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::exception& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("List dir error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["listDir"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.copy(src, dest, [overwrite=false]) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.copy", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.promises.copy requires src and dest arguments", token.loc);
            }
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = args.size() >= 3 && std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, src, dest, overwrite]() {
                try {
                    std::filesystem::copy_options opts = std::filesystem::copy_options::none;
                    if (overwrite) opts = static_cast<std::filesystem::copy_options>(opts | std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::copy(src, dest, opts);
                    
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{true};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("Copy error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["copy"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.move(src, dest, [overwrite=false]) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.move", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.promises.move requires src and dest arguments", token.loc);
            }
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = args.size() >= 3 && std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, src, dest, overwrite]() {
                try {
                    if (std::filesystem::exists(dest)) {
                        if (!overwrite) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Destination exists and overwrite is false")};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        std::filesystem::remove_all(dest);
                    }
                    std::filesystem::rename(src, dest);
                    
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{true};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error&) {
                    // Fallback: copy + remove for cross-device moves
                    try {
                        std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive);
                        std::filesystem::remove_all(src);
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{true};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Move error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["move"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.remove(path) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.remove", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.remove requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                try {
                    if (!std::filesystem::exists(path)) {
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{false};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                        return;
                    }
                    std::uintmax_t removed = std::filesystem::remove_all(path);
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{removed > 0};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("Remove error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["remove"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.makeDir(path, [recursive=true]) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.makeDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.makeDir requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            bool recursive = args.size() >= 2 && std::holds_alternative<bool>(args[1]) ? std::get<bool>(args[1]) : true;

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path, recursive]() {
                try {
                    if (std::filesystem::exists(path)) {
                        bool is_dir = std::filesystem::is_directory(path);
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{is_dir};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                        return;
                    }
                    bool ok = recursive ? std::filesystem::create_directories(path) : std::filesystem::create_directory(path);
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{ok};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("MakeDir error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["makeDir"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.stat(path) -> Promise<object>
        {
            auto fn = make_native_fn("fs.promises.stat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.stat requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                try {
                    auto obj = std::make_shared<ObjectValue>();
                    
                    if (!std::filesystem::exists(path)) {
                        obj->properties["exists"] = PropertyDescriptor{Value{false}, false, false, true, Token()};
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{obj};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                        return;
                    }

                    auto status = std::filesystem::status(path);
                    bool isFile = std::filesystem::is_regular_file(status);
                    bool isDir = std::filesystem::is_directory(status);
                    uintmax_t size = isFile ? std::filesystem::file_size(path) : 0;

                    std::string modifiedStr;
                    try {
                        auto ftime = std::filesystem::last_write_time(path);
                        auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                            ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
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

                    auto perms = status.permissions();
                    std::string permSummary;
                    permSummary += ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) ? "r" : "-";
                    permSummary += ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none) ? "w" : "-";
                    permSummary += ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? "x" : "-";

                    obj->properties["exists"] = PropertyDescriptor{Value{true}, false, false, true, Token()};
                    obj->properties["isFile"] = PropertyDescriptor{Value{isFile}, false, false, true, Token()};
                    obj->properties["isDir"] = PropertyDescriptor{Value{isDir}, false, false, true, Token()};
                    obj->properties["size"] = PropertyDescriptor{Value{static_cast<double>(size)}, false, false, true, Token()};
                    obj->properties["modifiedAt"] = PropertyDescriptor{Value{modifiedStr}, false, false, true, Token()};
                    obj->properties["permissions"] = PropertyDescriptor{Value{permSummary}, false, false, true, Token()};

                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{obj};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("Stat error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["stat"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // Attach promises sub-object to main fs object
        obj->properties["promises"] = PropertyDescriptor{Value{promises_obj}, false, false, true, Token()};
    }

    return obj;
}

// ----------------- HTTP module (uses libcurl if available; supports GET and POST) -----------------
std::shared_ptr<ObjectValue> make_http_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // METHODS : [.....]
    {
        auto arr = std::make_shared<ArrayValue>();
        arr->elements.push_back(Value(std::string("GET")));
        arr->elements.push_back(Value(std::string("POST")));
        arr->elements.push_back(Value(std::string("DELETE")));
        arr->elements.push_back(Value(std::string("PATCH")));
        arr->elements.push_back(Value(std::string("PUT")));
        arr->elements.push_back(Value(std::string("HEAD")));
        arr->elements.push_back(Value(std::string("OPTIONS")));
        obj->properties["METHODS"] = PropertyDescriptor(arr, false, false, true, Token());
    }

#if defined(HAVE_LIBCURL)
    // http.get(url, [headers_array]) -> string body
    {
        auto fn = make_native_fn("http.get", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value{ std::monostate{} };
            std::string url = value_to_string_simple(args[0]);

            CURL* c = curl_easy_init();
            if (!c) throw SwaziError("HttpError", "curl_easy_init failed", token.loc);
            
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
                throw SwaziError("HttpError", std::string("http.get failed: ") + curl_easy_strerror(res), token.loc);
            }
            return Value{ ctx.buf }; }, env);
        obj->properties["get"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // http.post(url, body, [contentType="application/json"], [headers_array]) -> string body
    {
        auto fn = make_native_fn("http.post", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 1) throw SwaziError("HttpError", "You should pass atleast 1 argument, URL string in http.post to make a post request. \nSyntax:\n --> http.post(url, body, contentType?, headers[]?)", token.loc);
            std::string url = value_to_string_simple(args[0]);
            std::string body("");
            if(args.size() >= 2) body = value_to_string_simple(args[1]);
            std::string contentType = "application/json";
            if (args.size() >= 3) contentType = value_to_string_simple(args[2]);

            CURL* c = curl_easy_init();
            if (!c) throw SwaziError("HttpError", "curl_easy_init failed", token.loc);
            
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
                throw SwaziError("HttpError", std::string("http.post failed: ") + curl_easy_strerror(res), token.loc);
            }
            return Value{ ctx.buf }; }, env);
        obj->properties["post"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
#else
    // Stubs when libcurl is not available
    auto fn_get = make_native_fn("http.get", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value { throw SwaziError("HttpError", "http.get native module requires libcurl support. Build with libcurl or provide an external http module.", token.loc); }, env);
    obj->properties["get"] = PropertyDescriptor{fn_get, false, false, false, Token()};

    auto fn_post = make_native_fn("http.post", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value { throw SwaziError("HttpError", "http.post native module requires libcurl support. Build with libcurl or provide an external http module.", token.loc); }, env);
    obj->properties["post"] = PropertyDescriptor{fn_post, false, false, false, Token()};
#endif

#if defined(HAVE_LIBUV)
    // http.createServer(handler) -> server object (uses libuv)
    {
        Token tok;
        tok.type = TokenType::IDENTIFIER;
        tok.loc = TokenLocation("<http>", 0, 0, 0);

        // native_createServer is implemented in HttpAPI.cpp and declared in builtins.hpp
        auto create_server_fn = std::make_shared<FunctionValue>("http.createServer", native_createServer, env, tok);
        obj->properties["createServer"] = PropertyDescriptor{Value{create_server_fn}, false, false, false, tok};
        obj->properties["server"] = PropertyDescriptor{Value{create_server_fn}, false, false, false, tok};
    }
#else
    // stub: clear error if libuv is not present
    {
        auto fn_server = make_native_fn("http.createServer", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value { throw SwaziError("NotImplementedError", "http.createServer requires libuv support. Build with libuv or provide an external http module.", token.loc); }, env);
        obj->properties["createServer"] = PropertyDescriptor{fn_server, false, false, false, Token()};
        obj->properties["server"] = PropertyDescriptor{fn_server, false, false, false, Token()};
    }
#endif

    // http.fetch(url, options?) -> Promise
    {
        auto fn = make_native_fn("http.fetch", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw SwaziError("TypeError", "fetch requires url", token.loc);
        std::string url = value_to_string_simple(args[0]);
    
        // Parse options object (second parameter)
        std::string method = "GET";
        std::vector<std::string> headers;
        std::vector<uint8_t> body_data;
        bool has_body = false;
        FilePtr streaming_file = nullptr;
        uint64_t streaming_file_size = 0;
        
        // Streaming callback support
        FunctionPtr onData_callback = nullptr;
        FunctionPtr onProgress_callback = nullptr;
        FunctionPtr onEnd_callback = nullptr;
        bool enable_streaming = false;
    
        if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[1]);
    
            // method
            auto it_method = opts->properties.find("method");
            if (it_method != opts->properties.end()) {
                method = value_to_string_simple(it_method->second.value);
                for (auto &c : method) c = static_cast<char>(::toupper((unsigned char)c));
            }
    
            // Parse body
            auto it_body = opts->properties.find("body");
            if (it_body != opts->properties.end()) {
                const Value& body_val = it_body->second.value;
    
                if (std::holds_alternative<std::string>(body_val)) {
                    std::string str = std::get<std::string>(body_val);
                    body_data.assign(str.begin(), str.end());
                    has_body = true;
                }
                else if (std::holds_alternative<BufferPtr>(body_val)) {
                    BufferPtr buf = std::get<BufferPtr>(body_val);
                    body_data = buf->data;
                    has_body = true;
                }
                else if (std::holds_alternative<FilePtr>(body_val)) {
                    streaming_file = std::get<FilePtr>(body_val);
                    if (!streaming_file->is_open) {
                        throw SwaziError("IOError", "File must be open for upload", token.loc);
                    }
                    has_body = true;

#ifdef _WIN32
                    LARGE_INTEGER filesize_li;
                    if (GetFileSizeEx((HANDLE)streaming_file->handle, &filesize_li)) {
                        streaming_file_size = static_cast<uint64_t>(filesize_li.QuadPart);
                    }
#else
                    struct stat st;
                    if (fstat(streaming_file->fd, &st) == 0) {
                        streaming_file_size = static_cast<uint64_t>(st.st_size);
                    }
#endif
                }
                else if (std::holds_alternative<ObjectPtr>(body_val)) {
                    // Check if it's a stream object
                    ObjectPtr stream_obj = std::get<ObjectPtr>(body_val);
                    auto stream_id_prop = stream_obj->properties.find("__stream_id__");
                    
                    if (stream_id_prop != stream_obj->properties.end()) {
                        // It's a stream - read from it
                        auto read_prop = stream_obj->properties.find("read");
                        if (read_prop != stream_obj->properties.end() && 
                            std::holds_alternative<FunctionPtr>(read_prop->second.value)) {
                            
                            FunctionPtr read_fn = std::get<FunctionPtr>(read_prop->second.value);
                            
                            // Read all chunks
                            while (true) {
                                Value chunk = read_fn->native_impl({}, nullptr, token);
                                if (std::holds_alternative<std::monostate>(chunk)) break;
                                
                                if (std::holds_alternative<BufferPtr>(chunk)) {
                                    BufferPtr buf = std::get<BufferPtr>(chunk);
                                    body_data.insert(body_data.end(), buf->data.begin(), buf->data.end());
                                }
                            }
                            has_body = true;
                        }
                    }
                }
            }
    
            // headers
            auto it_headers = opts->properties.find("headers");
            if (it_headers != opts->properties.end()) {
                if (std::holds_alternative<ObjectPtr>(it_headers->second.value)) {
                    ObjectPtr hobj = std::get<ObjectPtr>(it_headers->second.value);
                    for (auto& kv : hobj->properties) {
                        headers.push_back(kv.first + ": " + value_to_string_simple(kv.second.value));
                    }
                } else if (std::holds_alternative<ArrayPtr>(it_headers->second.value)) {
                    ArrayPtr harr = std::get<ArrayPtr>(it_headers->second.value);
                    for (auto& h : harr->elements) {
                        headers.push_back(value_to_string_simple(h));
                    }
                }
            }
            
            // Parse onData callback
            auto it_onData = opts->properties.find("onData");
            if (it_onData != opts->properties.end()) {
                if (std::holds_alternative<FunctionPtr>(it_onData->second.value)) {
                    onData_callback = std::get<FunctionPtr>(it_onData->second.value);
                    enable_streaming = true;
                }
            }
            
            // Parse onProgress callback
            auto it_onProgress = opts->properties.find("onProgress");
            if (it_onProgress != opts->properties.end()) {
                if (std::holds_alternative<FunctionPtr>(it_onProgress->second.value)) {
                    onProgress_callback = std::get<FunctionPtr>(it_onProgress->second.value);
                }
            }
            
            // Parse onEnd callback
            auto it_onEnd = opts->properties.find("onEnd");
            if (it_onEnd != opts->properties.end()) {
                if (std::holds_alternative<FunctionPtr>(it_onEnd->second.value)) {
                    onEnd_callback = std::get<FunctionPtr>(it_onEnd->second.value);
                }
            }
        }
    
        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

#if defined(HAVE_LIBCURL)
        // Schedule async fetch on loop thread
        scheduler_run_on_loop([promise, url, method, body_data, has_body, streaming_file, 
                               streaming_file_size, headers, onData_callback, onProgress_callback, 
                               onEnd_callback, enable_streaming]() {
            CURL* c = curl_easy_init();
            if (!c) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("curl_easy_init failed")};
                for (auto& cb : promise->catch_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
                return;
            }
    
            // Context for streaming
            struct StreamingContext {
                std::string accumulated_buf;
                FunctionPtr onData_cb;
                FunctionPtr onProgress_cb;
                FunctionPtr onEnd_cb;
                bool streaming_mode;
                size_t total_received = 0;
            };
            
            StreamingContext ctx;
            ctx.onData_cb = onData_callback;
            ctx.onProgress_cb = onProgress_callback;
            ctx.onEnd_cb = onEnd_callback;
            ctx.streaming_mode = enable_streaming;
            
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, 
                (+[](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
                    size_t total = size * nmemb;
                    StreamingContext* ctx = static_cast<StreamingContext*>(userdata);
                    
                    if (ctx->streaming_mode && ctx->onData_cb) {
                        auto chunk = std::make_shared<BufferValue>();
                        chunk->data.assign(ptr, ptr + total);
                        chunk->encoding = "binary";
                        
                        FunctionPtr cb = ctx->onData_cb;
                        CallbackPayload* payload = new CallbackPayload(cb, {Value{chunk}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    } else {
                        ctx->accumulated_buf.append(ptr, total);
                    }
                    
                    ctx->total_received += total;
                    
                    if (ctx->onProgress_cb) {
                        auto progress_obj = std::make_shared<ObjectValue>();
                        progress_obj->properties["received"] = PropertyDescriptor{
                            Value{static_cast<double>(ctx->total_received)}, 
                            false, false, true, Token{}
                        };
                        
                        FunctionPtr progress_cb = ctx->onProgress_cb;
                        CallbackPayload* payload = new CallbackPayload(progress_cb, {Value{progress_obj}});
                            enqueue_callback_global(static_cast<void*>(payload));
                    }
                    
                    return total;
                })); 
            
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
    
            // Capture headers (including status line)
            CurlWriteCtx header_ctx;
            curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, curl_header_cb);
            curl_easy_setopt(c, CURLOPT_HEADERDATA, &header_ctx);
    
            // Prepare headers list
            struct curl_slist* curl_headers = nullptr;
            for (const auto& h : headers) {
                curl_headers = curl_slist_append(curl_headers, h.c_str());
            }
    
            // Handle request body
            bool used_read_callback = false;
            if (has_body) {
                if (streaming_file) {
                    // File streaming upload
                    struct ReadContext {
                        FilePtr file;
                        uint64_t total_read = 0;
                    };
    
                    auto* read_ctx = new ReadContext{streaming_file};
    
                    if (method == "PUT") {
                        curl_easy_setopt(c, CURLOPT_UPLOAD, 1L);
                        if (streaming_file_size > 0) {
                            curl_off_t s = static_cast<curl_off_t>(streaming_file_size);
                            curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, s);
                        }
                    } else if (method == "POST") {
                        curl_easy_setopt(c, CURLOPT_POST, 1L);
                        if (streaming_file_size > 0) {
                            curl_off_t s = static_cast<curl_off_t>(streaming_file_size);
                            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, s);
                        }
                    } else {
                        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
                        if (streaming_file_size > 0) {
                            curl_off_t s = static_cast<curl_off_t>(streaming_file_size);
                            curl_easy_setopt(c, CURLOPT_INFILESIZE_LARGE, s);
                        }
                    }
    
                    curl_easy_setopt(c, CURLOPT_READDATA, read_ctx);
                    curl_easy_setopt(c, CURLOPT_READFUNCTION,
                        +[](char* buffer, size_t size, size_t nitems, void* userdata) -> size_t {
                            ReadContext* ctx = static_cast<ReadContext*>(userdata);
                            size_t to_read = size * nitems;
                            if (to_read == 0) return 0;

#ifdef _WIN32
                            DWORD bytes_read = 0;
                            if (!ReadFile((HANDLE)ctx->file->handle, buffer,
                                          static_cast<DWORD>(to_read), &bytes_read, nullptr)) {
                                return CURL_READFUNC_ABORT;
                            }
                            ctx->total_read += bytes_read;
                            return static_cast<size_t>(bytes_read);
#else
                            ssize_t bytes_read = ::read(ctx->file->fd, buffer, to_read);
                            if (bytes_read < 0) return CURL_READFUNC_ABORT;
                            ctx->total_read += static_cast<uint64_t>(bytes_read);
                            return static_cast<size_t>(bytes_read);
#endif
                        });
    
                    used_read_callback = true;
    
                    if (curl_headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, curl_headers);
    
                    CURLcode res = curl_easy_perform(c);
    
                    delete read_ctx;
    
                    if (curl_headers) curl_slist_free_all(curl_headers);
    
                    // Fire onEnd callback AFTER request completes
                    if (ctx.onEnd_cb) {
                        auto end_info = std::make_shared<ObjectValue>();
                        end_info->properties["bytesReceived"] = PropertyDescriptor{
                            Value{static_cast<double>(ctx.total_received)},
                            false, false, true, Token{}
                        };
                        end_info->properties["success"] = PropertyDescriptor{
                            Value{res == CURLE_OK},
                            false, false, true, Token{}
                        };
                        
                        FunctionPtr end_cb = ctx.onEnd_cb;
                        CallbackPayload* payload = new CallbackPayload(end_cb, {Value{end_info}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
    
                    long status_code = 0;
                    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status_code);
    
                    std::string status_text;
                    if (!header_ctx.headers.empty()) {
                        size_t pos_end = header_ctx.headers.find("\r\n");
                        if (pos_end != std::string::npos) {
                            std::string status_line = header_ctx.headers.substr(0, pos_end);
                            size_t sp1 = status_line.find(' ');
                            if (sp1 != std::string::npos) {
                                size_t sp2 = status_line.find(' ', sp1 + 1);
                                if (sp2 != std::string::npos && sp2 + 1 < status_line.size()) {
                                    status_text = status_line.substr(sp2 + 1);
                                    while (!status_text.empty() && (status_text.back() == '\r' || status_text.back() == '\n')) status_text.pop_back();
                                }
                            }
                        }
                    }
    
                    curl_easy_cleanup(c);
    
                    if (res != CURLE_OK) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("fetch failed: ") + curl_easy_strerror(res)};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                        return;
                    }
    
                    auto response = std::make_shared<ObjectValue>();
                    response->properties["status"] = PropertyDescriptor{Value{static_cast<double>(status_code)}, false, false, true, Token()};
                    response->properties["ok"] = PropertyDescriptor{Value{status_code >= 200 && status_code < 300}, false, false, true, Token()};
                    
                    if (ctx.streaming_mode) {
                        response->properties["body"] = PropertyDescriptor{Value{std::string("")}, false, false, true, Token()};
                    } else {
                        response->properties["body"] = PropertyDescriptor{Value{ctx.accumulated_buf}, false, false, true, Token()};
                    }
                    
                    if (!status_text.empty()) {
                        response->properties["statusText"] = PropertyDescriptor{Value{status_text}, false, false, true, Token()};
                    } else {
                        response->properties["statusText"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, Token()};
                    }
                    
                    if (status_code >= 400 && status_code < 600) {
                        if (!ctx.accumulated_buf.empty()) {
                            response->properties["errorMessage"] = PropertyDescriptor{Value{ctx.accumulated_buf}, false, false, true, Token()};
                        } else if (!status_text.empty()) {
                            response->properties["errorMessage"] = PropertyDescriptor{Value{status_text}, false, false, true, Token()};
                        } else {
                            response->properties["errorMessage"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, Token()};
                        }
                    } else {
                        response->properties["errorMessage"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, Token()};
                    }
    
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{response};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
    
                    return;
                } else {
                    // Memory body upload
                    if (method == "POST") {
                        curl_easy_setopt(c, CURLOPT_POST, 1L);
                        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_data.empty() ? nullptr : reinterpret_cast<const char*>(body_data.data()));
                        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_data.size()));
                    } else if (method == "PUT") {
                        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PUT");
                        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_data.empty() ? nullptr : reinterpret_cast<const char*>(body_data.data()));
                        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_data.size()));
                    } else if (method == "PATCH") {
                        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, "PATCH");
                        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_data.empty() ? nullptr : reinterpret_cast<const char*>(body_data.data()));
                        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_data.size()));
                    } else {
                        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
                        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_data.empty() ? nullptr : reinterpret_cast<const char*>(body_data.data()));
                        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, static_cast<long>(body_data.size()));
                    }
                }
            }
    
            // Non-file-upload path
            if (!used_read_callback) {
                if (curl_headers) curl_easy_setopt(c, CURLOPT_HTTPHEADER, curl_headers);
    
                CURLcode res = curl_easy_perform(c);
    
                if (curl_headers) curl_slist_free_all(curl_headers);
    
                // Fire onEnd callback AFTER request completes
                if (ctx.onEnd_cb) {
                    auto end_info = std::make_shared<ObjectValue>();
                    end_info->properties["bytesReceived"] = PropertyDescriptor{
                        Value{static_cast<double>(ctx.total_received)},
                        false, false, true, Token{}
                    };
                    end_info->properties["success"] = PropertyDescriptor{
                        Value{res == CURLE_OK},
                        false, false, true, Token{}
                    };
                    
                    FunctionPtr end_cb = ctx.onEnd_cb;
                    CallbackPayload* payload = new CallbackPayload(end_cb, {Value{end_info}});
                    enqueue_callback_global(static_cast<void*>(payload));
                }
    
                long status_code = 0;
                curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status_code);
    
                std::string status_text;
                if (!header_ctx.headers.empty()) {
                    size_t pos_end = header_ctx.headers.find("\r\n");
                    if (pos_end != std::string::npos) {
                        std::string status_line = header_ctx.headers.substr(0, pos_end);
                        size_t sp1 = status_line.find(' ');
                        if (sp1 != std::string::npos) {
                            size_t sp2 = status_line.find(' ', sp1 + 1);
                            if (sp2 != std::string::npos && sp2 + 1 < status_line.size()) {
                                status_text = status_line.substr(sp2 + 1);
                                while (!status_text.empty() && (status_text.back() == '\r' || status_text.back() == '\n')) status_text.pop_back();
                            }
                        }
                    }
                }
    
                curl_easy_cleanup(c);
    
                if (res != CURLE_OK) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("fetch failed: ") + curl_easy_strerror(res)};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                    return;
                }
    
                auto response = std::make_shared<ObjectValue>();
                response->properties["status"] = PropertyDescriptor{Value{static_cast<double>(status_code)}, false, false, true, Token()};
                response->properties["ok"] = PropertyDescriptor{Value{status_code >= 200 && status_code < 300}, false, false, true, Token()};
                
                if (ctx.streaming_mode) {
                    response->properties["body"] = PropertyDescriptor{Value{std::string("")}, false, false, true, Token()};
                } else {
                    response->properties["body"] = PropertyDescriptor{Value{ctx.accumulated_buf}, false, false, true, Token()};
                }
    
                if (!status_text.empty()) {
                    response->properties["statusText"] = PropertyDescriptor{Value{status_text}, false, false, true, Token()};
                } else {
                    response->properties["statusText"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, Token()};
                }
    
                if (status_code >= 400 && status_code < 600) {
                    if (!ctx.accumulated_buf.empty()) {
                        response->properties["errorMessage"] = PropertyDescriptor{Value{ctx.accumulated_buf}, false, false, true, Token()};
                    } else if (!status_text.empty()) {
                        response->properties["errorMessage"] = PropertyDescriptor{Value{status_text}, false, false, true, Token()};
                    } else {
                        response->properties["errorMessage"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, Token()};
                    }
                } else {
                    response->properties["errorMessage"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, Token()};
                }
    
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{response};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            }
    
        });
#else
        // No libcurl - reject immediately
        promise->state = PromiseValue::State::REJECTED;
        promise->result = Value{std::string("fetch requires libcurl support")};
        for (auto& cb : promise->catch_callbacks) {
            try { cb(promise->result); } catch(...) {}
        }
#endif
    
        return Value{promise}; }, env);
        obj->properties["fetch"] = PropertyDescriptor{fn, false, false, false, Token()};
        obj->properties["request"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
    return obj;
}

// ----------------- JSON module (parse & stringify) -----------------
// Exception for signaling property removal in JSON stringify replacer
class RemovePropertyException : public std::exception {
   public:
    const char* what() const noexcept override {
        return "Property should be removed";
    }
};
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

// Helper to apply replacer with remove() function and depth
static std::pair<bool, Value> apply_replacer_with_context(
    const std::string& key,
    const Value& value,
    int depth,
    FunctionPtr replacer_fn,
    Evaluator* evaluator,
    EnvPtr callEnv,
    const Token& token) {
    if (!replacer_fn || !evaluator) {
        return {true, value};
    }

    // Create the remove() function
    auto remove_fn = make_native_fn("remove", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value { throw RemovePropertyException(); }, callEnv);

    std::vector<Value> replacer_args = {
        Value{key},                        // key
        value,                             // value
        Value{remove_fn},                  // remove function
        Value{static_cast<double>(depth)}  // depth
    };

    try {
        Value result = evaluator->invoke_function(
            replacer_fn, replacer_args, callEnv, token);
        return {true, result};
    } catch (const RemovePropertyException&) {
        return {false, Value{}};  // Signal removal
    } catch (...) {
        return {false, Value{}};  // Error also removes
    }
}

// Helper to apply reviver recursively
static Value apply_reviver_recursive(
    const Value& value,
    const std::string& key,
    FunctionPtr reviver_fn,
    Evaluator* evaluator,
    EnvPtr callEnv,
    const Token& token,
    int depth) {
    // Process children first (bottom-up)
    Value processed_value = value;

    if (std::holds_alternative<ObjectPtr>(value)) {
        ObjectPtr obj = std::get<ObjectPtr>(value);
        auto new_obj = std::make_shared<ObjectValue>();

        for (auto& kv : obj->properties) {
            // Recursively process child first
            Value child_result = apply_reviver_recursive(
                kv.second.value, kv.first, reviver_fn, evaluator, callEnv, token, depth + 1);

            // Only add if not undefined (monostate returned from reviver means delete)
            if (!std::holds_alternative<std::monostate>(child_result)) {
                new_obj->properties[kv.first] = PropertyDescriptor{
                    child_result, false, false, false, Token()};
            }
        }
        processed_value = Value{new_obj};
    } else if (std::holds_alternative<ArrayPtr>(value)) {
        ArrayPtr arr = std::get<ArrayPtr>(value);
        auto new_arr = std::make_shared<ArrayValue>();

        for (size_t i = 0; i < arr->elements.size(); ++i) {
            Value child_result = apply_reviver_recursive(
                arr->elements[i], std::to_string(i), reviver_fn,
                evaluator, callEnv, token, depth + 1);

            // Arrays keep undefined values (unlike objects)
            new_arr->elements.push_back(child_result);
        }
        processed_value = Value{new_arr};
    }

    // Now call reviver on this value (after processing children)
    std::vector<Value> reviver_args = {
        Value{key},
        processed_value};

    try {
        Value result = evaluator->invoke_function(reviver_fn, reviver_args, callEnv, token);
        return result;  // Use the transformed value
    } catch (...) {
        return processed_value;  // On error, return unmodified
    }
}

static std::string json_stringify_value_enhanced(
    const Value& v,
    Evaluator* evaluator,
    std::unordered_set<const ObjectValue*>& objvisited,
    std::unordered_set<const ArrayValue*>& arrvisited,
    const Token& token,
    const std::vector<std::string>& whitelist,
    FunctionPtr replacer_fn,
    EnvPtr callEnv,
    const std::string& indent,
    int depth) {
    // Handle primitives first
    if (std::holds_alternative<std::monostate>(v)) return "null";
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";

    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }

    if (std::holds_alternative<std::string>(v)) {
        return json_stringify_string(std::get<std::string>(v));
    }

    // Handle arrays
    if (std::holds_alternative<ArrayPtr>(v)) {
        ArrayPtr a = std::get<ArrayPtr>(v);
        if (!a) return "[]";

        ArrayValue* p = a.get();
        if (arrvisited.count(p)) {
            throw SwaziError("JsonError", "Converting circular structure to JSON", token.loc);
        }
        arrvisited.insert(p);

        std::ostringstream ss;
        ss << '[';

        bool first = true;
        for (size_t i = 0; i < a->elements.size(); ++i) {
            Value element = a->elements[i];

            // Apply replacer if provided
            if (replacer_fn && evaluator) {
                auto result = apply_replacer_with_context(
                    std::to_string(i), element, depth,
                    replacer_fn, evaluator, callEnv, token);

                if (!result.first) {
                    continue;  // Skip removed element
                }
                element = result.second;
            }

            // Add separator and indentation
            if (!first) {
                ss << ',';
                if (!indent.empty()) {
                    ss << "\n"
                       << std::string((depth + 1) * indent.length(), ' ');
                }
            } else {
                first = false;
                if (!indent.empty()) {
                    ss << "\n"
                       << std::string((depth + 1) * indent.length(), ' ');
                }
            }

            ss << json_stringify_value_enhanced(element, evaluator, objvisited,
                arrvisited, token, whitelist, replacer_fn, callEnv, indent, depth + 1);
        }

        if (!indent.empty() && !a->elements.empty()) {
            ss << '\n'
               << std::string(depth * indent.length(), ' ');
        }
        ss << ']';

        arrvisited.erase(p);
        return ss.str();
    }

    // Handle objects
    if (std::holds_alternative<ObjectPtr>(v)) {
        ObjectPtr o = std::get<ObjectPtr>(v);
        if (!o) return "{}";

        ObjectValue* p = o.get();
        if (objvisited.count(p)) {
            throw SwaziError("JsonError", "Converting circular structure to JSON", token.loc);
        }
        objvisited.insert(p);

        std::ostringstream ss;
        ss << '{';

        bool first = true;
        for (auto& kv : o->properties) {
            // Apply whitelist filter if provided
            if (!whitelist.empty() &&
                std::find(whitelist.begin(), whitelist.end(), kv.first) == whitelist.end()) {
                continue;
            }

            Value prop_value = kv.second.value;

            // Apply replacer if provided
            if (replacer_fn && evaluator) {
                auto result = apply_replacer_with_context(
                    kv.first, prop_value, depth,
                    replacer_fn, evaluator, callEnv, token);

                if (!result.first) {
                    continue;  // Skip removed property
                }
                prop_value = result.second;
            }

            if (!first) {
                ss << ',';
                if (!indent.empty()) {
                    ss << "\n"
                       << std::string((depth + 1) * indent.length(), ' ');
                }
            } else {
                first = false;
                if (!indent.empty()) {
                    ss << "\n"
                       << std::string((depth + 1) * indent.length(), ' ');
                }
            }

            ss << json_stringify_string(kv.first) << ':';
            if (!indent.empty()) ss << ' ';
            ss << json_stringify_value_enhanced(prop_value, evaluator, objvisited,
                arrvisited, token, whitelist, replacer_fn, callEnv, indent, depth + 1);
        }

        if (!indent.empty() && !o->properties.empty()) {
            ss << '\n'
               << std::string(depth * indent.length(), ' ');
        }
        ss << '}';

        objvisited.erase(p);
        return ss.str();
    }

    // Functions/classes -> null
    return "null";
}

}  // namespace
std::shared_ptr<ObjectValue> make_json_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();

    // json.parse(str, reviver?) -> Value
    {
        auto fn = make_native_fn("json.parse", [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            if (args.empty()) return Value{std::monostate{}};
            std::string txt = value_to_string_simple(args[0]);
            
            // Parse reviver function (second parameter)
            FunctionPtr reviver_fn = nullptr;
            if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                reviver_fn = std::get<FunctionPtr>(args[1]);
            }
            
            try {
                JsonParser p(txt);
                Value v = p.parseValue();
                
                // Apply reviver if provided
                if (reviver_fn && evaluator) {
                    v = apply_reviver_recursive(v, "", reviver_fn, evaluator, callEnv, token, 0);
                }
                
                return v;
            } catch (const std::exception& e) {
                throw SwaziError("JsonError", std::string("json.parse failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["parse"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // json.stringify(val, replacer?, spaces?) -> string
    {
        auto fn = make_native_fn("json.stringify", [evaluator](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            if (args.empty()) return Value{std::string("")};
            
            // Parse replacer parameter (can be array or function)
            std::vector<std::string> whitelist;
            FunctionPtr replacer_fn = nullptr;
            
            if (args.size() >= 2 && !std::holds_alternative<std::monostate>(args[1])) {
                if (std::holds_alternative<ArrayPtr>(args[1])) {
                    // Whitelist mode: only include keys in the array
                    ArrayPtr arr = std::get<ArrayPtr>(args[1]);
                    for (const auto& el : arr->elements) {
                        whitelist.push_back(value_to_string_simple(el));
                    }
                } else if (std::holds_alternative<FunctionPtr>(args[1])) {
                    // Function replacer mode
                    replacer_fn = std::get<FunctionPtr>(args[1]);
                }
            }
            
            // Parse spaces parameter
            std::string indent;
            if (args.size() >= 3 && std::holds_alternative<double>(args[2])) {
                int spaces = static_cast<int>(std::get<double>(args[2]));
                spaces = std::max(0, std::min(10, spaces)); // clamp to 0-10
                if (spaces > 0) {
                    indent = std::string(spaces, ' ');
                }
            } else if (args.size() >= 3 && std::holds_alternative<std::string>(args[2])) {
                indent = std::get<std::string>(args[2]);
                if (indent.length() > 10) indent = indent.substr(0, 10);
            }
            
            std::unordered_set<const ObjectValue*> objvisited;
            std::unordered_set<const ArrayValue*> arrvisited;
            
            // Call root with empty key first if replacer exists
            Value root_value = args[0];
            if (replacer_fn && evaluator) {
                try {
                    auto remove_result = apply_replacer_with_context(
                        "", root_value, 0, replacer_fn, evaluator, callEnv, token);
                    if (!remove_result.first) {
                        // Root was removed
                        return Value{std::string("undefined")};
                    }
                    root_value = remove_result.second;
                } catch (...) {
                    return Value{std::string("undefined")};
                }
            }
            
            return Value{json_stringify_value_enhanced(
                root_value, 
                evaluator,
                objvisited, 
                arrvisited, 
                token,
                whitelist,
                replacer_fn,
                callEnv,
                indent,
                0  
            )}; }, env);
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
              throw SwaziError("RuntimeError", "path.join requires at least one path segment to join. Usage: join(...segments) -> string", token.loc);
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
              throw SwaziError("RuntimeError", "path.basename requires a path argument to extract basename from. Usage: basename(path) -> string", token.loc);
            }
            fs::path p = value_to_string_simple(args[0]);
            return Value{ p.filename().string() }; }, env);
        obj->properties["basename"] = PropertyDescriptor{fn, false, false, false, Token()};
    }
    // dirname(path)
    {
        auto fn = make_native_fn("path.dirname", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "path.dirname requires a path to extract dirname from. Usage: dirname(path)", token.loc);
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

Value process_send_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token);
Value process_on_message_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token);

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

    // os.cwd() -> string
    {
        auto fn = make_native_fn("os.cwd", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value { return Value{fs::current_path().string()}; }, env);
        obj->properties["cwd"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    {
        Token t;
        t.type = TokenType::IDENTIFIER;
        t.loc = TokenLocation("<process>", 0, 0, 0);

        // process.send(message)
        auto fn_send = std::make_shared<FunctionValue>(
            "process.send",
            process_send_ipc,  // Implemented in process_ipc.cc
            env,
            t);
        obj->properties["send"] = PropertyDescriptor{fn_send, false, false, false, t};

        // process.on(event, callback)
        auto fn_on = std::make_shared<FunctionValue>(
            "process.on",
            process_on_message_ipc,  // Implemented in process_ipc.cc
            env,
            t);
        obj->properties["on"] = PropertyDescriptor{fn_on, false, false, false, t};
    }

    return obj;
}

// ----------------- BASE64 module -----------------
std::shared_ptr<ObjectValue> make_base64_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // Base64 encoding table
    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    // base64.encode(data) -> string
    {
        auto fn = make_native_fn("base64.encode", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "base64.encode requires data argument. Usage: encode(data) -> string", token.loc);
            }
            
            std::string input = value_to_string_simple(args[0]);
            std::string encoded;
            encoded.reserve(((input.size() + 2) / 3) * 4);
            
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(input.data());
            size_t len = input.size();
            
            for (size_t i = 0; i < len; i += 3) {
                uint32_t octet_a = i < len ? bytes[i] : 0;
                uint32_t octet_b = i + 1 < len ? bytes[i + 1] : 0;
                uint32_t octet_c = i + 2 < len ? bytes[i + 2] : 0;
                
                uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
                
                encoded += base64_chars[(triple >> 18) & 0x3F];
                encoded += base64_chars[(triple >> 12) & 0x3F];
                encoded += (i + 1 < len) ? base64_chars[(triple >> 6) & 0x3F] : '=';
                encoded += (i + 2 < len) ? base64_chars[triple & 0x3F] : '=';
            }
            
            return Value{encoded}; }, env);
        obj->properties["encode"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // base64.decode(encoded) -> string
    {
        auto fn = make_native_fn("base64.decode", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "base64.decode requires encoded string argument. Usage: decode(encoded) -> string", token.loc);
            }
            
            std::string input = value_to_string_simple(args[0]);
            
            // Build reverse lookup table
            static int decode_table[256];
            static bool table_initialized = false;
            if (!table_initialized) {
                for (int i = 0; i < 256; i++) decode_table[i] = -1;
                for (int i = 0; i < 64; i++) {
                    decode_table[static_cast<unsigned char>(base64_chars[i])] = i;
                }
                decode_table['='] = 0;
                table_initialized = true;
            }
            
            std::string decoded;
            decoded.reserve((input.size() / 4) * 3);
            
            uint32_t buffer = 0;
            int bits_collected = 0;
            
            for (char c : input) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
                
                int value = decode_table[static_cast<unsigned char>(c)];
                if (value == -1) {
                    throw SwaziError("RuntimeError", "Invalid base64 character encountered", token.loc);
                }
                
                if (c == '=') break;
                
                buffer = (buffer << 6) | value;
                bits_collected += 6;
                
                if (bits_collected >= 8) {
                    bits_collected -= 8;
                    decoded += static_cast<char>((buffer >> bits_collected) & 0xFF);
                }
            }
            
            return Value{decoded}; }, env);
        obj->properties["decode"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

// ----------------- BUFFER module -----------------
std::shared_ptr<ObjectValue> make_buffer_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // buffer.alloc(size) -> Buffer
    {
        auto fn = make_native_fn("buffer.alloc", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<double>(args[0])) {
                throw SwaziError("TypeError", "buffer.alloc requires a numeric size argument", token.loc);
            }
            
            double size_d = std::get<double>(args[0]);
            if (size_d < 0 || size_d > 1e9) {
                throw SwaziError("RangeError", "Buffer size must be between 0 and 1e9", token.loc);
            }
            
            size_t size = static_cast<size_t>(size_d);
            auto buf = std::make_shared<BufferValue>();
            buf->data.resize(size, 0);  // zero-filled
            buf->encoding = "binary";
            
            return Value{buf}; }, env);
        obj->properties["alloc"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.from(source, encoding?) -> Buffer
    {
        auto fn = make_native_fn("buffer.from", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "buffer.from requires at least one argument", token.loc);
            }
            
            auto buf = std::make_shared<BufferValue>();
            
            // from array of numbers
            if (std::holds_alternative<ArrayPtr>(args[0])) {
                ArrayPtr arr = std::get<ArrayPtr>(args[0]);
                buf->data.reserve(arr->elements.size());
                
                for (const auto& elem : arr->elements) {
                    if (!std::holds_alternative<double>(elem)) {
                        throw SwaziError("TypeError", "Array elements must be numbers (0-255)", token.loc);
                    }
                    double val = std::get<double>(elem);
                    if (val < 0 || val > 255) {
                        throw SwaziError("RangeError", "Byte values must be 0-255", token.loc);
                    }
                    buf->data.push_back(static_cast<uint8_t>(val));
                }
                buf->encoding = "binary";
                return Value{buf};
            }
            
            // from string
            if (std::holds_alternative<std::string>(args[0])) {
                std::string str = std::get<std::string>(args[0]);
                std::string encoding = "utf8";
                
                if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
                    encoding = std::get<std::string>(args[1]);
                }
                
                // Handle hex encoding
                if (encoding == "hex") {
                    // Remove common separators and whitespace
                    std::string cleaned;
                    cleaned.reserve(str.size());
                    for (char c : str) {
                        if (c != ' ' && c != ':' && c != '-' && c != '\n' && c != '\r' && c != '\t') {
                            cleaned += c;
                        }
                    }
                    
                    if (cleaned.length() % 2 != 0) {
                        throw SwaziError("TypeError", "Hex string must have even length", token.loc);
                    }
                    
                    buf->data.reserve(cleaned.length() / 2);
                    
                    auto hexCharToInt = [](char c) -> int {
                        if (c >= '0' && c <= '9') return c - '0';
                        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                        return -1;
                    };
                    
                    for (size_t i = 0; i < cleaned.length(); i += 2) {
                        int high = hexCharToInt(cleaned[i]);
                        int low = hexCharToInt(cleaned[i + 1]);
                        
                        if (high == -1 || low == -1) {
                            throw SwaziError("TypeError", "Invalid hex character in string", token.loc);
                        }
                        
                        buf->data.push_back(static_cast<uint8_t>((high << 4) | low));
                    }
                    
                    buf->encoding = "hex";
                    return Value{buf};
                }
                
                // Handle utf8 and binary
                if (encoding != "utf8" && encoding != "binary") {
                    throw SwaziError("NotImplementedError", 
                        "Only 'utf8', 'binary', and 'hex' encodings are currently supported", token.loc);
                }
                
                buf->data.assign(str.begin(), str.end());
                buf->encoding = encoding;
                return Value{buf};
            }
            
            throw SwaziError("TypeError", "buffer.from requires array or string", token.loc); }, env);
        obj->properties["from"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.isBuffer(val) -> bool
    {
        auto fn = make_native_fn("buffer.isBuffer", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.empty()) return Value{false};
            return Value{std::holds_alternative<BufferPtr>(args[0])}; }, env);
        obj->properties["isBuffer"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.readUInt8(buf, offset) -> number
    {
        auto fn = make_native_fn("buffer.readUInt8", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.readUInt8 requires (buffer, offset)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            double offset_d = std::get<double>(args[1]);
            size_t offset = static_cast<size_t>(offset_d);
            
            if (offset >= buf->data.size()) {
                throw SwaziError("RangeError", "Offset out of bounds", token.loc);
            }
            
            return Value{static_cast<double>(buf->data[offset])}; }, env);
        obj->properties["readUInt8"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.writeUInt8(buf, offset, value) -> buffer
    {
        auto fn = make_native_fn("buffer.writeUInt8", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.writeUInt8 requires (buffer, offset, value)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            double value = std::get<double>(args[2]);
            
            if (offset >= buf->data.size()) {
                throw SwaziError("RangeError", "Offset out of bounds", token.loc);
            }
            if (value < 0 || value > 255) {
                throw SwaziError("RangeError", "Value must be 0-255", token.loc);
            }
            
            buf->data[offset] = static_cast<uint8_t>(value);
            return Value{buf}; }, env);
        obj->properties["writeUInt8"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.readUInt16LE(buf, offset) -> number
    {
        auto fn = make_native_fn("buffer.readUInt16LE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.readUInt16LE requires (buffer, offset)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            
            if (offset + 2 > buf->data.size()) {
                throw SwaziError("RangeError", "Not enough bytes for UInt16", token.loc);
            }
            
            uint16_t value = buf->data[offset] | (buf->data[offset + 1] << 8);
            return Value{static_cast<double>(value)}; }, env);
        obj->properties["readUInt16LE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.writeUInt16LE(buf, offset, value) -> buffer
    {
        auto fn = make_native_fn("buffer.writeUInt16LE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.writeUInt16LE requires (buffer, offset, value)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            uint16_t value = static_cast<uint16_t>(std::get<double>(args[2]));
            
            if (offset + 2 > buf->data.size()) {
                throw SwaziError("RangeError", "Not enough space for UInt16", token.loc);
            }
            
            buf->data[offset] = value & 0xFF;
            buf->data[offset + 1] = (value >> 8) & 0xFF;
            return Value{buf}; }, env);
        obj->properties["writeUInt16LE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.readUInt32BE(buf, offset) -> number
    {
        auto fn = make_native_fn("buffer.readUInt32BE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.readUInt32BE requires (buffer, offset)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            
            if (offset + 4 > buf->data.size()) {
                throw SwaziError("RangeError", "Not enough bytes for UInt32", token.loc);
            }
            
            uint32_t value = (buf->data[offset] << 24) | 
                           (buf->data[offset + 1] << 16) |
                           (buf->data[offset + 2] << 8) | 
                           buf->data[offset + 3];
            return Value{static_cast<double>(value)}; }, env);
        obj->properties["readUInt32BE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.writeUInt32BE(buf, offset, value) -> buffer
    {
        auto fn = make_native_fn("buffer.writeUInt32BE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.writeUInt32BE requires (buffer, offset, value)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            uint32_t value = static_cast<uint32_t>(std::get<double>(args[2]));
            
            if (offset + 4 > buf->data.size()) {
                throw SwaziError("RangeError", "Not enough space for UInt32", token.loc);
            }
            
            buf->data[offset] = (value >> 24) & 0xFF;
            buf->data[offset + 1] = (value >> 16) & 0xFF;
            buf->data[offset + 2] = (value >> 8) & 0xFF;
            buf->data[offset + 3] = value & 0xFF;
            return Value{buf}; }, env);
        obj->properties["writeUInt32BE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.readUInt16BE(buf, offset) -> number
    {
        auto fn = make_native_fn("buffer.readUInt16BE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 2 || !std::holds_alternative<BufferPtr>(args[0])) {
            throw SwaziError("TypeError", "buffer.readUInt16BE requires (buffer, offset)", token.loc);
        }
        
        BufferPtr buf = std::get<BufferPtr>(args[0]);
        size_t offset = static_cast<size_t>(std::get<double>(args[1]));
        
        if (offset + 2 > buf->data.size()) {
            throw SwaziError("RangeError", "Not enough bytes for UInt16", token.loc);
        }
        
        uint16_t value = (buf->data[offset] << 8) | buf->data[offset + 1];
        return Value{static_cast<double>(value)}; }, env);
        obj->properties["readUInt16BE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.writeUInt16BE(buf, offset, value) -> buffer
    {
        auto fn = make_native_fn("buffer.writeUInt16BE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 3 || !std::holds_alternative<BufferPtr>(args[0])) {
            throw SwaziError("TypeError", "buffer.writeUInt16BE requires (buffer, offset, value)", token.loc);
        }
        
        BufferPtr buf = std::get<BufferPtr>(args[0]);
        size_t offset = static_cast<size_t>(std::get<double>(args[1]));
        uint16_t value = static_cast<uint16_t>(std::get<double>(args[2]));
        
        if (offset + 2 > buf->data.size()) {
            throw SwaziError("RangeError", "Not enough space for UInt16", token.loc);
        }
        
        buf->data[offset] = (value >> 8) & 0xFF;
        buf->data[offset + 1] = value & 0xFF;
        return Value{buf}; }, env);
        obj->properties["writeUInt16BE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.readUInt32LE(buf, offset) -> number
    {
        auto fn = make_native_fn("buffer.readUInt32LE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 2 || !std::holds_alternative<BufferPtr>(args[0])) {
            throw SwaziError("TypeError", "buffer.readUInt32LE requires (buffer, offset)", token.loc);
        }
        
        BufferPtr buf = std::get<BufferPtr>(args[0]);
        size_t offset = static_cast<size_t>(std::get<double>(args[1]));
        
        if (offset + 4 > buf->data.size()) {
            throw SwaziError("RangeError", "Not enough bytes for UInt32", token.loc);
        }
        
        uint32_t value = buf->data[offset] | 
                       (buf->data[offset + 1] << 8) |
                       (buf->data[offset + 2] << 16) |
                       (buf->data[offset + 3] << 24);
        return Value{static_cast<double>(value)}; }, env);
        obj->properties["readUInt32LE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.writeUInt32LE(buf, offset, value) -> buffer
    {
        auto fn = make_native_fn("buffer.writeUInt32LE", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 3 || !std::holds_alternative<BufferPtr>(args[0])) {
            throw SwaziError("TypeError", "buffer.writeUInt32LE requires (buffer, offset, value)", token.loc);
        }
        
        BufferPtr buf = std::get<BufferPtr>(args[0]);
        size_t offset = static_cast<size_t>(std::get<double>(args[1]));
        uint32_t value = static_cast<uint32_t>(std::get<double>(args[2]));
        
        if (offset + 4 > buf->data.size()) {
            throw SwaziError("RangeError", "Not enough space for UInt32", token.loc);
        }
        
        buf->data[offset] = value & 0xFF;
        buf->data[offset + 1] = (value >> 8) & 0xFF;
        buf->data[offset + 2] = (value >> 16) & 0xFF;
        buf->data[offset + 3] = (value >> 24) & 0xFF;
        return Value{buf}; }, env);
        obj->properties["writeUInt32LE"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.readFloat32(buf, offset) -> number
    {
        auto fn = make_native_fn("buffer.readFloat32", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.readFloat32 requires (buffer, offset)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            
            if (offset + 4 > buf->data.size()) {
                throw SwaziError("RangeError", "Not enough bytes for Float32", token.loc);
            }
            
            float value;
            std::memcpy(&value, &buf->data[offset], sizeof(float));
            return Value{static_cast<double>(value)}; }, env);
        obj->properties["readFloat32"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.writeFloat32(buf, offset, value) -> buffer
    {
        auto fn = make_native_fn("buffer.writeFloat32", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3 || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.writeFloat32 requires (buffer, offset, value)", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t offset = static_cast<size_t>(std::get<double>(args[1]));
            float value = static_cast<float>(std::get<double>(args[2]));
            
            if (offset + 4 > buf->data.size()) {
                throw SwaziError("RangeError", "Not enough space for Float32", token.loc);
            }
            
            std::memcpy(&buf->data[offset], &value, sizeof(float));
            return Value{buf}; }, env);
        obj->properties["writeFloat32"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.slice(buf, start, end?) -> Buffer
    {
        auto fn = make_native_fn("buffer.slice", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.slice requires a buffer", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            size_t start = 0;
            size_t end = buf->data.size();
            
            if (args.size() >= 2) {
                double start_d = std::get<double>(args[1]);
                start = static_cast<size_t>(std::max(0.0, start_d));
            }
            if (args.size() >= 3) {
                double end_d = std::get<double>(args[2]);
                end = static_cast<size_t>(std::max(0.0, end_d));
            }
            
            start = std::min(start, buf->data.size());
            end = std::min(end, buf->data.size());
            if (start > end) start = end;
            
            auto newBuf = std::make_shared<BufferValue>();
            newBuf->data.assign(buf->data.begin() + start, buf->data.begin() + end);
            newBuf->encoding = buf->encoding;
            
            return Value{newBuf}; }, env);
        obj->properties["slice"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.concat(array_of_buffers) -> Buffer
    {
        auto fn = make_native_fn("buffer.concat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<ArrayPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.concat requires an array of buffers", token.loc);
            }
            
            ArrayPtr arr = std::get<ArrayPtr>(args[0]);
            auto result = std::make_shared<BufferValue>();
            result->encoding = "binary";
            
            for (const auto& elem : arr->elements) {
                if (!std::holds_alternative<BufferPtr>(elem)) {
                    throw SwaziError("TypeError", "All array elements must be buffers", token.loc);
                }
                BufferPtr buf = std::get<BufferPtr>(elem);
                result->data.insert(result->data.end(), buf->data.begin(), buf->data.end());
            }
            
            return Value{result}; }, env);
        obj->properties["concat"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.toString(buf, encoding="utf8") -> string
    {
        auto fn = make_native_fn("buffer.toString", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.toString requires a buffer", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            std::string encoding = "utf8";
            
            if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
                encoding = std::get<std::string>(args[1]);
            }
            
            if (encoding != "utf8" && encoding != "binary") {
                throw SwaziError("NotImplementedError", 
                    "Only 'utf8' and 'binary' encodings supported", token.loc);
            }
            
            return Value{std::string(buf->data.begin(), buf->data.end())}; }, env);
        obj->properties["toString"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.toBase64(buf) -> string
    {
        auto fn = make_native_fn("buffer.toBase64", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.toBase64 requires a buffer", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            
            // Base64 encoding table
            static const char* base64_chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "abcdefghijklmnopqrstuvwxyz"
                "0123456789+/";
            
            std::string encoded;
            encoded.reserve(((buf->data.size() + 2) / 3) * 4);
            
            for (size_t i = 0; i < buf->data.size(); i += 3) {
                uint32_t octet_a = i < buf->data.size() ? buf->data[i] : 0;
                uint32_t octet_b = i + 1 < buf->data.size() ? buf->data[i + 1] : 0;
                uint32_t octet_c = i + 2 < buf->data.size() ? buf->data[i + 2] : 0;
                
                uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
                
                encoded += base64_chars[(triple >> 18) & 0x3F];
                encoded += base64_chars[(triple >> 12) & 0x3F];
                encoded += (i + 1 < buf->data.size()) ? base64_chars[(triple >> 6) & 0x3F] : '=';
                encoded += (i + 2 < buf->data.size()) ? base64_chars[triple & 0x3F] : '=';
            }
            
            return Value{encoded}; }, env);
        obj->properties["toBase64"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.fromBase64(str) -> Buffer
    {
        auto fn = make_native_fn("buffer.fromBase64", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "buffer.fromBase64 requires a string", token.loc);
            }
            
            std::string input = std::get<std::string>(args[0]);
            
            // Build reverse lookup table
            static int decode_table[256];
            static bool table_initialized = false;
            if (!table_initialized) {
                for (int i = 0; i < 256; i++) decode_table[i] = -1;
                const char* base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                for (int i = 0; i < 64; i++) {
                    decode_table[static_cast<unsigned char>(base64_chars[i])] = i;
                }
                decode_table['='] = 0;
                table_initialized = true;
            }
            
            auto buf = std::make_shared<BufferValue>();
            buf->data.reserve((input.size() / 4) * 3);
            buf->encoding = "binary";
            
            uint32_t buffer = 0;
            int bits_collected = 0;
            
            for (char c : input) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
                
                int value = decode_table[static_cast<unsigned char>(c)];
                if (value == -1) {
                    throw SwaziError("RuntimeError", "Invalid base64 character", token.loc);
                }
                
                if (c == '=') break;
                
                buffer = (buffer << 6) | value;
                bits_collected += 6;
                
                if (bits_collected >= 8) {
                    bits_collected -= 8;
                    buf->data.push_back(static_cast<uint8_t>((buffer >> bits_collected) & 0xFF));
                }
            }
            
            return Value{buf}; }, env);
        obj->properties["fromBase64"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.fromHex(str) -> Buffer
    {
        auto fn = make_native_fn("buffer.fromHex", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "buffer.fromHex requires a string", token.loc);
            }
            
            std::string str = std::get<std::string>(args[0]);
            
            // Remove common separators and whitespace
            std::string cleaned;
            cleaned.reserve(str.size());
            for (char c : str) {
                if (c != ' ' && c != ':' && c != '-' && c != '\n' && c != '\r' && c != '\t') {
                    cleaned += c;
                }
            }
            
            if (cleaned.length() % 2 != 0) {
                throw SwaziError("TypeError", "Hex string must have even length", token.loc);
            }
            
            auto buf = std::make_shared<BufferValue>();
            buf->data.reserve(cleaned.length() / 2);
            buf->encoding = "hex";
            
            auto hexCharToInt = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            
            for (size_t i = 0; i < cleaned.length(); i += 2) {
                int high = hexCharToInt(cleaned[i]);
                int low = hexCharToInt(cleaned[i + 1]);
                
                if (high == -1 || low == -1) {
                    throw SwaziError("TypeError", "Invalid hex character in string", token.loc);
                }
                
                buf->data.push_back(static_cast<uint8_t>((high << 4) | low));
            }
            
            return Value{buf}; }, env);
        obj->properties["fromHex"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // buffer.toHex(buf) -> string
    {
        auto fn = make_native_fn("buffer.toHex", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<BufferPtr>(args[0])) {
                throw SwaziError("TypeError", "buffer.toHex requires a buffer", token.loc);
            }
            
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            
            static const char hex_chars[] = "0123456789abcdef";
            std::string result;
            result.reserve(buf->data.size() * 2);
            
            for (uint8_t byte : buf->data) {
                result += hex_chars[byte >> 4];
                result += hex_chars[byte & 0x0F];
            }
            
            return Value{result}; }, env);
        obj->properties["toHex"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}