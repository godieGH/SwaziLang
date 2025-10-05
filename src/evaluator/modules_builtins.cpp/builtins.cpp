#include "builtins.hpp"
#include "evaluator.hpp"
#include <regex>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>

#ifdef __has_include
# if __has_include(<curl/curl.h>)
#  include <curl/curl.h>
#  define HAVE_LIBCURL 1
# endif
#endif

namespace fs = std::filesystem;

// small helper to coerce Value -> string (same idea as earlier)
static std::string value_to_string_simple(const Value &v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss; ss << std::get<double>(v); return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Helper: create a native FunctionValue from a lambda
template<typename F>
static FunctionPtr make_native_fn(const std::string &name, F impl, EnvPtr env) {
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
struct CurlWriteCtx { std::string buf; };

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t total = size * nmemb;
    CurlWriteCtx* ctx = static_cast<CurlWriteCtx*>(userdata);
    if (ctx) ctx->buf.append(ptr, total);
    return total;
}
#endif

// ----------------- REGEX module -----------------
std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // match(str, pattern) -> bool (search anywhere)
    {
        auto fn = make_native_fn("regex.match", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) return Value{ false };
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            try {
                std::regex re(pat);
                std::smatch m;
                bool res = std::regex_search(s, m, re);
                return Value{ res };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            }
        }, env);
        obj->properties["match"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // fullMatch(str, pattern) -> bool
    {
        auto fn = make_native_fn("regex.fullMatch", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) return Value{ false };
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            try {
                std::regex re(pat);
                bool res = std::regex_match(s, re);
                return Value{ res };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            }
        }, env);
        obj->properties["fullMatch"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // search(str, pattern) -> number (first match position) or -1
    {
        auto fn = make_native_fn("regex.search", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) return Value{ static_cast<double>(-1) };
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            try {
                std::regex re(pat);
                std::smatch m;
                if (std::regex_search(s, m, re)) {
                    return Value{ static_cast<double>(m.position()) };
                }
                return Value{ static_cast<double>(-1) };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            }
        }, env);
        obj->properties["search"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // replace(str, pattern, replacement) -> string (replace all)
    {
        auto fn = make_native_fn("regex.replace", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3) return Value{ std::string() };
            std::string s = value_to_string_simple(args[0]);
            std::string pat = value_to_string_simple(args[1]);
            std::string repl = value_to_string_simple(args[2]);
            try {
                std::regex re(pat);
                std::string out = std::regex_replace(s, re, repl);
                return Value{ out };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            }
        }, env);
        obj->properties["replace"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // split(str, pattern) -> Array
    {
        auto fn = make_native_fn("regex.split", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value{ std::make_shared<ArrayValue>() };
            std::string s = value_to_string_simple(args[0]);
            std::string pat = args.size() >= 2 ? value_to_string_simple(args[1]) : std::string();
            auto arr = std::make_shared<ArrayValue>();
            if (pat.empty()) {
                for (char c : s) arr->elements.push_back(Value{ std::string(1, c) });
                return Value{ arr };
            }
            try {
                std::regex re(pat);
                std::sregex_token_iterator it(s.begin(), s.end(), re, -1), end;
                for (; it != end; ++it) arr->elements.push_back(Value{ it->str() });
                return Value{ arr };
            } catch (const std::regex_error &e) {
                throw std::runtime_error(std::string("regex error at ") + token.loc.to_string() + ": " + e.what());
            }
        }, env);
        obj->properties["split"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    return obj;
}

// ----------------- FS module -----------------
std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // readFile(path) -> string
    {
        auto fn = make_native_fn("fs.readFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value{ std::monostate{} };
            std::string path = value_to_string_simple(args[0]);
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) return Value{ std::monostate{} };
            std::ostringstream ss; ss << in.rdbuf();
            return Value{ ss.str() };
        }, env);
        obj->properties["readFile"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // writeFile(path, content) -> bool
    {
        auto fn = make_native_fn("fs.writeFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) return Value{ false };
            std::string path = value_to_string_simple(args[0]);
            std::string content = value_to_string_simple(args[1]);
            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) return Value{ false };
            out << content;
            return Value{ true };
        }, env);
        obj->properties["writeFile"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // exists(path) -> bool
    {
        auto fn = make_native_fn("fs.exists", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
            if (args.empty()) return Value{ false };
            std::string path = value_to_string_simple(args[0]);
            return Value{ std::filesystem::exists(path) };
        }, env);
        obj->properties["exists"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    // listDir(path) -> array of entries (strings)
    {
        auto fn = make_native_fn("fs.listDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
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
            return Value{ arr };
        }, env);
        obj->properties["listDir"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }

    return obj;
}

// ----------------- HTTP module (lightweight) -----------------
std::shared_ptr<ObjectValue> make_http_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

#if defined(HAVE_LIBCURL)
    // http.get(url) -> string body (blocking)
    {
        auto fn = make_native_fn("http.get", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) return Value{ std::monostate{} };
            std::string url = value_to_string_simple(args[0]);
            CURL* c = curl_easy_init();
            if (!c) throw std::runtime_error("curl:init failed");
            CurlWriteCtx ctx;
            curl_easy_setopt(c, CURLOPT_URL, url.c_str());
            curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &curl_write_cb);
            curl_easy_setopt(c, CURLOPT_WRITEDATA, &ctx);
            CURLcode res = curl_easy_perform(c);
            curl_easy_cleanup(c);
            if (res != CURLE_OK) {
                throw std::runtime_error(std::string("http.get failed: ") + curl_easy_strerror(res) + " at " + token.loc.to_string());
            }
            return Value{ ctx.buf };
        }, env);
        obj->properties["get"] = PropertyDescriptor{ fn, false, false, false, Token() };
    }
#else
    // If libcurl is not available, provide a stub that throws an informative error.
    auto fn_stub = make_native_fn("http.get", [](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value {
        throw std::runtime_error("http.get native module requires libcurl support. Build with libcurl or provide an external http module. (called at " + token.loc.to_string() + ")");
    }, env);
    obj->properties["get"] = PropertyDescriptor{ fn_stub, false, false, false, Token() };
#endif

    return obj;
}