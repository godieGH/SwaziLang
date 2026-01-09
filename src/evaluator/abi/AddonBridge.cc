// AddonBridge.cpp -
#include "swazi_abi.h"
#include "evaluator.hpp"
#include "SwaziError.hpp"

#include <cstring>
#include <map>
#include <memory>
#include <mutex> 
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct SwaziValueDeleter {
    void operator()(swazi_value v) const {
        if (v) delete v;
    }
};
using SwaziValuePtr = std::unique_ptr<swazi_value_s, SwaziValueDeleter>;

// ============================================================================
// Internal Structures (never exposed to addons)
// ============================================================================

struct swazi_env_s {
    Evaluator* evaluator;
    EnvPtr env_ptr;
    std::string last_error_code;
    std::string last_error_message;
    Value last_exception;
    bool exception_pending = false;
};

struct swazi_value_s {
    Value internal_value;
    
    swazi_value_s() : internal_value(std::monostate{}) {}
    explicit swazi_value_s(const Value& v) : internal_value(v) {}
};

struct swazi_callback_info_s {
    std::vector<Value> args;
    ObjectPtr this_object;
    Value new_target;
    void* user_data;
};

struct swazi_deferred_s {
    PromisePtr promise;
};

struct swazi_ref_s {
    Value value;
    uint32_t refcount;
};

// ============================================================================
// Global API Table
// ============================================================================

static swazi_api g_api = {};
static bool g_api_initialized = false;

// ============================================================================
// Helper Functions
// ============================================================================

static swazi_value wrap_value(const Value& v) {
    return new swazi_value_s(v);
}

static Value unwrap_value(swazi_value v) {
    if (!v) return std::monostate{};
    return v->internal_value;
}

static void set_error(swazi_env env, const char* code, const char* msg) {
    if (!env) return;
    env->last_error_code = code ? code : "";
    env->last_error_message = msg ? msg : "";
}

// ============================================================================
// API Implementation - Environment Operations
// ============================================================================

static swazi_status api_get_undefined(swazi_env env, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(std::monostate{});
    return SWAZI_OK;
}

static swazi_status api_get_null(swazi_env env, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(std::monostate{});
    return SWAZI_OK;
}

static swazi_status api_get_global(swazi_env env, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    if (!env->evaluator) return SWAZI_GENERIC_FAILURE;
    
    // Return global environment as an object proxy
    auto global_obj = std::make_shared<ObjectValue>();
    global_obj->is_env_proxy = true;
    global_obj->proxy_env = env->evaluator->get_global_env();
    
    *result = wrap_value(Value{global_obj});
    return SWAZI_OK;
}

static swazi_status api_get_boolean(swazi_env env, bool value, 
                                    swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(Value{value});
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Type Checking
// ============================================================================

static swazi_status api_typeof_value(swazi_env env, swazi_value value,
                                     swazi_valuetype* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    
    if (std::holds_alternative<std::monostate>(v)) {
        *result = SWAZI_NULL; // or UNDEFINED
    } else if (std::holds_alternative<bool>(v)) {
        *result = SWAZI_BOOLEAN;
    } else if (std::holds_alternative<double>(v)) {
        *result = SWAZI_NUMBER;
    } else if (std::holds_alternative<std::string>(v)) {
        *result = SWAZI_STRING;
    } else if (std::holds_alternative<FunctionPtr>(v)) {
        *result = SWAZI_FUNCTION;
    } else if (std::holds_alternative<ArrayPtr>(v)) {
        *result = SWAZI_ARRAY;
    } else if (std::holds_alternative<ObjectPtr>(v)) {
        *result = SWAZI_OBJECT;
    } else if (std::holds_alternative<BufferPtr>(v)) {
        *result = SWAZI_BUFFER;
    } else if (std::holds_alternative<PromisePtr>(v)) {
        *result = SWAZI_PROMISE;
    } else if (std::holds_alternative<DateTimePtr>(v)) {
        *result = SWAZI_DATETIME;
    } else if (std::holds_alternative<RangePtr>(v)) {
        *result = SWAZI_RANGE;
    } else {
        *result = SWAZI_OBJECT;
    }
    
    return SWAZI_OK;
}

static swazi_status api_is_array(swazi_env env, swazi_value value, 
                                 bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    *result = std::holds_alternative<ArrayPtr>(unwrap_value(value));
    return SWAZI_OK;
}

static swazi_status api_is_buffer(swazi_env env, swazi_value value,
                                  bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    *result = std::holds_alternative<BufferPtr>(unwrap_value(value));
    return SWAZI_OK;
}

static swazi_status api_is_error(swazi_env env, swazi_value value,
                                 bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<ObjectPtr>(v)) {
        *result = false;
        return SWAZI_OK;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(v);
    auto it = obj->properties.find("__error__");
    *result = (it != obj->properties.end());
    return SWAZI_OK;
}

static swazi_status api_is_promise(swazi_env env, swazi_value value,
                                   bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    *result = std::holds_alternative<PromisePtr>(unwrap_value(value));
    return SWAZI_OK;
}

static swazi_status api_is_date(swazi_env env, swazi_value value,
                                bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    *result = std::holds_alternative<DateTimePtr>(unwrap_value(value));
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Boolean Operations
// ============================================================================

static swazi_status api_get_value_bool(swazi_env env, swazi_value value,
                                       bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<bool>(v)) {
        set_error(env, "TypeError", "Value is not a boolean");
        return SWAZI_BOOLEAN_EXPECTED;
    }
    
    *result = std::get<bool>(v);
    return SWAZI_OK;
}

static swazi_status api_create_bool(swazi_env env, bool value,
                                    swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(Value{value});
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Number Operations
// ============================================================================

static swazi_status api_get_value_double(swazi_env env, swazi_value value,
                                         double* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<double>(v)) {
        set_error(env, "TypeError", "Value is not a number");
        return SWAZI_NUMBER_EXPECTED;
    }
    
    *result = std::get<double>(v);
    return SWAZI_OK;
}

static swazi_status api_get_value_int32(swazi_env env, swazi_value value,
                                        int32_t* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<double>(v)) {
        set_error(env, "TypeError", "Value is not a number");
        return SWAZI_NUMBER_EXPECTED;
    }
    
    *result = static_cast<int32_t>(std::get<double>(v));
    return SWAZI_OK;
}

static swazi_status api_get_value_uint32(swazi_env env, swazi_value value,
                                         uint32_t* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<double>(v)) {
        set_error(env, "TypeError", "Value is not a number");
        return SWAZI_NUMBER_EXPECTED;
    }
    
    *result = static_cast<uint32_t>(std::get<double>(v));
    return SWAZI_OK;
}

static swazi_status api_get_value_int64(swazi_env env, swazi_value value,
                                        int64_t* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<double>(v)) {
        set_error(env, "TypeError", "Value is not a number");
        return SWAZI_NUMBER_EXPECTED;
    }
    
    *result = static_cast<int64_t>(std::get<double>(v));
    return SWAZI_OK;
}

static swazi_status api_create_double(swazi_env env, double value,
                                      swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(Value{value});
    return SWAZI_OK;
}

static swazi_status api_create_int32(swazi_env env, int32_t value,
                                     swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(Value{static_cast<double>(value)});
    return SWAZI_OK;
}

static swazi_status api_create_uint32(swazi_env env, uint32_t value,
                                      swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(Value{static_cast<double>(value)});
    return SWAZI_OK;
}

static swazi_status api_create_int64(swazi_env env, int64_t value,
                                     swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    *result = wrap_value(Value{static_cast<double>(value)});
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - String Operations
// ============================================================================

static swazi_status api_get_value_string_utf8(swazi_env env, swazi_value value,
                                               char* buf, size_t bufsize,
                                               size_t* result) {
    if (!env || !value) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<std::string>(v)) {
        set_error(env, "TypeError", "Value is not a string");
        return SWAZI_STRING_EXPECTED;
    }
    
    const std::string& str = std::get<std::string>(v);
    
    if (result) {
        *result = str.length();
    }
    
    if (buf && bufsize > 0) {
        size_t copy_len = std::min(str.length(), bufsize - 1);
        std::memcpy(buf, str.c_str(), copy_len);
        buf[copy_len] = '\0';
    }
    
    return SWAZI_OK;
}

static swazi_status api_get_value_string_length(swazi_env env, 
                                                 swazi_value value,
                                                 size_t* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<std::string>(v)) {
        set_error(env, "TypeError", "Value is not a string");
        return SWAZI_STRING_EXPECTED;
    }
    
    *result = std::get<std::string>(v).length();
    return SWAZI_OK;
}

static swazi_status api_create_string_utf8(swazi_env env, const char* str,
                                           size_t length, swazi_value* result) {
    if (!env || !str || !result) return SWAZI_INVALID_ARG;
    
    std::string s;
    if (length == (size_t)-1) {
        s = std::string(str);
    } else {
        s = std::string(str, length);
    }
    
    *result = wrap_value(Value{s});
    return SWAZI_OK;
}

static swazi_status api_create_string_latin1(swazi_env env, const char* str,
                                             size_t length, swazi_value* result) {
    // For simplicity, treat latin1 same as utf8
    return api_create_string_utf8(env, str, length, result);
}

// ============================================================================
// API Implementation - Object Operations
// ============================================================================

static swazi_status api_create_object(swazi_env env, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto obj = std::make_shared<ObjectValue>();
    *result = wrap_value(Value{obj});
    return SWAZI_OK;
}

static swazi_status api_get_property(swazi_env env, swazi_value object,
                                     swazi_value key, swazi_value* result) {
    if (!env || !object || !key || !result) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    const Value& key_val = unwrap_value(key);
    if (!std::holds_alternative<std::string>(key_val)) {
        set_error(env, "TypeError", "Property key must be a string");
        return SWAZI_STRING_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str = std::get<std::string>(key_val);
    
    auto it = obj->properties.find(key_str);
    if (it == obj->properties.end()) {
        *result = wrap_value(std::monostate{});
    } else {
        *result = wrap_value(it->second.value);
    }
    
    return SWAZI_OK;
}

static swazi_status api_get_named_property(swazi_env env, swazi_value object,
                                           const char* utf8name,
                                           swazi_value* result) {
    if (!env || !object || !utf8name || !result) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str(utf8name);
    
    auto it = obj->properties.find(key_str);
    if (it == obj->properties.end()) {
        *result = wrap_value(std::monostate{});
    } else {
        *result = wrap_value(it->second.value);
    }
    
    return SWAZI_OK;
}

static swazi_status api_set_property(swazi_env env, swazi_value object,
                                     swazi_value key, swazi_value value) {
    if (!env || !object || !key || !value) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    const Value& key_val = unwrap_value(key);
    if (!std::holds_alternative<std::string>(key_val)) {
        set_error(env, "TypeError", "Property key must be a string");
        return SWAZI_STRING_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str = std::get<std::string>(key_val);
    Value val = unwrap_value(value);
    
    obj->properties[key_str] = PropertyDescriptor{val, false, false, false, Token()};
    return SWAZI_OK;
}

static swazi_status api_set_named_property(swazi_env env, swazi_value object,
                                           const char* utf8name,
                                           swazi_value value) {
    if (!env || !object || !utf8name || !value) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str(utf8name);
    Value val = unwrap_value(value);
    
    obj->properties[key_str] = PropertyDescriptor{val, false, false, false, Token()};
    return SWAZI_OK;
}

static swazi_status api_has_property(swazi_env env, swazi_value object,
                                     swazi_value key, bool* result) {
    if (!env || !object || !key || !result) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    const Value& key_val = unwrap_value(key);
    if (!std::holds_alternative<std::string>(key_val)) {
        set_error(env, "TypeError", "Property key must be a string");
        return SWAZI_STRING_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str = std::get<std::string>(key_val);
    
    *result = (obj->properties.find(key_str) != obj->properties.end());
    return SWAZI_OK;
}

static swazi_status api_has_named_property(swazi_env env, swazi_value object,
                                           const char* utf8name, bool* result) {
    if (!env || !object || !utf8name || !result) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str(utf8name);
    
    *result = (obj->properties.find(key_str) != obj->properties.end());
    return SWAZI_OK;
}

static swazi_status api_delete_property(swazi_env env, swazi_value object,
                                        swazi_value key, bool* result) {
    if (!env || !object || !key) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    const Value& key_val = unwrap_value(key);
    if (!std::holds_alternative<std::string>(key_val)) {
        set_error(env, "TypeError", "Property key must be a string");
        return SWAZI_STRING_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    std::string key_str = std::get<std::string>(key_val);
    
    size_t erased = obj->properties.erase(key_str);
    if (result) *result = (erased > 0);
    
    return SWAZI_OK;
}

static swazi_status api_get_property_names(swazi_env env, swazi_value object,
                                           swazi_value* result) {
    if (!env || !object || !result) return SWAZI_INVALID_ARG;
    
    const Value& obj_val = unwrap_value(object);
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    auto arr = std::make_shared<ArrayValue>();
    
    for (const auto& kv : obj->properties) {
        arr->elements.push_back(Value{kv.first});
    }
    
    *result = wrap_value(Value{arr});
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Array Operations
// ============================================================================

static swazi_status api_create_array(swazi_env env, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto arr = std::make_shared<ArrayValue>();
    *result = wrap_value(Value{arr});
    return SWAZI_OK;
}

static swazi_status api_create_array_with_length(swazi_env env, size_t length,
                                                  swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto arr = std::make_shared<ArrayValue>();
    arr->elements.resize(length, std::monostate{});
    *result = wrap_value(Value{arr});
    return SWAZI_OK;
}

static swazi_status api_get_array_length(swazi_env env, swazi_value value,
                                         uint32_t* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<ArrayPtr>(v)) {
        set_error(env, "TypeError", "Value is not an array");
        return SWAZI_ARRAY_EXPECTED;
    }
    
    ArrayPtr arr = std::get<ArrayPtr>(v);
    *result = static_cast<uint32_t>(arr->elements.size());
    return SWAZI_OK;
}

static swazi_status api_get_element(swazi_env env, swazi_value array,
                                    uint32_t index, swazi_value* result) {
    if (!env || !array || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(array);
    if (!std::holds_alternative<ArrayPtr>(v)) {
        set_error(env, "TypeError", "Value is not an array");
        return SWAZI_ARRAY_EXPECTED;
    }
    
    ArrayPtr arr = std::get<ArrayPtr>(v);
    if (index >= arr->elements.size()) {
        *result = wrap_value(std::monostate{});
    } else {
        *result = wrap_value(arr->elements[index]);
    }
    
    return SWAZI_OK;
}

static swazi_status api_set_element(swazi_env env, swazi_value array,
                                    uint32_t index, swazi_value value) {
    if (!env || !array || !value) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(array);
    if (!std::holds_alternative<ArrayPtr>(v)) {
        set_error(env, "TypeError", "Value is not an array");
        return SWAZI_ARRAY_EXPECTED;
    }
    
    ArrayPtr arr = std::get<ArrayPtr>(v);
    Value val = unwrap_value(value);
    
    // Expand array if needed
    if (index >= arr->elements.size()) {
        arr->elements.resize(index + 1, std::monostate{});
    }
    
    arr->elements[index] = val;
    return SWAZI_OK;
}

static swazi_status api_has_element(swazi_env env, swazi_value array,
                                    uint32_t index, bool* result) {
    if (!env || !array || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(array);
    if (!std::holds_alternative<ArrayPtr>(v)) {
        set_error(env, "TypeError", "Value is not an array");
        return SWAZI_ARRAY_EXPECTED;
    }
    
    ArrayPtr arr = std::get<ArrayPtr>(v);
    *result = (index < arr->elements.size());
    return SWAZI_OK;
}

static swazi_status api_delete_element(swazi_env env, swazi_value array,
                                       uint32_t index, bool* result) {
    if (!env || !array) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(array);
    if (!std::holds_alternative<ArrayPtr>(v)) {
        set_error(env, "TypeError", "Value is not an array");
        return SWAZI_ARRAY_EXPECTED;
    }
    
    ArrayPtr arr = std::get<ArrayPtr>(v);
    if (index < arr->elements.size()) {
        arr->elements[index] = HoleValue{};
        if (result) *result = true;
    } else {
        if (result) *result = false;
    }
    
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Function Operations
// ============================================================================

static swazi_status api_create_function(swazi_env env, const char* utf8name,
                                        size_t length, swazi_callback cb,
                                        void* data, swazi_value* result) {
    if (!env || !cb || !result) return SWAZI_INVALID_ARG;
    
    std::string name;
    if (utf8name) {
        if (length == (size_t)-1) {
            name = std::string(utf8name);
        } else {
            name = std::string(utf8name, length);
        }
    } else {
        name = "anonymous";
    }
    
    // Create native implementation that bridges to addon callback
    auto native_impl = [cb, data, env](const std::vector<Value>& args,
                                       EnvPtr callEnv, const Token& token) -> Value {
        // Build callback info
        swazi_callback_info_s cbinfo;
        cbinfo.args = args;
        cbinfo.user_data = data;
        cbinfo.this_object = nullptr; // Will be set by call_function if method call
        cbinfo.new_target = std::monostate{};
        
        // Call addon's callback
        swazi_value result_handle = cb(env, &cbinfo);
        
        // Check for pending exception
        if (env->exception_pending) {
            env->exception_pending = false;
            throw SwaziError("AddonError", env->last_error_message, token.loc);
        }
        
        if (!result_handle) {
            return std::monostate{};
        }
        
        Value result = unwrap_value(result_handle);
        delete result_handle; // Clean up the handle
        return result;
    };
    
    auto fn = std::make_shared<FunctionValue>(name, native_impl,
                                              env->env_ptr, Token());
    *result = wrap_value(Value{fn});
    return SWAZI_OK;
}

static swazi_status api_call_function(swazi_env env, swazi_value recv,
                                      swazi_value func, size_t argc,
                                      const swazi_value* argv,
                                      swazi_value* result) {
    if (!env || !func) return SWAZI_INVALID_ARG;
    
    const Value& func_val = unwrap_value(func);
    if (!std::holds_alternative<FunctionPtr>(func_val)) {
        set_error(env, "TypeError", "Value is not a function");
        return SWAZI_FUNCTION_EXPECTED;
    }
    
    FunctionPtr fn = std::get<FunctionPtr>(func_val);
    
    // Build arguments
    std::vector<Value> args;
    args.reserve(argc);
    for (size_t i = 0; i < argc; i++) {
        args.push_back(unwrap_value(argv[i]));
    }
    
    // Call the function
    try {
        Value ret;
        if (recv) {
            const Value& recv_val = unwrap_value(recv);
            if (std::holds_alternative<ObjectPtr>(recv_val)) {
                ObjectPtr obj = std::get<ObjectPtr>(recv_val);
                ret = env->evaluator->call_function_with_receiver_public(
                    fn, obj, args, env->env_ptr, Token());
            } else {
                ret = env->evaluator->invoke_function(fn, args, env->env_ptr, Token());
            }
        } else {
            ret = env->evaluator->invoke_function(fn, args, env->env_ptr, Token());
        }
        
        if (result) {
            *result = wrap_value(ret);
        }
        return SWAZI_OK;
    } catch (const SwaziError& e) {
        set_error(env, "Error", e.what());
        env->exception_pending = true;
        return SWAZI_PENDING_EXCEPTION;
    } catch (const std::exception& e) {
        set_error(env, "Error", e.what());
        env->exception_pending = true;
        return SWAZI_PENDING_EXCEPTION;
    }
}

static swazi_status api_new_instance(swazi_env env, swazi_value constructor,
                                     size_t argc, const swazi_value* argv,
                                     swazi_value* result) {
    if (!env || !constructor) return SWAZI_INVALID_ARG;
    
    // For now, just call as function (simplified)
    return api_call_function(env, nullptr, constructor, argc, argv, result);
}

// ============================================================================
// API Implementation - Callback Info
// ============================================================================

static swazi_status api_get_cb_info(swazi_env env, swazi_callback_info cbinfo,
                                    size_t* argc, swazi_value* argv,
                                    swazi_value* this_arg, void** data) {
    if (!env || !cbinfo) return SWAZI_INVALID_ARG;
    
    if (argc) {
        *argc = cbinfo->args.size();
    }
    
    if (argv) {
        for (size_t i = 0; i < cbinfo->args.size(); i++) {
            argv[i] = wrap_value(cbinfo->args[i]);
        }
    }
    
    if (this_arg) {
        if (cbinfo->this_object) {
            *this_arg = wrap_value(Value{cbinfo->this_object});
        } else {
            *this_arg = nullptr;
        }
    }
    
    if (data) {
        *data = cbinfo->user_data;
    }
    
    return SWAZI_OK;
}

static swazi_status api_get_new_target(swazi_env env, 
                                       swazi_callback_info cbinfo,
                                       swazi_value* result) {
    if (!env || !cbinfo || !result) return SWAZI_INVALID_ARG;
    
    *result = wrap_value(cbinfo->new_target);
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Error Handling
// ============================================================================

static swazi_status api_throw_error(swazi_env env, const char* code,
                                    const char* msg) {
    if (!env) return SWAZI_INVALID_ARG;
    
    set_error(env, code ? code : "Error", msg ? msg : "Unknown error");
    env->exception_pending = true;
    
    return SWAZI_OK;
}

static swazi_status api_throw_type_error(swazi_env env, const char* code,
                                         const char* msg) {
    if (!env) return SWAZI_INVALID_ARG;
    
    set_error(env, code ? code : "TypeError", msg ? msg : "Type error");
    env->exception_pending = true;
    
    return SWAZI_OK;
}

static swazi_status api_throw_range_error(swazi_env env, const char* code,
                                          const char* msg) {
    if (!env) return SWAZI_INVALID_ARG;
    
    set_error(env, code ? code : "RangeError", msg ? msg : "Range error");
    env->exception_pending = true;
    
    return SWAZI_OK;
}

static swazi_status api_is_exception_pending(swazi_env env, bool* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    *result = env->exception_pending;
    return SWAZI_OK;
}

static swazi_status api_get_and_clear_last_exception(swazi_env env,
                                                      swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    if (env->exception_pending) {
        *result = wrap_value(env->last_exception);
        env->exception_pending = false;
        env->last_exception = std::monostate{};
    } else {
        *result = wrap_value(std::monostate{});
    }
    
    return SWAZI_OK;
}

static swazi_status api_create_error(swazi_env env, swazi_value code,
                                     swazi_value msg, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto err_obj = std::make_shared<ObjectValue>();
    err_obj->properties["__error__"] = PropertyDescriptor{Value{true}, false, false, false, Token()};
    err_obj->properties["code"] = PropertyDescriptor{unwrap_value(code), false, false, false, Token()};
    err_obj->properties["message"] = PropertyDescriptor{unwrap_value(msg), false, false, false, Token()};
    
    *result = wrap_value(Value{err_obj});
    return SWAZI_OK;
}

static swazi_status api_create_type_error(swazi_env env, swazi_value code,
                                          swazi_value msg, swazi_value* result) {
    return api_create_error(env, code, msg, result);
}

static swazi_status api_create_range_error(swazi_env env, swazi_value code,
                                           swazi_value msg, swazi_value* result) {
    return api_create_error(env, code, msg, result);
}

// ============================================================================
// API Implementation - Buffer Operations
// ============================================================================

static swazi_status api_create_buffer(swazi_env env, size_t length,
                                      void** data, swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto buf = std::make_shared<BufferValue>();
    buf->data.resize(length, 0);
    buf->encoding = "binary";
    
    if (data) {
        *data = buf->data.data();
    }
    
    *result = wrap_value(Value{buf});
    return SWAZI_OK;
}

static swazi_status api_create_external_buffer(swazi_env env, size_t length,
                                                void* data,
                                                swazi_finalize finalize_cb,
                                                void* finalize_hint,
                                                swazi_value* result) {
    if (!env || !data || !result) return SWAZI_INVALID_ARG;
    
    // For simplicity, copy the data
    // In a full implementation, you'd store the pointer and call finalize_cb
    auto buf = std::make_shared<BufferValue>();
    buf->data.assign(static_cast<uint8_t*>(data),
                     static_cast<uint8_t*>(data) + length);
    buf->encoding = "binary";
    
    // Call finalizer immediately since we copied (normally would defer)
    if (finalize_cb) {
        finalize_cb(env, data, finalize_hint);
    }
    
    *result = wrap_value(Value{buf});
    return SWAZI_OK;
}

static swazi_status api_create_buffer_copy(swazi_env env, size_t length,
                                           const void* data, void** result_data,
                                           swazi_value* result) {
    if (!env || !data || !result) return SWAZI_INVALID_ARG;
    
    auto buf = std::make_shared<BufferValue>();
    buf->data.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + length);
    buf->encoding = "binary";
    
    if (result_data) {
        *result_data = buf->data.data();
    }
    
    *result = wrap_value(Value{buf});
    return SWAZI_OK;
}

static swazi_status api_get_buffer_info(swazi_env env, swazi_value value,
                                        void** data, size_t* length) {
    if (!env || !value) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<BufferPtr>(v)) {
        set_error(env, "TypeError", "Value is not a buffer");
        return SWAZI_BUFFER_EXPECTED;
    }
    
    BufferPtr buf = std::get<BufferPtr>(v);
    
    if (data) {
        *data = buf->data.data();
    }
    
    if (length) {
        *length = buf->data.size();
    }
    
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Promise Operations
// ============================================================================

static swazi_status api_create_promise(swazi_env env, swazi_deferred* deferred,
                                       swazi_value* promise) {
    if (!env || !deferred || !promise) return SWAZI_INVALID_ARG;
    
    auto prom = std::make_shared<PromiseValue>();
    prom->state = PromiseValue::State::PENDING;
    
    auto def = new swazi_deferred_s();
    def->promise = prom;
    
    *deferred = def;
    *promise = wrap_value(Value{prom});
    
    return SWAZI_OK;
}

static swazi_status api_resolve_deferred(swazi_env env, swazi_deferred deferred,
                                         swazi_value resolution) {
    if (!env || !deferred || !resolution) return SWAZI_INVALID_ARG;
    
    Value res = unwrap_value(resolution);
    env->evaluator->fulfill_promise(deferred->promise, res);
    
    delete deferred;
    return SWAZI_OK;
}

static swazi_status api_reject_deferred(swazi_env env, swazi_deferred deferred,
                                        swazi_value rejection) {
    if (!env || !deferred || !rejection) return SWAZI_INVALID_ARG;
    
    Value rej = unwrap_value(rejection);
    env->evaluator->reject_promise(deferred->promise, rej);
    
    delete deferred;
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Reference Management
// ============================================================================

static std::map<swazi_ref, std::shared_ptr<swazi_ref_s>> g_refs;
static std::mutex g_refs_mutex;

static swazi_status api_create_reference(swazi_env env, swazi_value value,
                                         uint32_t initial_refcount,
                                         swazi_ref* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    auto ref = std::make_shared<swazi_ref_s>();
    ref->value = unwrap_value(value);
    ref->refcount = initial_refcount;
    
    std::lock_guard<std::mutex> lock(g_refs_mutex);
    g_refs[ref.get()] = ref;
    
    *result = ref.get();
    return SWAZI_OK;
}

static swazi_status api_delete_reference(swazi_env env, swazi_ref ref) {
    if (!env || !ref) return SWAZI_INVALID_ARG;
    
    std::lock_guard<std::mutex> lock(g_refs_mutex);
    g_refs.erase(ref);
    
    return SWAZI_OK;
}

static swazi_status api_reference_ref(swazi_env env, swazi_ref ref,
                                      uint32_t* result) {
    if (!env || !ref) return SWAZI_INVALID_ARG;
    
    std::lock_guard<std::mutex> lock(g_refs_mutex);
    auto it = g_refs.find(ref);
    if (it == g_refs.end()) return SWAZI_INVALID_ARG;
    
    it->second->refcount++;
    if (result) *result = it->second->refcount;
    
    return SWAZI_OK;
}

static swazi_status api_reference_unref(swazi_env env, swazi_ref ref,
                                        uint32_t* result) {
    if (!env || !ref) return SWAZI_INVALID_ARG;
    
    std::lock_guard<std::mutex> lock(g_refs_mutex);
    auto it = g_refs.find(ref);
    if (it == g_refs.end()) return SWAZI_INVALID_ARG;
    
    if (it->second->refcount > 0) {
        it->second->refcount--;
    }
    
    if (result) *result = it->second->refcount;
    
    // Auto-delete when refcount reaches 0
    if (it->second->refcount == 0) {
        g_refs.erase(it);
    }
    
    return SWAZI_OK;
}

static swazi_status api_get_reference_value(swazi_env env, swazi_ref ref,
                                            swazi_value* result) {
    if (!env || !ref || !result) return SWAZI_INVALID_ARG;
    
    std::lock_guard<std::mutex> lock(g_refs_mutex);
    auto it = g_refs.find(ref);
    if (it == g_refs.end()) return SWAZI_INVALID_ARG;
    
    *result = wrap_value(it->second->value);
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Type Coercion
// ============================================================================

static swazi_status api_coerce_to_bool(swazi_env env, swazi_value value,
                                       swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    bool b = env->evaluator->to_bool_public(v);
    *result = wrap_value(Value{b});
    
    return SWAZI_OK;
}

static swazi_status api_coerce_to_number(swazi_env env, swazi_value value,
                                         swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    try {
        const Value& v = unwrap_value(value);
        double d = env->evaluator->to_number_public(v, Token());
        *result = wrap_value(Value{d});
        return SWAZI_OK;
    } catch (...) {
        set_error(env, "TypeError", "Cannot coerce to number");
        return SWAZI_GENERIC_FAILURE;
    }
}

static swazi_status api_coerce_to_string(swazi_env env, swazi_value value,
                                         swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    std::string s = env->evaluator->to_string_value_public(v, true);
    *result = wrap_value(Value{s});
    
    return SWAZI_OK;
}

static swazi_status api_coerce_to_object(swazi_env env, swazi_value value,
                                         swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    
    // If already an object, return as-is
    if (std::holds_alternative<ObjectPtr>(v)) {
        *result = wrap_value(v);
        return SWAZI_OK;
    }
    
    // Otherwise create wrapper object
    auto obj = std::make_shared<ObjectValue>();
    obj->properties["value"] = PropertyDescriptor{v, false, false, false, Token()};
    *result = wrap_value(Value{obj});
    
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Strict Equality
// ============================================================================

static swazi_status api_strict_equals(swazi_env env, swazi_value lhs,
                                      swazi_value rhs, bool* result) {
    if (!env || !lhs || !rhs || !result) return SWAZI_INVALID_ARG;
    
    const Value& l = unwrap_value(lhs);
    const Value& r = unwrap_value(rhs);
    
    *result = env->evaluator->is_strict_equal_public(l, r);
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - External Data
// ============================================================================

struct ExternalData {
    void* data;
    swazi_finalize finalize_cb;
    void* finalize_hint;
    swazi_env env;
};

static std::map<ObjectPtr, std::shared_ptr<ExternalData>> g_external_data;
static std::mutex g_external_mutex;

static swazi_status api_create_external(swazi_env env, void* data,
                                        swazi_finalize finalize_cb,
                                        void* finalize_hint,
                                        swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto obj = std::make_shared<ObjectValue>();
    
    // Store external data
    auto ext_data = std::make_shared<ExternalData>();
    ext_data->data = data;
    ext_data->finalize_cb = finalize_cb;
    ext_data->finalize_hint = finalize_hint;
    ext_data->env = env;
    
    {
        std::lock_guard<std::mutex> lock(g_external_mutex);
        g_external_data[obj] = ext_data;
    }
    
    // Mark object as external
    obj->properties["__external__"] = PropertyDescriptor{
        Value{true}, false, false, false, Token()};
    
    *result = wrap_value(Value{obj});
    return SWAZI_OK;
}

static swazi_status api_get_value_external(swazi_env env, swazi_value value,
                                           void** result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<ObjectPtr>(v)) {
        set_error(env, "TypeError", "Value is not an external object");
        return SWAZI_OBJECT_EXPECTED;
    }
    
    ObjectPtr obj = std::get<ObjectPtr>(v);
    
    std::lock_guard<std::mutex> lock(g_external_mutex);
    auto it = g_external_data.find(obj);
    if (it == g_external_data.end()) {
        set_error(env, "TypeError", "Object is not external");
        return SWAZI_GENERIC_FAILURE;
    }
    
    *result = it->second->data;
    return SWAZI_OK;
}

// Cleanup external data when objects are destroyed
void cleanup_external_object(ObjectPtr obj) {
    std::lock_guard<std::mutex> lock(g_external_mutex);
    auto it = g_external_data.find(obj);
    if (it != g_external_data.end()) {
        auto ext = it->second;
        if (ext->finalize_cb) {
            ext->finalize_cb(ext->env, ext->data, ext->finalize_hint);
        }
        g_external_data.erase(it);
    }
}

// ============================================================================
// API Implementation - DateTime Operations
// ============================================================================

static swazi_status api_create_date(swazi_env env, double time,
                                    swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;
    
    auto dt = std::make_shared<DateTimeValue>();
    dt->epochNanoseconds = static_cast<uint64_t>(time * 1000000.0); // ms to ns
    dt->recompute_calendar_fields();
    dt->update_literal_text();
    
    *result = wrap_value(Value{dt});
    return SWAZI_OK;
}

static swazi_status api_get_date_value(swazi_env env, swazi_value value,
                                       double* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    
    DateTimePtr dt = std::get<DateTimePtr>(v);
    *result = static_cast<double>(dt->epochNanoseconds) / 1000000.0; // ns to ms
    
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Instance Checking
// ============================================================================

static swazi_status api_instanceof(swazi_env env, swazi_value object,
                                   swazi_value constructor, bool* result) {
    if (!env || !object || !constructor || !result) return SWAZI_INVALID_ARG;
    
    // Simplified instanceof check
    // In full implementation, walk prototype chain
    
    const Value& obj_val = unwrap_value(object);
    const Value& ctor_val = unwrap_value(constructor);
    
    if (!std::holds_alternative<ObjectPtr>(obj_val)) {
        *result = false;
        return SWAZI_OK;
    }
    
    if (!std::holds_alternative<FunctionPtr>(ctor_val) &&
        !std::holds_alternative<ClassPtr>(ctor_val)) {
        *result = false;
        return SWAZI_OK;
    }
    
    // For now, just check if object has __class__ property matching constructor
    ObjectPtr obj = std::get<ObjectPtr>(obj_val);
    auto it = obj->properties.find("__class__");
    
    if (it == obj->properties.end()) {
        *result = false;
        return SWAZI_OK;
    }
    
    *result = env->evaluator->is_strict_equal_public(it->second.value, ctor_val);
    return SWAZI_OK;
}

// ============================================================================
// API Initialization
// ============================================================================

void init_addon_api() {
    if (g_api_initialized) return;
    
    // Environment operations
    g_api.get_undefined = api_get_undefined;
    g_api.get_null = api_get_null;
    g_api.get_global = api_get_global;
    g_api.get_boolean = api_get_boolean;
    
    // Type checking
    g_api.typeof_value = api_typeof_value;
    g_api.is_array = api_is_array;
    g_api.is_buffer = api_is_buffer;
    g_api.is_error = api_is_error;
    g_api.is_promise = api_is_promise;
    g_api.is_date = api_is_date;
    
    // Boolean operations
    g_api.get_value_bool = api_get_value_bool;
    g_api.create_bool = api_create_bool;
    
    // Number operations
    g_api.get_value_double = api_get_value_double;
    g_api.get_value_int32 = api_get_value_int32;
    g_api.get_value_uint32 = api_get_value_uint32;
    g_api.get_value_int64 = api_get_value_int64;
    g_api.create_double = api_create_double;
    g_api.create_int32 = api_create_int32;
    g_api.create_uint32 = api_create_uint32;
    g_api.create_int64 = api_create_int64;
    
    // String operations
    g_api.get_value_string_utf8 = api_get_value_string_utf8;
    g_api.get_value_string_length = api_get_value_string_length;
    g_api.create_string_utf8 = api_create_string_utf8;
    g_api.create_string_latin1 = api_create_string_latin1;
    
    // Object operations
    g_api.create_object = api_create_object;
    g_api.get_property = api_get_property;
    g_api.get_named_property = api_get_named_property;
    g_api.set_property = api_set_property;
    g_api.set_named_property = api_set_named_property;
    g_api.has_property = api_has_property;
    g_api.has_named_property = api_has_named_property;
    g_api.delete_property = api_delete_property;
    g_api.get_property_names = api_get_property_names;
    
    // Array operations
    g_api.create_array = api_create_array;
    g_api.create_array_with_length = api_create_array_with_length;
    g_api.get_array_length = api_get_array_length;
    g_api.get_element = api_get_element;
    g_api.set_element = api_set_element;
    g_api.has_element = api_has_element;
    g_api.delete_element = api_delete_element;
    
    // Function operations
    g_api.create_function = api_create_function;
    g_api.call_function = api_call_function;
    g_api.new_instance = api_new_instance;
    
    // Callback info
    g_api.get_cb_info = api_get_cb_info;
    g_api.get_new_target = api_get_new_target;
    
    // Error handling
    g_api.throw_error = api_throw_error;
    g_api.throw_type_error = api_throw_type_error;
    g_api.throw_range_error = api_throw_range_error;
    g_api.is_exception_pending = api_is_exception_pending;
    g_api.get_and_clear_last_exception = api_get_and_clear_last_exception;
    g_api.create_error = api_create_error;
    g_api.create_type_error = api_create_type_error;
    g_api.create_range_error = api_create_range_error;
    
    // Buffer operations
    g_api.create_buffer = api_create_buffer;
    g_api.create_external_buffer = api_create_external_buffer;
    g_api.create_buffer_copy = api_create_buffer_copy;
    g_api.get_buffer_info = api_get_buffer_info;
    
    // Promise operations
    g_api.create_promise = api_create_promise;
    g_api.resolve_deferred = api_resolve_deferred;
    g_api.reject_deferred = api_reject_deferred;
    
    // Reference management
    g_api.create_reference = api_create_reference;
    g_api.delete_reference = api_delete_reference;
    g_api.reference_ref = api_reference_ref;
    g_api.reference_unref = api_reference_unref;
    g_api.get_reference_value = api_get_reference_value;
    
    // Type coercion
    g_api.coerce_to_bool = api_coerce_to_bool;
    g_api.coerce_to_number = api_coerce_to_number;
    g_api.coerce_to_string = api_coerce_to_string;
    g_api.coerce_to_object = api_coerce_to_object;
    
    // Strict equality
    g_api.strict_equals = api_strict_equals;
    
    // External data
    g_api.create_external = api_create_external;
    g_api.get_value_external = api_get_value_external;
    
    // DateTime operations
    g_api.create_date = api_create_date;
    g_api.get_date_value = api_get_date_value;
    
    // Instance checking
    g_api.instanceof = api_instanceof;
    
    g_api_initialized = true;
}

// ============================================================================
// Public API Access
// ============================================================================

extern "C" const swazi_api* swazi_get_api() {
    if (!g_api_initialized) {
        init_addon_api();
    }
    return &g_api;
}

// ============================================================================
// Addon Loading
// ============================================================================

ObjectPtr load_addon(const std::string& path, Evaluator* evaluator, EnvPtr env) {
    init_addon_api();
    
    // Load the shared library
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path.c_str());
    if (!handle) {
        DWORD err = GetLastError();
        throw std::runtime_error("Failed to load addon: " + path + 
                               " (Error: " + std::to_string(err) + ")");
    }
    
    auto register_func = (swazi_addon_register_func)
        GetProcAddress(handle, "swazi_addon_register");
#else
    void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        throw std::runtime_error(std::string("Failed to load addon: ") + 
                               path + " (" + dlerror() + ")");
    }
    
    dlerror(); // Clear any existing error
    
    auto register_func = (swazi_addon_register_func)
        dlsym(handle, "swazi_addon_register");
    
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        dlclose(handle);
        throw std::runtime_error(std::string("Failed to find swazi_addon_register: ") + 
                               dlsym_error);
    }
#endif
    
    if (!register_func) {
        throw std::runtime_error("Addon missing swazi_addon_register function: " + path);
    }
    
    auto env_wrapper = std::make_unique<swazi_env_s>();
    env_wrapper->evaluator = evaluator;
    env_wrapper->env_ptr = env;
    
    auto exports = std::make_shared<ObjectValue>();
    SwaziValuePtr exports_handle(wrap_value(Value{exports}));
    
    // Call addon's register function
    try {
        swazi_value result = register_func(env_wrapper.get(), exports_handle.get());
        
        if (env_wrapper->exception_pending) {
            throw SwaziError("AddonError", 
                           env_wrapper->last_error_message,
                           Token{}.loc);
        }
        
        if (result && result != exports_handle.get()) {
            // Addon returned different object
            SwaziValuePtr result_ptr(result);
            Value result_val = unwrap_value(result);
            
            if (std::holds_alternative<ObjectPtr>(result_val)) {
                return std::get<ObjectPtr>(result_val);
            }
        }
        
    } catch (const SwaziError&) {
        throw;
    } catch (const std::exception& e) {
        throw SwaziError("AddonError", e.what(), Token{}.loc);
    }
    
    return exports;
}