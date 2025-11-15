#include "url_class.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <unordered_map>

#include "ClassRuntime.hpp"
#include "SwaziError.hpp"
#include "token.hpp"
#include "globals.hpp"

// ============================================
// ========== URL PARSING UTILITIES ===========
// ============================================

static std::string percent_encode(const std::string& str, bool full_uri = false) {
    std::ostringstream encoded;
    encoded << std::hex << std::uppercase << std::setfill('0');
    
    for (unsigned char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        }
        else if (full_uri && (c == ':' || c == '/' || c == '?' || c == '#' || 
                              c == '[' || c == ']' || c == '@' || c == '!' ||
                              c == '$' || c == '&' || c == '\'' || c == '(' ||
                              c == ')' || c == '*' || c == '+' || c == ',' ||
                              c == ';' || c == '=')) {
            encoded << c;
        }
        else {
            encoded << '%' << std::setw(2) << static_cast<int>(c);
        }
    }
    
    return encoded.str();
}

static std::string percent_decode(const std::string& str) {
    std::ostringstream decoded;
    
    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '%' && i + 2 < str.length()) {
            std::string hex = str.substr(i + 1, 2);
            try {
                int value = std::stoi(hex, nullptr, 16);
                decoded << static_cast<char>(value);
                i += 2;
            } catch (...) {
                decoded << str[i];
            }
        } else if (str[i] == '+') {
            decoded << ' ';
        } else {
            decoded << str[i];
        }
    }
    
    return decoded.str();
}

static std::string normalize_path(const std::string& path) {
    if (path.empty()) return "/";
    
    std::vector<std::string> segments;
    std::string current;
    
    for (char c : path) {
        if (c == '/') {
            if (!current.empty()) {
                if (current == "..") {
                    if (!segments.empty() && segments.back() != "..") {
                        segments.pop_back();
                    }
                } else if (current != ".") {
                    segments.push_back(current);
                }
                current.clear();
            }
        } else {
            current += c;
        }
    }
    
    if (!current.empty()) {
        if (current == "..") {
            if (!segments.empty() && segments.back() != "..") {
                segments.pop_back();
            }
        } else if (current != ".") {
            segments.push_back(current);
        }
    }
    
    std::string result = "/";
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) result += "/";
        result += segments[i];
    }
    
    if (path.back() == '/' && result != "/") result += "/";
    
    return result;
}

struct URLComponents {
    std::string protocol;
    std::string username;
    std::string password;
    std::string hostname;
    std::string port;
    std::string pathname;
    std::string search;
    std::string hash;
    
    std::string origin() const {
        std::string result = protocol;
        if (!result.empty() && result.back() != ':') result += ":";
        result += "//";
        result += hostname;
        if (!port.empty()) {
            result += ":" + port;
        }
        return result;
    }
    
    std::string href() const {
        std::string result = protocol;
        if (!result.empty() && result.back() != ':') result += ":";
        result += "//";
        
        if (!username.empty()) {
            result += username;
            if (!password.empty()) {
                result += ":" + password;
            }
            result += "@";
        }
        
        result += hostname;
        
        if (!port.empty()) {
            result += ":" + port;
        }
        
        result += pathname.empty() ? "/" : pathname;
        result += search;
        result += hash;
        
        return result;
    }
};

static URLComponents parse_url(const std::string& url_str) {
    URLComponents comp;
    
    std::string remaining = url_str;
    
    size_t proto_end = remaining.find("://");
    if (proto_end != std::string::npos) {
        comp.protocol = remaining.substr(0, proto_end) + ":";
        remaining = remaining.substr(proto_end + 3);
    }
    
    size_t hash_pos = remaining.find('#');
    if (hash_pos != std::string::npos) {
        comp.hash = remaining.substr(hash_pos);
        remaining = remaining.substr(0, hash_pos);
    }
    
    size_t search_pos = remaining.find('?');
    if (search_pos != std::string::npos) {
        comp.search = remaining.substr(search_pos);
        remaining = remaining.substr(0, search_pos);
    }
    
    size_t path_pos = remaining.find('/');
    std::string authority;
    if (path_pos != std::string::npos) {
        authority = remaining.substr(0, path_pos);
        comp.pathname = remaining.substr(path_pos);
    } else {
        authority = remaining;
        comp.pathname = "/";
    }
    
    size_t at_pos = authority.find('@');
    std::string host_port;
    if (at_pos != std::string::npos) {
        std::string userinfo = authority.substr(0, at_pos);
        host_port = authority.substr(at_pos + 1);
        
        size_t colon_pos = userinfo.find(':');
        if (colon_pos != std::string::npos) {
            comp.username = userinfo.substr(0, colon_pos);
            comp.password = userinfo.substr(colon_pos + 1);
        } else {
            comp.username = userinfo;
        }
    } else {
        host_port = authority;
    }
    
    size_t port_pos = host_port.rfind(':');
    if (port_pos != std::string::npos) {
        comp.hostname = host_port.substr(0, port_pos);
        comp.port = host_port.substr(port_pos + 1);
    } else {
        comp.hostname = host_port;
    }
    
    return comp;
}

// Helper to safely get ObjectPtr from Value
static ObjectPtr safe_get_object(const Value& v, const std::string& context, const Token& tok) {
    if (!std::holds_alternative<ObjectPtr>(v)) {
        throw SwaziError("TypeError", context + " requires an object", tok.loc);
    }
    ObjectPtr obj = std::get<ObjectPtr>(v);
    if (!obj) {
        throw SwaziError("TypeError", context + " received null object", tok.loc);
    }
    return obj;
}

// Helper to safely get string property from object
static std::string safe_get_string_property(ObjectPtr obj, const std::string& prop, const std::string& context, const Token& tok) {
    auto it = obj->properties.find(prop);
    if (it == obj->properties.end()) {
        throw SwaziError("TypeError", context + " missing property '" + prop + "'", tok.loc);
    }
    return value_to_string(it->second.value);
}

// Helper to update URL href after changes
static void update_url_href(ObjectPtr url, const Token& tok) {
    try {
        std::string protocol = safe_get_string_property(url, "protocol", "URL.updateHref", tok);
        std::string hostname = safe_get_string_property(url, "hostname", "URL.updateHref", tok);
        std::string port = safe_get_string_property(url, "port", "URL.updateHref", tok);
        std::string pathname = safe_get_string_property(url, "pathname", "URL.updateHref", tok);
        std::string search = safe_get_string_property(url, "search", "URL.updateHref", tok);
        std::string hash = safe_get_string_property(url, "hash", "URL.updateHref", tok);
        
        URLComponents comp;
        comp.protocol = protocol;
        comp.hostname = hostname;
        comp.port = port;
        comp.pathname = pathname;
        comp.search = search;
        comp.hash = hash;
        
        url->properties["href"].value = Value{comp.href()};
    } catch (const std::exception& e) {
        // Silently fail to avoid cascading errors during updates
    }
}

// ============================================
// ========== NATIVE STATIC METHODS ===========
// ============================================

static Value native_URL_encode(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    std::string str = value_to_string(args[0]);
    return Value{percent_encode(str, false)};
}

static Value native_URL_decode(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    std::string str = value_to_string(args[0]);
    return Value{percent_decode(str)};
}

static Value native_URL_encodeURIComponent(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    std::string str = value_to_string(args[0]);
    return Value{percent_encode(str, false)};
}

static Value native_URL_decodeURIComponent(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    std::string str = value_to_string(args[0]);
    return Value{percent_decode(str)};
}

static Value native_URL_encodeURI(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    std::string str = value_to_string(args[0]);
    return Value{percent_encode(str, true)};
}

static Value native_URL_decodeURI(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    std::string str = value_to_string(args[0]);
    return Value{percent_decode(str)};
}

// ============================================
// ========== URLSearchParams METHODS =========
// ============================================

static Value native_searchParams_get(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.searchParams.get() requires a key argument", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.get()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.get() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.get()", tok);
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.get()", tok);
    std::string key = value_to_string(args[1]);
    
    if (search.empty() || search == "?") return std::monostate{};
    std::string q = search[0] == '?' ? search.substr(1) : search;
    
    size_t pos = 0;
    while (pos < q.length()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.length();
        std::string pair = q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = percent_decode(pair.substr(0, eq));
            if (k == key) {
                return Value{percent_decode(pair.substr(eq + 1))};
            }
        }
        pos = amp + 1;
    }
    return std::monostate{};
}

static Value native_searchParams_getAll(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.searchParams.getAll() requires a key argument", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.getAll()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.getAll() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.getAll()", tok);
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.getAll()", tok);
    std::string key = value_to_string(args[1]);
    
    auto arr = std::make_shared<ArrayValue>();
    if (search.empty() || search == "?") return Value{arr};
    std::string q = search[0] == '?' ? search.substr(1) : search;
    
    size_t pos = 0;
    while (pos < q.length()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.length();
        std::string pair = q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = percent_decode(pair.substr(0, eq));
            if (k == key) {
                arr->elements.push_back(Value{percent_decode(pair.substr(eq + 1))});
            }
        }
        pos = amp + 1;
    }
    return Value{arr};
}

static Value native_searchParams_set(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "URL.searchParams.set() requires key and value arguments", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.set()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.set() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.set()", tok);
    std::string key = value_to_string(args[1]);
    std::string value = value_to_string(args[2]);
    
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.set()", tok);
    std::string q = search.empty() || search == "?" ? "" : (search[0] == '?' ? search.substr(1) : search);
    
    std::ostringstream result;
    bool found = false;
    size_t pos = 0;
    bool first = true;
    
    while (pos < q.length()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.length();
        std::string pair = q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        
        if (eq != std::string::npos) {
            std::string k = percent_decode(pair.substr(0, eq));
            if (k == key) {
                if (!found) {
                    if (!first) result << "&";
                    result << percent_encode(key, false) << "=" << percent_encode(value, false);
                    found = true;
                    first = false;
                }
                // Skip duplicate keys
            } else {
                if (!first) result << "&";
                result << pair;
                first = false;
            }
        }
        pos = amp + 1;
    }
    
    if (!found) {
        if (!first) result << "&";
        result << percent_encode(key, false) << "=" << percent_encode(value, false);
    }
    
    std::string new_search = result.str();
    url->properties["search"].value = Value{new_search.empty() ? "" : "?" + new_search};
    update_url_href(url, tok);
    
    return std::monostate{};
}

static Value native_searchParams_append(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "URL.searchParams.append() requires key and value arguments", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.append()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.append() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.append()", tok);
    std::string key = value_to_string(args[1]);
    std::string value = value_to_string(args[2]);
    
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.append()", tok);
    std::string q = search.empty() || search == "?" ? "" : (search[0] == '?' ? search.substr(1) : search);
    
    std::ostringstream result;
    result << q;
    if (!q.empty()) result << "&";
    result << percent_encode(key, false) << "=" << percent_encode(value, false);
    
    std::string new_search = result.str();
    url->properties["search"].value = Value{"?" + new_search};
    update_url_href(url, tok);
    
    return std::monostate{};
}

static Value native_searchParams_delete(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.searchParams.delete() requires a key argument", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.delete()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.delete() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.delete()", tok);
    std::string key = value_to_string(args[1]);
    
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.delete()", tok);
    std::string q = search.empty() || search == "?" ? "" : (search[0] == '?' ? search.substr(1) : search);
    
    std::ostringstream result;
    size_t pos = 0;
    bool first = true;
    
    while (pos < q.length()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.length();
        std::string pair = q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        
        if (eq != std::string::npos) {
            std::string k = percent_decode(pair.substr(0, eq));
            if (k != key) {
                if (!first) result << "&";
                result << pair;
                first = false;
            }
        }
        pos = amp + 1;
    }
    
    std::string new_search = result.str();
    url->properties["search"].value = Value{new_search.empty() ? "" : "?" + new_search};
    update_url_href(url, tok);
    
    return std::monostate{};
}

static Value native_searchParams_has(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.searchParams.has() requires a key argument", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.has()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.has() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.has()", tok);
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.has()", tok);
    std::string key = value_to_string(args[1]);
    
    if (search.empty() || search == "?") return Value{false};
    std::string q = search[0] == '?' ? search.substr(1) : search;
    
    size_t pos = 0;
    while (pos < q.length()) {
        size_t amp = q.find('&', pos);
        if (amp == std::string::npos) amp = q.length();
        std::string pair = q.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            std::string k = percent_decode(pair.substr(0, eq));
            if (k == key) return Value{true};
        }
        pos = amp + 1;
    }
    return Value{false};
}

static Value native_searchParams_toString(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) {
        throw SwaziError("TypeError", "URL.searchParams.toString() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(args[0], "URL.searchParams.toString()", tok);
    
    auto it = sp->properties.find("__parent_url__");
    if (it == sp->properties.end()) {
        throw SwaziError("TypeError", "URL.searchParams.toString() invalid searchParams object", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(it->second.value, "URL.searchParams.toString()", tok);
    std::string search = safe_get_string_property(url, "search", "URL.searchParams.toString()", tok);
    
    if (!search.empty() && search[0] == '?') {
        return Value{search.substr(1)};
    }
    return Value{search};
}

// ============================================
// ========== URL CONSTRUCTOR =================
// ============================================

static Value native_URL_ctor(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL constructor requires a URL string argument", tok.loc);
    }
    
    ObjectPtr instance = safe_get_object(args[0], "URL constructor", tok);
    std::string url_str = value_to_string(args[1]);
    
    if (url_str.empty()) {
        throw SwaziError("TypeError", "URL constructor requires a non-empty URL string", tok.loc);
    }
    
    // Parse URL
    URLComponents comp;
    try {
        comp = parse_url(url_str);
    } catch (const std::exception& e) {
        throw SwaziError("TypeError", "URL constructor failed to parse URL: " + std::string(e.what()), tok.loc);
    }
    
    // Set properties
    instance->properties["protocol"].value = Value{comp.protocol};
    instance->properties["username"].value = Value{comp.username};
    instance->properties["password"].value = Value{comp.password};
    instance->properties["hostname"].value = Value{comp.hostname};
    instance->properties["port"].value = Value{comp.port};
    instance->properties["pathname"].value = Value{comp.pathname};
    instance->properties["search"].value = Value{comp.search};
    instance->properties["hash"].value = Value{comp.hash};
    instance->properties["origin"].value = Value{comp.origin()};
    instance->properties["href"].value = Value{comp.href()};
    
    // Create searchParams object
    auto sp = std::make_shared<ObjectValue>();
    sp->properties["__parent_url__"] = {Value{instance}, true, false, false, tok};
    
    // Add searchParams methods with proper 'this' binding
    // We create wrapper functions that capture 'sp' and prepend it to args
    Token spTok;
    
    auto make_sp_method = [sp, env, &spTok](
        const std::string& name,
        std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl
    ) -> FunctionPtr {
        auto wrapper = [sp, impl](const std::vector<Value>& args, EnvPtr env, const Token& tok) -> Value {
            // Prepend 'this' (sp) to the arguments
            std::vector<Value> new_args;
            new_args.push_back(Value{sp});
            for (const auto& arg : args) {
                new_args.push_back(arg);
            }
            return impl(new_args, env, tok);
        };
        return std::make_shared<FunctionValue>(name, wrapper, env, spTok);
    };
    
    sp->properties["get"] = {make_sp_method("get", native_searchParams_get), false, false, false, spTok};
    sp->properties["getAll"] = {make_sp_method("getAll", native_searchParams_getAll), false, false, false, spTok};
    sp->properties["set"] = {make_sp_method("set", native_searchParams_set), false, false, false, spTok};
    sp->properties["append"] = {make_sp_method("append", native_searchParams_append), false, false, false, spTok};
    sp->properties["delete"] = {make_sp_method("delete", native_searchParams_delete), false, false, false, spTok};
    sp->properties["has"] = {make_sp_method("has", native_searchParams_has), false, false, false, spTok};
    sp->properties["toString"] = {make_sp_method("toString", native_searchParams_toString), false, false, false, spTok};
    
    instance->properties["searchParams"].value = Value{sp};
    
    return std::monostate{};
}

// ============================================
// ========== URL INSTANCE METHODS ============
// ============================================

static Value native_url_toString(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) {
        throw SwaziError("TypeError", "URL.toString() missing 'this' context", tok.loc);
    }
    
    ObjectPtr obj = safe_get_object(args[0], "URL.toString()", tok);
    
    auto it = obj->properties.find("href");
    if (it == obj->properties.end()) {
        throw SwaziError("TypeError", "URL.toString() invalid URL object (missing href)", tok.loc);
    }
    
    return it->second.value;
}

static Value native_url_normalize(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) {
        throw SwaziError("TypeError", "URL.normalize() missing 'this' context", tok.loc);
    }
    
    ObjectPtr obj = safe_get_object(args[0], "URL.normalize()", tok);
    std::string path = safe_get_string_property(obj, "pathname", "URL.normalize()", tok);
    
    obj->properties["pathname"].value = Value{normalize_path(path)};
    update_url_href(obj, tok);
    
    return Value{obj};
}

static Value native_url_clone(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.empty()) {
        throw SwaziError("TypeError", "URL.clone() missing 'this' context", tok.loc);
    }
    
    ObjectPtr original = safe_get_object(args[0], "URL.clone()", tok);
    std::string href = safe_get_string_property(original, "href", "URL.clone()", tok);
    
    // Create new URL from href
    auto newInstance = std::make_shared<ObjectValue>();
    return native_URL_ctor({newInstance, Value{href}}, env, tok);
}

// Shortcut methods
static Value native_url_setQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) {
        throw SwaziError("TypeError", "URL.setQuery() requires key and value arguments", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(args[0], "URL.setQuery()", tok);
    
    auto it = url->properties.find("searchParams");
    if (it == url->properties.end()) {
        throw SwaziError("TypeError", "URL.setQuery() invalid URL object (missing searchParams)", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(it->second.value, "URL.setQuery()", tok);
    return native_searchParams_set({sp, args[1], args[2]}, nullptr, tok);
}

static Value native_url_getQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.getQuery() requires a key argument", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(args[0], "URL.getQuery()", tok);
    
    auto it = url->properties.find("searchParams");
    if (it == url->properties.end()) {
        throw SwaziError("TypeError", "URL.getQuery() invalid URL object (missing searchParams)", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(it->second.value, "URL.getQuery()", tok);
    return native_searchParams_get({sp, args[1]}, nullptr, tok);
}

static Value native_url_deleteQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.deleteQuery() requires a key argument", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(args[0], "URL.deleteQuery()", tok);
    
    auto it = url->properties.find("searchParams");
    if (it == url->properties.end()) {
        throw SwaziError("TypeError", "URL.deleteQuery() invalid URL object (missing searchParams)", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(it->second.value, "URL.deleteQuery()", tok);
    return native_searchParams_delete({sp, args[1]}, nullptr, tok);
}

static Value native_url_hasQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.hasQuery() requires a key argument", tok.loc);
    }
    
    ObjectPtr url = safe_get_object(args[0], "URL.hasQuery()", tok);
    
    auto it = url->properties.find("searchParams");
    if (it == url->properties.end()) {
        throw SwaziError("TypeError", "URL.hasQuery() invalid URL object (missing searchParams)", tok.loc);
    }
    
    ObjectPtr sp = safe_get_object(it->second.value, "URL.hasQuery()", tok);
    return native_searchParams_has({sp, args[1]}, nullptr, tok);
}


// ============================================
// ========== PATH PARAMETER METHODS ==========
// ============================================

// Helper to split path into segments
static std::vector<std::string> split_path_segments(const std::string& path) {
    std::vector<std::string> segments;
    std::string segment;
    
    for (char c : path) {
        if (c == '/') {
            if (!segment.empty()) {
                segments.push_back(segment);
                segment.clear();
            }
        } else {
            segment += c;
        }
    }
    if (!segment.empty()) {
        segments.push_back(segment);
    }
    
    return segments;
}

static Value native_url_getPathSegments(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) {
        throw SwaziError("TypeError", "URL.getPathSegments() missing 'this' context", tok.loc);
    }
    
    ObjectPtr obj = safe_get_object(args[0], "URL.getPathSegments()", tok);
    std::string pathname = safe_get_string_property(obj, "pathname", "URL.getPathSegments()", tok);
    
    auto arr = std::make_shared<ArrayValue>();
    std::vector<std::string> segments = split_path_segments(pathname);
    
    for (const auto& seg : segments) {
        arr->elements.push_back(Value{seg});
    }
    
    return Value{arr};
}

static Value native_url_getPathSegment(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.getPathSegment() requires an index argument", tok.loc);
    }
    
    ObjectPtr obj = safe_get_object(args[0], "URL.getPathSegment()", tok);
    std::string pathname = safe_get_string_property(obj, "pathname", "URL.getPathSegment()", tok);
    
    int index = static_cast<int>(value_to_number(args[1]));
    std::vector<std::string> segments = split_path_segments(pathname);
    
    // Handle negative indices (from end)
    if (index < 0) {
        index = static_cast<int>(segments.size()) + index;
    }
    
    if (index >= 0 && index < static_cast<int>(segments.size())) {
        return Value{segments[index]};
    }
    
    return std::monostate{};  // Return null if out of bounds
}

static Value native_url_matchPath(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "URL.matchPath() requires a pattern argument", tok.loc);
    }
    
    ObjectPtr obj = safe_get_object(args[0], "URL.matchPath()", tok);
    std::string pathname = safe_get_string_property(obj, "pathname", "URL.matchPath()", tok);
    std::string pattern = value_to_string(args[1]);
    
    std::vector<std::string> pathSegs = split_path_segments(pathname);
    std::vector<std::string> patternSegs = split_path_segments(pattern);
    
    // Must have same number of segments to match
    if (pathSegs.size() != patternSegs.size()) {
        return std::monostate{};  // No match
    }
    
    // Match and extract params
    auto result = std::make_shared<ObjectValue>();
    
    for (size_t i = 0; i < patternSegs.size(); ++i) {
        const std::string& patSeg = patternSegs[i];
        const std::string& pathSeg = pathSegs[i];
        
        if (!patSeg.empty() && patSeg[0] == ':') {
            // This is a param placeholder - extract the name and value
            std::string paramName = patSeg.substr(1);  // Remove ':'
            if (paramName.empty()) {
                throw SwaziError("TypeError", "URL.matchPath() invalid pattern: empty parameter name", tok.loc);
            }
            result->properties[paramName] = {Value{pathSeg}, false, false, false, tok};
        } else if (!patSeg.empty() && patSeg[0] == '*') {
            // Wildcard segment - matches anything, optionally named
            if (patSeg.length() > 1) {
                std::string paramName = patSeg.substr(1);  // Get name after '*'
                result->properties[paramName] = {Value{pathSeg}, false, false, false, tok};
            }
            // If just '*', it matches but doesn't capture
        } else {
            // Literal segment - must match exactly (case-sensitive)
            if (patSeg != pathSeg) {
                return std::monostate{};  // No match
            }
        }
    }
    
    return Value{result};
}




// ============================================
// ========== INITIALIZATION ==================
// ============================================

void init_url_class(EnvPtr env) {
    if (!env) return;
    
    auto add_native = [&](const std::string& name, 
                          std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> fn) {
        auto fv = std::make_shared<FunctionValue>(name, fn, env, Token{});
        Environment::Variable v{fv, true};
        env->set(name, v);
    };
    
    add_native("URL_native_ctor", native_URL_ctor);
    add_native("URL_native_toString", native_url_toString);
    add_native("URL_native_normalize", native_url_normalize);
    add_native("URL_native_clone", native_url_clone);
    add_native("URL_native_setQuery", native_url_setQuery);
    add_native("URL_native_getQuery", native_url_getQuery);
    add_native("URL_native_deleteQuery", native_url_deleteQuery);
    add_native("URL_native_hasQuery", native_url_hasQuery);
    add_native("URL_native_getPathSegments", native_url_getPathSegments);
    add_native("URL_native_getPathSegment", native_url_getPathSegment);
    add_native("URL_native_matchPath", native_url_matchPath);
    
    auto classDesc = std::make_shared<ClassValue>();
    classDesc->name = "URL";
    classDesc->token = Token{};
    classDesc->body = std::make_unique<ClassBodyNode>();
    
    // Properties
    std::vector<std::string> props = {
        "href", "protocol", "username", "password", "hostname", 
        "port", "origin", "pathname", "search", "hash", "searchParams"
    };
    
    for (const auto& prop : props) {
        auto p = std::make_unique<ClassPropertyNode>();
        p->name = prop;
        p->is_private = false;
        p->is_locked = false;
        classDesc->body->properties.push_back(std::move(p));
    }
    
    // Constructor
    {
        auto ctor = std::make_unique<ClassMethodNode>();
        ctor->name = "URL";
        ctor->is_constructor = true;
        
        auto p = std::make_unique<ParameterNode>();
        p->name = "url_str";
        ctor->params.push_back(std::move(p));
        
        auto call = std::make_unique<CallExpressionNode>();
        auto callee = std::make_unique<IdentifierNode>();
        callee->name = "URL_native_ctor";
        call->callee = std::move(callee);
        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        auto arg = std::make_unique<IdentifierNode>();
        arg->name = "url_str";
        call->arguments.push_back(std::move(arg));
        
        auto stmt = std::make_unique<ExpressionStatementNode>();
        stmt->expression = std::move(call);
        ctor->body.push_back(std::move(stmt));
        
        classDesc->body->methods.push_back(std::move(ctor));
    }
    
    // Methods
    auto add_method = [&](const std::string& name, const std::vector<std::string>& params = {}) {
        auto m = std::make_unique<ClassMethodNode>();
        m->name = name;
        
        for (const auto& pn : params) {
            auto pnode = std::make_unique<ParameterNode>();
            pnode->name = pn;
            m->params.push_back(std::move(pnode));
        }
        
        auto call = std::make_unique<CallExpressionNode>();
        auto callee = std::make_unique<IdentifierNode>();
        callee->name = "URL_native_" + name;
        call->callee = std::move(callee);
        call->arguments.push_back(std::make_unique<ThisExpressionNode>());
        
        for (const auto& p : params) {
            auto id = std::make_unique<IdentifierNode>();
            id->name = p;
            call->arguments.push_back(std::move(id));
        }
        
        auto ret = std::make_unique<ReturnStatementNode>();
        ret->value = std::move(call);
        m->body.push_back(std::move(ret));
        
        classDesc->body->methods.push_back(std::move(m));
    };
    
    add_method("toString");
    add_method("normalize");
    add_method("clone");
    add_method("setQuery", {"key", "value"});
    add_method("getQuery", {"key"});
    add_method("deleteQuery", {"key"});
    add_method("hasQuery", {"key"});
    add_method("getPathSegments");
    add_method("getPathSegment", {"index"});
    add_method("matchPath", {"pattern"});
    
    // Static methods
    classDesc->static_table = std::make_shared<ObjectValue>();
    Token staticTok;
    
    classDesc->static_table->properties["encode"] = {
        std::make_shared<FunctionValue>("encode", native_URL_encode, env, staticTok),
        false, false, false, staticTok
    };
    classDesc->static_table->properties["decode"] = {
        std::make_shared<FunctionValue>("decode", native_URL_decode, env, staticTok),
        false, false, false, staticTok
    };
    classDesc->static_table->properties["encodeURIComponent"] = {
        std::make_shared<FunctionValue>("encodeURIComponent", native_URL_encodeURIComponent, env, staticTok),
        false, false, false, staticTok
    };
    classDesc->static_table->properties["decodeURIComponent"] = {
        std::make_shared<FunctionValue>("decodeURIComponent", native_URL_decodeURIComponent, env, staticTok),
        false, false, false, staticTok
    };
    classDesc->static_table->properties["encodeURI"] = {
        std::make_shared<FunctionValue>("encodeURI", native_URL_encodeURI, env, staticTok),
        false, false, false, staticTok
    };
    classDesc->static_table->properties["decodeURI"] = {
        std::make_shared<FunctionValue>("decodeURI", native_URL_decodeURI, env, staticTok),
        false, false, false, staticTok
    };
    
    Environment::Variable var;
    var.value = classDesc;
    var.is_constant = true;
    env->set("URL", var);
}