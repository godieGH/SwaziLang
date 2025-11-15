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

// ============================================
// ========== NATIVE STATIC METHODS ===========
// ============================================

static Value native_URL_encode(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "URL.encode expects a string argument", tok.loc);
    }
    return Value{percent_encode(std::get<std::string>(args[0]), false)};
}

static Value native_URL_decode(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "URL.decode expects a string argument", tok.loc);
    }
    return Value{percent_decode(std::get<std::string>(args[0]))};
}

static Value native_URL_encodeURIComponent(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "URL.encodeURIComponent expects a string argument", tok.loc);
    }
    return Value{percent_encode(std::get<std::string>(args[0]), false)};
}

static Value native_URL_decodeURIComponent(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "URL.decodeURIComponent expects a string argument", tok.loc);
    }
    return Value{percent_decode(std::get<std::string>(args[0]))};
}

static Value native_URL_encodeURI(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "URL.encodeURI expects a string argument", tok.loc);
    }
    return Value{percent_encode(std::get<std::string>(args[0]), true)};
}

static Value native_URL_decodeURI(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.empty()) return Value{std::string("")};
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "URL.decodeURI expects a string argument", tok.loc);
    }
    return Value{percent_decode(std::get<std::string>(args[0]))};
}

// ============================================
// ========== URLSearchParams METHODS =========
// ============================================

// These operate on the searchParams object which has __parent_url__ link

static Value native_searchParams_get(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw SwaziError("TypeError", "get requires key", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string search = std::get<std::string>(url->properties["search"].value);
    std::string key = std::get<std::string>(args[1]);
    
    // Simple query parsing
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
    if (args.size() < 2) throw SwaziError("TypeError", "getAll requires key", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string search = std::get<std::string>(url->properties["search"].value);
    std::string key = std::get<std::string>(args[1]);
    
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
    if (args.size() < 3) throw SwaziError("TypeError", "set requires key and value", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string key = std::get<std::string>(args[1]);
    std::string value = std::get<std::string>(args[2]);
    
    std::string search = std::get<std::string>(url->properties["search"].value);
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
    url->properties["href"].value = Value{parse_url(std::get<std::string>(url->properties["protocol"].value) + "//" + 
        std::get<std::string>(url->properties["hostname"].value) + std::get<std::string>(url->properties["pathname"].value) + 
        std::get<std::string>(url->properties["search"].value) + std::get<std::string>(url->properties["hash"].value)).href()};
    
    return std::monostate{};
}

static Value native_searchParams_append(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 3) throw SwaziError("TypeError", "append requires key and value", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string key = std::get<std::string>(args[1]);
    std::string value = std::get<std::string>(args[2]);
    
    std::string search = std::get<std::string>(url->properties["search"].value);
    std::string q = search.empty() || search == "?" ? "" : (search[0] == '?' ? search.substr(1) : search);
    
    std::ostringstream result;
    result << q;
    if (!q.empty()) result << "&";
    result << percent_encode(key, false) << "=" << percent_encode(value, false);
    
    std::string new_search = result.str();
    url->properties["search"].value = Value{"?" + new_search};
    
    return std::monostate{};
}

static Value native_searchParams_delete(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw SwaziError("TypeError", "delete requires key", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string key = std::get<std::string>(args[1]);
    
    std::string search = std::get<std::string>(url->properties["search"].value);
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
    
    return std::monostate{};
}

static Value native_searchParams_has(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    if (args.size() < 2) throw SwaziError("TypeError", "has requires key", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string search = std::get<std::string>(url->properties["search"].value);
    std::string key = std::get<std::string>(args[1]);
    
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
    if (args.empty()) throw SwaziError("TypeError", "Invalid searchParams", tok.loc);
    ObjectPtr sp = std::get<ObjectPtr>(args[0]);
    ObjectPtr url = std::get<ObjectPtr>(sp->properties["__parent_url__"].value);
    std::string search = std::get<std::string>(url->properties["search"].value);
    
    if (!search.empty() && search[0] == '?') {
        return Value{search.substr(1)};
    }
    return Value{search};
}

// ============================================
// ========== URL CONSTRUCTOR =================
// ============================================

static Value native_URL_ctor(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    if (args.size() < 2) throw SwaziError("TypeError", "URL constructor requires (this, url_str)", tok.loc);
    ObjectPtr instance = std::get<ObjectPtr>(args[0]);
    std::string url_str = std::get<std::string>(args[1]);
    
    // Parse URL
    URLComponents comp = parse_url(url_str);
    
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
    
    // Add searchParams methods
    Token spTok;
    sp->properties["get"] = {std::make_shared<FunctionValue>("get", native_searchParams_get, env, spTok), false, false, false, spTok};
    sp->properties["getAll"] = {std::make_shared<FunctionValue>("getAll", native_searchParams_getAll, env, spTok), false, false, false, spTok};
    sp->properties["set"] = {std::make_shared<FunctionValue>("set", native_searchParams_set, env, spTok), false, false, false, spTok};
    sp->properties["append"] = {std::make_shared<FunctionValue>("append", native_searchParams_append, env, spTok), false, false, false, spTok};
    sp->properties["delete"] = {std::make_shared<FunctionValue>("delete", native_searchParams_delete, env, spTok), false, false, false, spTok};
    sp->properties["has"] = {std::make_shared<FunctionValue>("has", native_searchParams_has, env, spTok), false, false, false, spTok};
    sp->properties["toString"] = {std::make_shared<FunctionValue>("toString", native_searchParams_toString, env, spTok), false, false, false, spTok};
    
    instance->properties["searchParams"].value = Value{sp};
    
    return std::monostate{};
}

// ============================================
// ========== URL INSTANCE METHODS ============
// ============================================

static Value native_url_toString(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    return obj->properties["href"].value;
}

static Value native_url_normalize(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    ObjectPtr obj = std::get<ObjectPtr>(args[0]);
    std::string path = std::get<std::string>(obj->properties["pathname"].value);
    obj->properties["pathname"].value = Value{normalize_path(path)};
    return Value{obj};
}

static Value native_url_clone(const std::vector<Value>& args, EnvPtr env, const Token& tok) {
    ObjectPtr original = std::get<ObjectPtr>(args[0]);
    std::string href = std::get<std::string>(original->properties["href"].value);
    
    // Create new URL from href
    return native_URL_ctor({std::make_shared<ObjectValue>(), Value{href}}, env, tok);
}

// Shortcut methods
static Value native_url_setQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    ObjectPtr url = std::get<ObjectPtr>(args[0]);
    ObjectPtr sp = std::get<ObjectPtr>(url->properties["searchParams"].value);
    return native_searchParams_set({sp, args[1], args[2]}, nullptr, tok);
}

static Value native_url_getQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    ObjectPtr url = std::get<ObjectPtr>(args[0]);
    ObjectPtr sp = std::get<ObjectPtr>(url->properties["searchParams"].value);
    return native_searchParams_get({sp, args[1]}, nullptr, tok);
}

static Value native_url_deleteQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    ObjectPtr url = std::get<ObjectPtr>(args[0]);
    ObjectPtr sp = std::get<ObjectPtr>(url->properties["searchParams"].value);
    return native_searchParams_delete({sp, args[1]}, nullptr, tok);
}

static Value native_url_hasQuery(const std::vector<Value>& args, EnvPtr, const Token& tok) {
    ObjectPtr url = std::get<ObjectPtr>(args[0]);
    ObjectPtr sp = std::get<ObjectPtr>(url->properties["searchParams"].value);
    return native_searchParams_has({sp, args[1]}, nullptr, tok);
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