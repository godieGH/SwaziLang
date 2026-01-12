// AddonBridge.cpp -
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AsyncBridge.hpp"
#include "ClassRuntime.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "swazi_abi.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static std::atomic<size_t> g_addon_active_threads{0};

// Add these helper functions
void addon_thread_started() {
    g_addon_active_threads.fetch_add(1);
}

void addon_thread_finished() {
    g_addon_active_threads.fetch_sub(1);
}

bool addon_threads_exist() {
    return g_addon_active_threads.load() > 0;
}

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

struct SwaziValueDeleter {
    void operator()(swazi_value v) const {
        if (v) delete v;
    }
};
using SwaziValuePtr = std::unique_ptr<swazi_value_s, SwaziValueDeleter>;

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
        *result = SWAZI_NULL;  // or UNDEFINED
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

    // ADD FROZEN CHECK:
    if (obj->is_frozen) {
        set_error(env, "TypeError", "Cannot modify frozen object");
        return SWAZI_GENERIC_FAILURE;
    }

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

    // ADD FROZEN CHECK:
    if (obj->is_frozen) {
        set_error(env, "TypeError", "Cannot modify frozen object");
        return SWAZI_GENERIC_FAILURE;
    }

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
        name = "<lambda>";
    }

    // Create native implementation that bridges to addon callback
    auto native_impl = [cb, data, env](const std::vector<Value>& args,
                           EnvPtr callEnv, const Token& token) -> Value {
        // Build callback info WITH receiver support
        swazi_callback_info_s cbinfo;
        cbinfo.args = args;
        cbinfo.user_data = data;

        // Try to extract receiver from environment's "$" binding
        cbinfo.this_object = nullptr;
        if (callEnv && callEnv->has("$")) {
            Value dollar = callEnv->get("$").value;
            if (std::holds_alternative<ObjectPtr>(dollar)) {
                cbinfo.this_object = std::get<ObjectPtr>(dollar);
            }
        }

        cbinfo.new_target = std::monostate{};

        // Call addon's callback
        swazi_value result_handle = cb(env, &cbinfo);

        // Check for pending exception FIRST
        if (env->exception_pending) {
            env->exception_pending = false;
            throw SwaziError(env->last_error_code, env->last_error_message, token.loc);
        }

        // Now safe to check result_handle
        if (!result_handle) {
            return std::monostate{};
        }

        Value result = unwrap_value(result_handle);
        delete result_handle;
        return result;
    };

    auto fn = std::make_shared<FunctionValue>(name, native_impl,
        env->env_ptr, Token());
    *result = wrap_value(Value{fn});
    return SWAZI_OK;
}

static swazi_status api_create_bound_function(
    swazi_env env,
    const char* utf8name,
    size_t length,
    swazi_callback cb,
    void* data,
    swazi_value receiver,
    swazi_value* result) {
    if (!env || !cb || !result || !receiver) return SWAZI_INVALID_ARG;

    std::string name;
    if (utf8name) {
        if (length == (size_t)-1) {
            name = std::string(utf8name);
        } else {
            name = std::string(utf8name, length);
        }
    } else {
        name = "<lambda>";
    }

    // Unwrap the receiver object
    Value receiver_val = unwrap_value(receiver);
    if (!std::holds_alternative<ObjectPtr>(receiver_val)) {
        set_error(env, "TypeError", "Receiver must be an object");
        return SWAZI_OBJECT_EXPECTED;
    }
    ObjectPtr receiver_obj = std::get<ObjectPtr>(receiver_val);

    // Create native implementation WITH pre-bound receiver
    auto native_impl = [cb, data, env, receiver_obj](
                           const std::vector<Value>& args,
                           EnvPtr callEnv,
                           const Token& token) -> Value {
        swazi_callback_info_s cbinfo;
        cbinfo.args = args;
        cbinfo.user_data = data;
        cbinfo.this_object = receiver_obj;  // ← Pre-bound receiver
        cbinfo.new_target = std::monostate{};

        swazi_value result_handle = cb(env, &cbinfo);

        if (env->exception_pending) {
            env->exception_pending = false;
            throw SwaziError(env->last_error_code, env->last_error_message, token.loc);
        }

        if (!result_handle) return std::monostate{};

        Value result = unwrap_value(result_handle);
        delete result_handle;
        return result;
    };

    // Create the function with a closure that has $ bound
    EnvPtr method_closure = std::make_shared<Environment>(env->env_ptr);
    Environment::Variable thisVar;
    thisVar.value = receiver_obj;
    thisVar.is_constant = true;
    method_closure->set("$", thisVar);

    auto fn = std::make_shared<FunctionValue>(
        name, native_impl, method_closure, Token());

    *result = wrap_value(Value{fn});
    return SWAZI_OK;
}

static swazi_status api_call_function(swazi_env env, swazi_value recv,
    swazi_value func, size_t argc,
    const swazi_value* argv,
    swazi_value* result) {
    if (!env || !func) return SWAZI_INVALID_ARG;

    const Value& fn_val = unwrap_value(func);
    if (!std::holds_alternative<FunctionPtr>(fn_val)) {
        set_error(env, "TypeError", "Value is not a function");
        return SWAZI_FUNCTION_EXPECTED;
    }

    FunctionPtr fn = std::get<FunctionPtr>(fn_val);

    // Convert arguments
    std::vector<Value> args;
    args.reserve(argc);
    for (size_t i = 0; i < argc; i++) {
        args.push_back(unwrap_value(argv[i]));
    }

    try {
        Value call_result;

        // Handle receiver if provided
        if (recv) {
            Value recv_val = unwrap_value(recv);
            if (std::holds_alternative<ObjectPtr>(recv_val)) {
                ObjectPtr receiver = std::get<ObjectPtr>(recv_val);
                call_result = env->evaluator->call_function_with_receiver_public(
                    fn, receiver, args, env->env_ptr, Token());
            } else {
                call_result = env->evaluator->call_function_public(
                    fn, args, env->env_ptr, Token());
            }
        } else {
            call_result = env->evaluator->call_function_public(
                fn, args, env->env_ptr, Token());
        }

        if (result) {
            *result = wrap_value(call_result);
        }

        return SWAZI_OK;
    } catch (const SwaziError& e) {
        set_error(env, "Error", e.what());
        return SWAZI_GENERIC_FAILURE;
    }
}

static swazi_status api_new_instance(swazi_env env, swazi_value constructor,
    size_t argc, const swazi_value* argv,
    swazi_value* result) {
    if (!env || !constructor || !result) return SWAZI_INVALID_ARG;

    // Unwrap constructor - must be a ClassPtr
    Value ctor_val = unwrap_value(constructor);
    if (!std::holds_alternative<ClassPtr>(ctor_val)) {
        set_error(env, "TypeError", "Constructor must be a class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(ctor_val);
    if (!cls) {
        set_error(env, "TypeError", "Invalid class value");
        return SWAZI_GENERIC_FAILURE;
    }

    // Create the instance object
    auto instance = std::make_shared<ObjectValue>();

    // Attach __class__ link (private property)
    PropertyDescriptor classLink;
    classLink.value = cls;
    classLink.is_private = true;
    classLink.token = Token();
    instance->properties["__class__"] = std::move(classLink);

    // Build class hierarchy chain (bottom-up, then reverse for top-down initialization)
    std::vector<ClassPtr> chain;
    for (ClassPtr walk = cls; walk; walk = walk->super) {
        chain.push_back(walk);
    }
    std::reverse(chain.begin(), chain.end());

    // Initialize properties and methods from each class in the chain (top-down)
    for (auto& c : chain) {
        if (!c || !c->body) continue;

        // Use the class's defining environment for evaluation
        EnvPtr classEnv = c->defining_env ? c->defining_env : env->env_ptr;

        // --- Initialize instance properties (non-static) ---
        for (auto& p : c->body->properties) {
            if (!p) continue;
            if (p->is_static) continue;

            // Create environment with $ bound to instance for initializer evaluation
            auto initEnv = std::make_shared<Environment>(classEnv);
            Environment::Variable thisVar;
            thisVar.value = instance;
            thisVar.is_constant = false;
            initEnv->set("$", thisVar);

            // Check for native property marker
            std::string marker_name = "__native_property_" + p->name;
            auto marker_it = c->static_table->properties.find(marker_name);

            Value initVal = std::monostate{};
            if (marker_it != c->static_table->properties.end()) {
                // Native property - use stored initial value
                initVal = marker_it->second.value;
            } else if (p->value) {
                // AST property - evaluate initializer expression
                initVal = env->evaluator->evaluate_expression_public(p->value.get(), initEnv);
            }

            PropertyDescriptor pd;
            pd.value = initVal;
            pd.is_private = p->is_private;
            pd.is_locked = p->is_locked;
            pd.is_readonly = false;
            pd.token = p->token;
            instance->properties[p->name] = std::move(pd);
        }

        // --- Initialize instance methods (non-static) ---
        for (auto& m : c->body->methods) {
            if (!m) continue;
            if (m->is_static) continue;

            // Skip constructors and destructors - they're handled separately
            if (m->is_constructor || m->is_destructor) continue;

            // Check for native method marker
            std::string marker_name = "__native_method_" + m->name;
            auto marker_it = c->static_table->properties.find(marker_name);

            if (marker_it != c->static_table->properties.end()) {
                // Native method found
                if (std::holds_alternative<FunctionPtr>(marker_it->second.value)) {
                    FunctionPtr nativeFn = std::get<FunctionPtr>(marker_it->second.value);

                    // Create closure with $ bound to instance
                    EnvPtr methodClosure = std::make_shared<Environment>(classEnv);
                    Environment::Variable thisVar;
                    thisVar.value = instance;
                    thisVar.is_constant = true;
                    methodClosure->set("$", thisVar);

                    // Wrapper that substitutes methodClosure for callEnv
                    auto wrapper_impl = [nativeFn, methodClosure](
                                            const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                        return nativeFn->native_impl(args, methodClosure, token);
                    };

                    auto wrappedFn = std::make_shared<FunctionValue>(
                        nativeFn->name,
                        wrapper_impl,
                        methodClosure,
                        nativeFn->token);

                    PropertyDescriptor pd;
                    pd.value = wrappedFn;
                    pd.is_private = m->is_private;
                    pd.is_locked = m->is_locked;
                    pd.is_readonly = m->is_getter;
                    pd.token = m->token;
                    instance->properties[m->name] = std::move(pd);
                    continue;
                }
            }

            // AST method - create FunctionDeclarationNode
            auto persisted = std::make_shared<FunctionDeclarationNode>();
            persisted->name = m->name;
            persisted->token = m->token;
            persisted->is_async = m->is_async;

            // Clone parameters
            persisted->parameters.reserve(m->params.size());
            for (const auto& pp : m->params) {
                if (pp)
                    persisted->parameters.push_back(pp->clone());
                else
                    persisted->parameters.push_back(nullptr);
            }

            // Clone body
            persisted->body.reserve(m->body.size());
            for (const auto& s : m->body) {
                persisted->body.push_back(s ? s->clone() : nullptr);
            }

            // Create closure with $ bound to instance
            EnvPtr methodClosure = std::make_shared<Environment>(classEnv);
            Environment::Variable thisVar;
            thisVar.value = instance;
            thisVar.is_constant = true;
            methodClosure->set("$", thisVar);

            auto fn = std::make_shared<FunctionValue>(
                persisted->name, persisted->parameters, persisted, methodClosure, persisted->token);

            PropertyDescriptor pd;
            pd.value = fn;
            pd.is_private = m->is_private;
            pd.is_locked = m->is_locked;
            pd.is_readonly = m->is_getter;
            pd.token = m->token;
            instance->properties[m->name] = std::move(pd);
        }
    }

    // --- Call constructor if present ---
    if (cls->body) {
        // Convert argv to vector
        std::vector<Value> ctorArgs;
        ctorArgs.reserve(argc);
        for (size_t i = 0; i < argc; i++) {
            ctorArgs.push_back(unwrap_value(argv[i]));
        }

        // Check for native constructor first
        auto native_ctor_it = cls->static_table->properties.find("__constructor__");
        if (native_ctor_it != cls->static_table->properties.end()) {
            if (std::holds_alternative<FunctionPtr>(native_ctor_it->second.value)) {
                FunctionPtr constructorFn = std::get<FunctionPtr>(native_ctor_it->second.value);

                try {
                    // Set class context for super() calls
                    ClassPtr saved_ctx = env->evaluator->get_current_class_context_public();
                    env->evaluator->set_current_class_context_public(cls);

                    env->evaluator->call_function_with_receiver_public(
                        constructorFn, instance, ctorArgs, env->env_ptr, Token());

                    env->evaluator->set_current_class_context_public(saved_ctx);
                } catch (const SwaziError& e) {
                    set_error(env, "instantiationError", e.what());
                    return SWAZI_GENERIC_FAILURE;
                } catch (const std::exception& e) {
                    set_error(env, "Error", e.what());
                    return SWAZI_GENERIC_FAILURE;
                }
            }
        } else {
            // Look for AST constructor
            ClassMethodNode* ctorNode = nullptr;
            for (auto& m : cls->body->methods) {
                if (m && m->is_constructor) {
                    ctorNode = m.get();
                    break;
                }
            }

            if (ctorNode) {
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = ctorNode->name;
                persisted->token = ctorNode->token;
                persisted->is_async = ctorNode->is_async;

                // Clone parameters
                persisted->parameters.reserve(ctorNode->params.size());
                for (const auto& pp : ctorNode->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // Clone body
                persisted->body.reserve(ctorNode->body.size());
                for (const auto& stmt : ctorNode->body) {
                    persisted->body.push_back(stmt ? stmt->clone() : nullptr);
                }

                // Create constructor function
                EnvPtr ctorClosure = cls->defining_env ? cls->defining_env : env->env_ptr;
                auto constructorFn = std::make_shared<FunctionValue>(
                    persisted->name, persisted->parameters, persisted, ctorClosure, persisted->token);

                try {
                    // Set class context for super() calls
                    ClassPtr saved_ctx = env->evaluator->get_current_class_context_public();
                    env->evaluator->set_current_class_context_public(cls);

                    env->evaluator->call_function_with_receiver_public(
                        constructorFn, instance, ctorArgs, env->env_ptr, persisted->token);

                    env->evaluator->set_current_class_context_public(saved_ctx);
                } catch (const SwaziError& e) {
                    set_error(env, "instantiationError", e.what());
                    return SWAZI_GENERIC_FAILURE;
                } catch (const std::exception& e) {
                    set_error(env, "Error", e.what());
                    return SWAZI_GENERIC_FAILURE;
                }
            }
        }
    }

    // Return the fully constructed instance
    *result = wrap_value(Value{instance});
    return SWAZI_OK;
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
    err_obj->properties["__error__"] = PropertyDescriptor{Value{true}, true, false, false, Token()};
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

static swazi_status api_get_last_error(swazi_env env, swazi_error_info* info) {
    if (!env || !info) return SWAZI_INVALID_ARG;

    info->code = env->last_error_code.c_str();
    info->message = env->last_error_message.c_str();

    return SWAZI_OK;
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

static swazi_status api_queue_macrotask(swazi_env env, swazi_callback callback,
    void* user_data) {
    if (!env || !callback) return SWAZI_INVALID_ARG;

    // Create a wrapper that will be executed on the main loop
    auto wrapper = [env, callback, user_data]() {
        swazi_callback_info_s cbinfo;
        cbinfo.args = {};  // No args for this callback
        cbinfo.user_data = user_data;
        cbinfo.this_object = nullptr;
        cbinfo.new_target = std::monostate{};

        swazi_value result_handle = callback(env, &cbinfo);

        if (env->exception_pending) {
            env->exception_pending = false;
            std::cerr << "Exception in queued callback: "
                      << env->last_error_message << std::endl;
        }

        if (result_handle) {
            delete result_handle;
        }
    };

    // Use the scheduler bridge to queue this on the main loop
    if (env->evaluator && env->evaluator->scheduler()) {
        env->evaluator->scheduler()->enqueue_macrotask(wrapper);
    } else {
        // Fallback: use global bridge
        CallbackPayload* payload = new CallbackPayload(nullptr, {});
        // Store wrapper in a static map keyed by payload pointer
        // (implementation detail - needs global map)
        enqueue_callback_global(static_cast<void*>(payload));
    }

    return SWAZI_OK;
}

static swazi_status api_queue_microtask(swazi_env env, swazi_callback callback,
    void* user_data) {
    if (!env || !callback) return SWAZI_INVALID_ARG;

    auto wrapper = [env, callback, user_data]() {
        swazi_callback_info_s cbinfo;
        cbinfo.args = {};
        cbinfo.user_data = user_data;
        cbinfo.this_object = nullptr;
        cbinfo.new_target = std::monostate{};

        swazi_value result_handle = callback(env, &cbinfo);

        if (env->exception_pending) {
            env->exception_pending = false;
            std::cerr << "Exception in microtask: "
                      << env->last_error_message << std::endl;
        }

        if (result_handle) {
            delete result_handle;
        }
    };

    if (env->evaluator && env->evaluator->scheduler()) {
        env->evaluator->scheduler()->enqueue_microtask(wrapper);
    } else {
        // Fallback
        CallbackPayload* payload = new CallbackPayload(nullptr, {});
        enqueue_microtask_global(static_cast<void*>(payload));
    }

    return SWAZI_OK;
}

static swazi_status api_resolve_deferred_async(swazi_env env,
    swazi_deferred deferred, swazi_value resolution) {
    if (!env || !deferred || !resolution) return SWAZI_INVALID_ARG;

    // Capture the promise and resolution value by VALUE (copies)
    PromisePtr prom = deferred->promise;
    Value res = unwrap_value(resolution);

    // Queue the resolution to happen on the main loop
    if (env->evaluator && env->evaluator->scheduler()) {
        env->evaluator->scheduler()->enqueue_macrotask([env, prom, res]() {
            env->evaluator->fulfill_promise(prom, res);
        });
    } else {
        // Fallback: immediate (unsafe but best-effort)
        env->evaluator->fulfill_promise(prom, res);
    }

    // Clean up deferred (addon no longer owns it)
    delete deferred;
    return SWAZI_OK;
}

static swazi_status api_reject_deferred_async(swazi_env env,
    swazi_deferred deferred, swazi_value rejection) {
    if (!env || !deferred || !rejection) return SWAZI_INVALID_ARG;

    PromisePtr prom = deferred->promise;
    Value rej = unwrap_value(rejection);

    if (env->evaluator && env->evaluator->scheduler()) {
        env->evaluator->scheduler()->enqueue_macrotask([env, prom, rej]() {
            env->evaluator->reject_promise(prom, rej);
        });
    } else {
        env->evaluator->reject_promise(prom, rej);
    }

    delete deferred;
    return SWAZI_OK;
}

static void api_thread_will_start(swazi_env env) {
    (void)env;
    addon_thread_started();
}

static void api_thread_did_finish(swazi_env env) {
    (void)env;
    addon_thread_finished();
}

static void* api_get_event_loop(swazi_env env) {
    if (!env || !env->evaluator) return nullptr;
    return env->evaluator->scheduler()->get_uv_loop();
}

// Better: Wrap uv_queue_work to hide libuv from addons
struct BackgroundWork {
    void (*work_cb)(void*);
    void (*after_cb)(void*);
    void* user_data;
};

static void uv_work_wrapper(uv_work_t* req) {
    BackgroundWork* bw = (BackgroundWork*)req->data;
    if (bw && bw->work_cb) {
        bw->work_cb(bw->user_data);
    }
}

static void uv_after_wrapper(uv_work_t* req, int status) {
    BackgroundWork* bw = (BackgroundWork*)req->data;
    if (bw) {
        if (bw->after_cb) {
            bw->after_cb(bw->user_data);
        }
        delete bw;
    }
    delete req;
}

static swazi_status api_queue_background_work(
    swazi_env env,
    void (*work_cb)(void*),
    void (*after_cb)(void*),
    void* user_data) {
    if (!env || !work_cb) return SWAZI_INVALID_ARG;

    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) return SWAZI_GENERIC_FAILURE;

    auto* req = new uv_work_t;
    auto* bw = new BackgroundWork{work_cb, after_cb, user_data};
    req->data = bw;

    int r = uv_queue_work(loop, req, uv_work_wrapper, uv_after_wrapper);
    if (r != 0) {
        delete bw;
        delete req;
        return SWAZI_GENERIC_FAILURE;
    }

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
        Value{true}, true, false, false, Token()};  // mark it private and locked so userland can not access or override

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
    dt->epochNanoseconds = static_cast<uint64_t>(time * 1000000.0);
    dt->isUTC = true;
    dt->tzOffsetSeconds = 0;
    dt->precision = DateTimePrecision::MILLISECOND;
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
    *result = static_cast<double>(dt->epochNanoseconds) / 1000000.0;

    return SWAZI_OK;
}

// Field getters
static swazi_status api_datetime_get_year(swazi_env env, swazi_value value,
    int32_t* year) {
    if (!env || !value || !year) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    *year = std::get<DateTimePtr>(v)->year;
    return SWAZI_OK;
}

static swazi_status api_datetime_get_month(swazi_env env, swazi_value value,
    int32_t* month) {
    if (!env || !value || !month) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    *month = std::get<DateTimePtr>(v)->month;
    return SWAZI_OK;
}

static swazi_status api_datetime_get_day(swazi_env env, swazi_value value,
    int32_t* day) {
    if (!env || !value || !day) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    *day = std::get<DateTimePtr>(v)->day;
    return SWAZI_OK;
}

static swazi_status api_datetime_get_hour(swazi_env env, swazi_value value,
    int32_t* hour) {
    if (!env || !value || !hour) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    *hour = std::get<DateTimePtr>(v)->hour;
    return SWAZI_OK;
}

static swazi_status api_datetime_get_minute(swazi_env env, swazi_value value,
    int32_t* minute) {
    if (!env || !value || !minute) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    *minute = std::get<DateTimePtr>(v)->minute;
    return SWAZI_OK;
}

static swazi_status api_datetime_get_second(swazi_env env, swazi_value value,
    int32_t* second) {
    if (!env || !value || !second) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }
    *second = std::get<DateTimePtr>(v)->second;
    return SWAZI_OK;
}

// Field setters
static swazi_status api_datetime_set_year(swazi_env env, swazi_value value,
    int32_t year, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    auto dt = std::make_shared<DateTimeValue>(*std::get<DateTimePtr>(v));
    dt->year = year;
    dt->recompute_epoch_from_fields();
    dt->update_literal_text();

    *result = wrap_value(Value{dt});
    return SWAZI_OK;
}

static swazi_status api_datetime_set_month(swazi_env env, swazi_value value,
    int32_t month, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    if (month < 1 || month > 12) {
        set_error(env, "RangeError", "Month must be 1-12");
        return SWAZI_GENERIC_FAILURE;
    }

    auto dt = std::make_shared<DateTimeValue>(*std::get<DateTimePtr>(v));
    dt->month = month;
    dt->recompute_epoch_from_fields();
    dt->update_literal_text();

    *result = wrap_value(Value{dt});
    return SWAZI_OK;
}

static swazi_status api_datetime_set_day(swazi_env env, swazi_value value,
    int32_t day, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    if (day < 1 || day > 31) {
        set_error(env, "RangeError", "Day must be 1-31");
        return SWAZI_GENERIC_FAILURE;
    }

    auto dt = std::make_shared<DateTimeValue>(*std::get<DateTimePtr>(v));
    dt->day = day;
    dt->recompute_epoch_from_fields();
    dt->update_literal_text();

    *result = wrap_value(Value{dt});
    return SWAZI_OK;
}

// Arithmetic operations
static swazi_status api_datetime_add_days(swazi_env env, swazi_value value,
    int days, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    DateTimePtr dt = std::get<DateTimePtr>(v);
    DateTimePtr newDt = dt->addDays(days);

    *result = wrap_value(Value{newDt});
    return SWAZI_OK;
}

static swazi_status api_datetime_add_months(swazi_env env, swazi_value value,
    int months, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    DateTimePtr dt = std::get<DateTimePtr>(v);
    DateTimePtr newDt = dt->addMonths(months);

    *result = wrap_value(Value{newDt});
    return SWAZI_OK;
}

static swazi_status api_datetime_add_years(swazi_env env, swazi_value value,
    int years, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    DateTimePtr dt = std::get<DateTimePtr>(v);
    DateTimePtr newDt = dt->addYears(years);

    *result = wrap_value(Value{newDt});
    return SWAZI_OK;
}

static swazi_status api_datetime_add_hours(swazi_env env, swazi_value value,
    double hours, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    DateTimePtr dt = std::get<DateTimePtr>(v);
    DateTimePtr newDt = dt->addHours(hours);

    *result = wrap_value(Value{newDt});
    return SWAZI_OK;
}

static swazi_status api_datetime_add_seconds(swazi_env env, swazi_value value,
    double seconds, swazi_value* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    DateTimePtr dt = std::get<DateTimePtr>(v);
    DateTimePtr newDt = dt->addSeconds(seconds);

    *result = wrap_value(Value{newDt});
    return SWAZI_OK;
}

// Formatting
static swazi_status api_datetime_format(swazi_env env, swazi_value value,
    const char* format, char* buf,
    size_t bufsize, size_t* result) {
    if (!env || !value || !format) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    DateTimePtr dt = std::get<DateTimePtr>(v);
    std::string formatted = dt->format(format);

    if (result) *result = formatted.length();

    if (buf && bufsize > 0) {
        size_t copy_len = std::min(formatted.length(), bufsize - 1);
        std::memcpy(buf, formatted.c_str(), copy_len);
        buf[copy_len] = '\0';
    }

    return SWAZI_OK;
}

// Timezone
static swazi_status api_datetime_set_timezone(swazi_env env, swazi_value value,
    const char* tz, swazi_value* result) {
    if (!env || !value || !tz || !result) return SWAZI_INVALID_ARG;
    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<DateTimePtr>(v)) {
        set_error(env, "TypeError", "Value is not a datetime");
        return SWAZI_DATE_EXPECTED;
    }

    try {
        DateTimePtr dt = std::get<DateTimePtr>(v);
        DateTimePtr newDt = dt->setZone(tz);
        *result = wrap_value(Value{newDt});
        return SWAZI_OK;
    } catch (const std::exception& e) {
        set_error(env, "ValueError", e.what());
        return SWAZI_GENERIC_FAILURE;
    }
}

// ============================================================================
// API Implementation - Range Operations
// ============================================================================

static swazi_status api_create_range(swazi_env env, int32_t start, int32_t end,
    size_t step, bool inclusive,
    swazi_value* result) {
    if (!env || !result) return SWAZI_INVALID_ARG;

    if (step == 0) {
        set_error(env, "ValueError", "Range step cannot be zero");
        return SWAZI_GENERIC_FAILURE;
    }

    auto range = std::make_shared<RangeValue>(start, end, step, inclusive);
    *result = wrap_value(Value{range});
    return SWAZI_OK;
}

static swazi_status api_range_has_next(swazi_env env, swazi_value value,
    bool* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;

    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<RangePtr>(v)) {
        set_error(env, "TypeError", "Value is not a range");
        return SWAZI_GENERIC_FAILURE;
    }

    RangePtr range = std::get<RangePtr>(v);
    *result = range->hasNext();
    return SWAZI_OK;
}

static swazi_status api_range_next(swazi_env env, swazi_value value,
    int32_t* result) {
    if (!env || !value || !result) return SWAZI_INVALID_ARG;

    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<RangePtr>(v)) {
        set_error(env, "TypeError", "Value is not a range");
        return SWAZI_GENERIC_FAILURE;
    }

    RangePtr range = std::get<RangePtr>(v);
    if (!range->hasNext()) {
        set_error(env, "RangeError", "Range exhausted");
        return SWAZI_GENERIC_FAILURE;
    }

    *result = range->next();
    return SWAZI_OK;
}

static swazi_status api_range_reset(swazi_env env, swazi_value value) {
    if (!env || !value) return SWAZI_INVALID_ARG;

    const Value& v = unwrap_value(value);
    if (!std::holds_alternative<RangePtr>(v)) {
        set_error(env, "TypeError", "Value is not a range");
        return SWAZI_GENERIC_FAILURE;
    }

    RangePtr range = std::get<RangePtr>(v);
    range->cur = range->start;
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
// API Implementation - Regex Operations
// ============================================================================

static swazi_status api_create_regex(swazi_env env, const char* pattern,
    const char* flags, swazi_value* result) {
    if (!env || !pattern || !result) return SWAZI_INVALID_ARG;

    try {
        std::string pat(pattern);
        std::string flg = flags ? std::string(flags) : "";

        auto regex = std::make_shared<RegexValue>(pat, flg);
        // Compile to validate pattern
        regex->getCompiled();

        *result = wrap_value(Value{regex});
        return SWAZI_OK;
    } catch (const std::exception& e) {
        set_error(env, "SyntaxError", e.what());
        return SWAZI_GENERIC_FAILURE;
    }
}

static swazi_status api_regex_test(swazi_env env, swazi_value regex,
    swazi_value str, bool* result) {
    if (!env || !regex || !str || !result) return SWAZI_INVALID_ARG;

    const Value& re_val = unwrap_value(regex);
    if (!std::holds_alternative<RegexPtr>(re_val)) {
        set_error(env, "TypeError", "First argument is not a regex");
        return SWAZI_GENERIC_FAILURE;
    }
    const Value& str_val = unwrap_value(str);
    if (!std::holds_alternative<std::string>(str_val)) {
        set_error(env, "TypeError", "Second argument must be a string");
        return SWAZI_STRING_EXPECTED;
    }

    try {
        RegexPtr re = std::get<RegexPtr>(re_val);
        const std::string& text = std::get<std::string>(str_val);

        re2::RE2& compiled = re->getCompiled();
        *result = re2::RE2::PartialMatch(text, compiled);

        return SWAZI_OK;
    } catch (const std::exception& e) {
        set_error(env, "Error", e.what());
        return SWAZI_GENERIC_FAILURE;
    }
}

static swazi_status api_regex_exec(swazi_env env, swazi_value regex,
    swazi_value str, swazi_value* result) {
    if (!env || !regex || !str || !result) return SWAZI_INVALID_ARG;

    const Value& re_val = unwrap_value(regex);
    if (!std::holds_alternative<RegexPtr>(re_val)) {
        set_error(env, "TypeError", "First argument is not a regex");
        return SWAZI_GENERIC_FAILURE;
    }

    const Value& str_val = unwrap_value(str);
    if (!std::holds_alternative<std::string>(str_val)) {
        set_error(env, "TypeError", "Second argument must be a string");
        return SWAZI_STRING_EXPECTED;
    }

    try {
        RegexPtr re = std::get<RegexPtr>(re_val);
        const std::string& text = std::get<std::string>(str_val);

        re2::RE2& compiled = re->getCompiled();
        int num_groups = re->getNumGroups();

        std::vector<re2::RE2::Arg> args(num_groups + 1);
        std::vector<std::string> captures(num_groups + 1);
        std::vector<const re2::RE2::Arg*> arg_ptrs(num_groups + 1);

        for (int i = 0; i <= num_groups; ++i) {
            args[i] = &captures[i];
            arg_ptrs[i] = &args[i];
        }

        bool matched = re2::RE2::FullMatchN(text, compiled, arg_ptrs.data(), num_groups + 1);

        if (!matched) {
            *result = wrap_value(std::monostate{});
            return SWAZI_OK;
        }

        // Create result array
        auto arr = std::make_shared<ArrayValue>();
        arr->elements.reserve(captures.size());
        for (const auto& cap : captures) {
            arr->elements.push_back(Value{cap});
        }

        *result = wrap_value(Value{arr});
        return SWAZI_OK;
    } catch (const std::exception& e) {
        set_error(env, "Error", e.what());
        return SWAZI_GENERIC_FAILURE;
    }
}

static swazi_status api_regex_get_last_index(swazi_env env, swazi_value regex,
    size_t* result) {
    if (!env || !regex || !result) return SWAZI_INVALID_ARG;

    const Value& re_val = unwrap_value(regex);
    if (!std::holds_alternative<RegexPtr>(re_val)) {
        set_error(env, "TypeError", "Value is not a regex");
        return SWAZI_GENERIC_FAILURE;
    }

    RegexPtr re = std::get<RegexPtr>(re_val);
    *result = re->lastIndex;
    return SWAZI_OK;
}

static swazi_status api_regex_set_last_index(swazi_env env, swazi_value regex,
    size_t index) {
    if (!env || !regex) return SWAZI_INVALID_ARG;

    const Value& re_val = unwrap_value(regex);
    if (!std::holds_alternative<RegexPtr>(re_val)) {
        set_error(env, "TypeError", "Value is not a regex");
        return SWAZI_GENERIC_FAILURE;
    }

    RegexPtr re = std::get<RegexPtr>(re_val);
    re->lastIndex = index;
    return SWAZI_OK;
}

// ============================================================================
// API Implementation - Enhanced Class Operations
// ============================================================================

static swazi_status api_create_class(swazi_env env, const char* name,
    swazi_value parent_class, swazi_value* result) {
    if (!env || !name || !result) return SWAZI_INVALID_ARG;

    auto cls = std::make_shared<ClassValue>();
    cls->name = name;
    cls->token = Token();
    cls->defining_env = env->env_ptr;
    cls->static_table = std::make_shared<ObjectValue>();

    // ✅ CREATE A REAL ClassBodyNode (not leave it null!)
    cls->body = std::make_unique<ClassBodyNode>();

    // Set parent class if provided
    if (parent_class) {
        Value parent_val = unwrap_value(parent_class);
        if (std::holds_alternative<ClassPtr>(parent_val)) {
            cls->super = std::get<ClassPtr>(parent_val);
        } else {
            set_error(env, "TypeError", "Parent must be a class value");
            return SWAZI_OBJECT_EXPECTED;
        }
    }

    *result = wrap_value(Value{cls});
    return SWAZI_OK;
}

static swazi_status api_class_define_method(swazi_env env, swazi_value class_val,
    const char* method_name, swazi_callback callback,
    void* user_data, uint32_t flags) {
    if (!env || !class_val || !method_name || !callback) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    if (!cls || !cls->body) {
        set_error(env, "TypeError", "Invalid class");
        return SWAZI_GENERIC_FAILURE;
    }

    std::string name_str(method_name);
    bool is_static = (flags & SWAZI_CLASS_MEMBER_STATIC) != 0;
    bool is_private = (flags & SWAZI_CLASS_MEMBER_PRIVATE) != 0;
    bool is_locked = (flags & SWAZI_CLASS_MEMBER_LOCKED) != 0;
    bool is_getter = (flags & SWAZI_CLASS_MEMBER_GETTER) != 0;
    bool is_constructor = (flags & SWAZI_CLASS_MEMBER_CONSTRUCTOR) != 0;
    bool is_destructor = (flags & SWAZI_CLASS_MEMBER_DESTRUCTOR) != 0;

    // ✅ Create native wrapper that handles $ binding
    auto native_impl = [callback, user_data, env](
                           const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        swazi_callback_info_s cbinfo;
        cbinfo.args = args;
        cbinfo.user_data = user_data;
        cbinfo.this_object = nullptr;

        // Extract receiver from environment's "$" binding
        if (callEnv && callEnv->has("$")) {
            Value dollar = callEnv->get("$").value;
            if (std::holds_alternative<ObjectPtr>(dollar)) {
                cbinfo.this_object = std::get<ObjectPtr>(dollar);
            }
        }

        cbinfo.new_target = std::monostate{};

        swazi_value result_handle = callback(env, &cbinfo);

        if (env->exception_pending) {
            env->exception_pending = false;
            throw SwaziError(env->last_error_code, env->last_error_message, token.loc);
        }

        if (!result_handle) return std::monostate{};

        Value result = unwrap_value(result_handle);
        delete result_handle;
        return result;
    };

    auto fn = std::make_shared<FunctionValue>(name_str, native_impl, env->env_ptr, Token());

    // ✅ For STATIC methods: add directly to static_table
    if (is_static) {
        PropertyDescriptor pd;
        pd.value = fn;
        pd.is_private = is_private;
        pd.is_locked = is_locked;
        pd.is_readonly = is_getter;
        pd.token = Token();
        cls->static_table->properties[name_str] = std::move(pd);
        return SWAZI_OK;
    }

    // ✅ For CONSTRUCTORS: store in special location for instantiation code
    if (is_constructor) {
        PropertyDescriptor pd;
        pd.value = fn;
        pd.is_private = is_private;
        pd.token = Token();
        cls->static_table->properties["__constructor__"] = std::move(pd);

        // Also add synthetic ClassMethodNode so userland can find it
        auto method_node = std::make_unique<ClassMethodNode>();
        method_node->name = name_str;
        method_node->token = Token();
        method_node->is_static = false;
        method_node->is_constructor = true;
        method_node->is_private = is_private;
        method_node->is_locked = is_locked;

        // Mark as native-backed
        std::string marker_name = "__native_method_" + name_str;
        PropertyDescriptor marker;
        marker.value = fn;
        marker.is_private = true;
        marker.token = Token();
        cls->static_table->properties[marker_name] = marker;

        cls->body->methods.push_back(std::move(method_node));
        return SWAZI_OK;
    }

    // ✅ For DESTRUCTORS: similar to constructor
    if (is_destructor) {
        PropertyDescriptor pd;
        pd.value = fn;
        pd.is_private = is_private;
        pd.token = Token();
        cls->static_table->properties["__destructor__"] = std::move(pd);

        auto method_node = std::make_unique<ClassMethodNode>();
        method_node->name = name_str;
        method_node->token = Token();
        method_node->is_static = false;
        method_node->is_destructor = true;
        method_node->is_private = is_private;

        std::string marker_name = "__native_method_" + name_str;
        PropertyDescriptor marker;
        marker.value = fn;
        marker.is_private = true;
        marker.token = Token();
        cls->static_table->properties[marker_name] = marker;

        cls->body->methods.push_back(std::move(method_node));
        return SWAZI_OK;
    }

    // ✅ For INSTANCE methods: create synthetic ClassMethodNode
    auto method_node = std::make_unique<ClassMethodNode>();
    method_node->name = name_str;
    method_node->token = Token();
    method_node->is_static = false;
    method_node->is_private = is_private;
    method_node->is_locked = is_locked;
    method_node->is_getter = is_getter;
    method_node->is_constructor = false;
    method_node->is_destructor = false;
    method_node->is_async = false;

    // Store native function in marker
    std::string marker_name = "__native_method_" + name_str;
    PropertyDescriptor marker;
    marker.value = fn;
    marker.is_private = true;
    marker.token = Token();
    cls->static_table->properties[marker_name] = marker;

    cls->body->methods.push_back(std::move(method_node));

    return SWAZI_OK;
}

static swazi_status api_class_define_property(swazi_env env, swazi_value class_val,
    const char* property_name, swazi_value initial_value,
    uint32_t flags) {
    if (!env || !class_val || !property_name) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    if (!cls || !cls->body) {
        set_error(env, "TypeError", "Invalid class");
        return SWAZI_GENERIC_FAILURE;
    }

    std::string prop_name(property_name);
    Value init_val = initial_value ? unwrap_value(initial_value) : std::monostate{};

    bool is_static = (flags & SWAZI_CLASS_MEMBER_STATIC) != 0;
    bool is_private = (flags & SWAZI_CLASS_MEMBER_PRIVATE) != 0;
    bool is_locked = (flags & SWAZI_CLASS_MEMBER_LOCKED) != 0;

    // ✅ For STATIC properties: add to static_table
    if (is_static) {
        PropertyDescriptor pd;
        pd.value = init_val;
        pd.is_private = is_private;
        pd.is_locked = is_locked;
        pd.is_readonly = false;
        pd.token = Token();
        cls->static_table->properties[prop_name] = std::move(pd);
        return SWAZI_OK;
    }

    // ✅ For INSTANCE properties: create synthetic ClassPropertyNode
    auto prop_node = std::make_unique<ClassPropertyNode>();
    prop_node->name = prop_name;
    prop_node->token = Token();
    prop_node->is_static = false;
    prop_node->is_private = is_private;
    prop_node->is_locked = is_locked;

    // Store initial value in a marker so evaluator can retrieve it
    std::string marker_name = "__native_property_" + prop_name;
    PropertyDescriptor marker;
    marker.value = init_val;
    marker.is_private = true;
    marker.token = Token();
    cls->static_table->properties[marker_name] = marker;

    // Add to body
    cls->body->properties.push_back(std::move(prop_node));

    return SWAZI_OK;
}

static swazi_status api_class_modify_method(swazi_env env, swazi_value class_val,
    const char* method_name, swazi_callback new_callback,
    void* new_user_data, uint32_t flags) {
    if (!env || !class_val || !method_name) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    std::string name_str(method_name);
    PropertyDescriptor* pd = nullptr;

    // ✅ First check for instance method marker
    std::string marker_name = "__native_method_" + name_str;
    auto marker_it = cls->static_table->properties.find(marker_name);
    if (marker_it != cls->static_table->properties.end()) {
        pd = &marker_it->second;
    }

    // ✅ Check static methods (no markers, direct storage)
    if (!pd) {
        auto static_it = cls->static_table->properties.find(name_str);
        if (static_it != cls->static_table->properties.end() &&
            name_str != "__addon_class__" &&
            name_str != "__instance_defaults__" &&
            name_str != "__instance_methods__" &&
            !name_str.starts_with("__native_")) {  // Exclude all markers
            pd = &static_it->second;
        }
    }

    // ✅ Check constructor (special property)
    if (!pd) {
        auto ctor_it = cls->static_table->properties.find("__constructor__");
        if (ctor_it != cls->static_table->properties.end() &&
            (name_str == cls->name || name_str == "__constructor__")) {
            pd = &ctor_it->second;
        }
    }

    // ✅ Check destructor (special property)
    if (!pd) {
        auto dtor_it = cls->static_table->properties.find("__destructor__");
        if (dtor_it != cls->static_table->properties.end() &&
            (name_str == ("~" + cls->name) || name_str == "__destructor__")) {
            pd = &dtor_it->second;
        }
    }

    if (!pd) {
        set_error(env, "ReferenceError", "Method not found");
        return SWAZI_GENERIC_FAILURE;
    }

    // Update callback if provided
    if (new_callback) {
        auto native_impl = [new_callback, new_user_data, env](
                               const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
            swazi_callback_info_s cbinfo;
            cbinfo.args = args;
            cbinfo.user_data = new_user_data;
            cbinfo.this_object = nullptr;

            if (callEnv && callEnv->has("$")) {
                Value dollar = callEnv->get("$").value;
                if (std::holds_alternative<ObjectPtr>(dollar)) {
                    cbinfo.this_object = std::get<ObjectPtr>(dollar);
                }
            }

            cbinfo.new_target = std::monostate{};

            swazi_value result_handle = new_callback(env, &cbinfo);

            if (env->exception_pending) {
                env->exception_pending = false;
                throw SwaziError(env->last_error_code, env->last_error_message, token.loc);
            }

            if (!result_handle) return std::monostate{};

            Value result = unwrap_value(result_handle);
            delete result_handle;
            return result;
        };

        pd->value = std::make_shared<FunctionValue>(name_str, native_impl, env->env_ptr, Token());
    }

    // Update flags if provided
    if (flags != 0) {
        pd->is_private = (flags & SWAZI_CLASS_MEMBER_PRIVATE) != 0;
        pd->is_locked = (flags & SWAZI_CLASS_MEMBER_LOCKED) != 0;
        pd->is_readonly = (flags & SWAZI_CLASS_MEMBER_READONLY) != 0 ||
            (flags & SWAZI_CLASS_MEMBER_GETTER) != 0;
    }

    return SWAZI_OK;
}

static swazi_status api_class_modify_property(swazi_env env, swazi_value class_val,
    const char* property_name, swazi_value new_value,
    uint32_t flags) {
    if (!env || !class_val || !property_name) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    std::string prop_name(property_name);
    PropertyDescriptor* pd = nullptr;

    // ✅ First check for instance property marker
    std::string marker_name = "__native_property_" + prop_name;
    auto marker_it = cls->static_table->properties.find(marker_name);
    if (marker_it != cls->static_table->properties.end()) {
        pd = &marker_it->second;
    }

    // ✅ Check static properties (no markers, direct storage)
    if (!pd) {
        auto static_it = cls->static_table->properties.find(prop_name);
        if (static_it != cls->static_table->properties.end() &&
            prop_name != "__addon_class__" &&
            prop_name != "__instance_defaults__" &&
            prop_name != "__instance_methods__" &&
            prop_name != "__constructor__" &&
            prop_name != "__destructor__" &&
            !prop_name.starts_with("__native_")) {  // Exclude all markers
            pd = &static_it->second;
        }
    }

    if (!pd) {
        set_error(env, "ReferenceError", "Property not found");
        return SWAZI_GENERIC_FAILURE;
    }

    // Update value if provided
    if (new_value) {
        pd->value = unwrap_value(new_value);
    }

    // Update flags if provided
    if (flags != 0) {
        pd->is_private = (flags & SWAZI_CLASS_MEMBER_PRIVATE) != 0;
        pd->is_locked = (flags & SWAZI_CLASS_MEMBER_LOCKED) != 0;
    }

    return SWAZI_OK;
}

static swazi_status api_class_remove_method(swazi_env env, swazi_value class_val,
    const char* method_name) {
    if (!env || !class_val || !method_name) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    std::string name_str(method_name);

    // ✅ Try instance method marker
    std::string marker_name = "__native_method_" + name_str;
    auto marker_it = cls->static_table->properties.find(marker_name);
    if (marker_it != cls->static_table->properties.end()) {
        cls->static_table->properties.erase(marker_it);

        // Also remove synthetic node from body
        auto& methods = cls->body->methods;
        methods.erase(
            std::remove_if(methods.begin(), methods.end(),
                [&name_str](const auto& m) { return m->name == name_str; }),
            methods.end());

        return SWAZI_OK;
    }

    // ✅ Try static methods (no markers)
    auto static_it = cls->static_table->properties.find(name_str);
    if (static_it != cls->static_table->properties.end() &&
        name_str != "__addon_class__" &&
        name_str != "__instance_defaults__" &&
        name_str != "__instance_methods__" &&
        !name_str.starts_with("__native_")) {
        cls->static_table->properties.erase(static_it);
        return SWAZI_OK;
    }

    // ✅ Try constructor
    if (name_str == cls->name || name_str == "__constructor__") {
        auto ctor_it = cls->static_table->properties.find("__constructor__");
        if (ctor_it != cls->static_table->properties.end()) {
            cls->static_table->properties.erase(ctor_it);

            // Remove synthetic node
            auto& methods = cls->body->methods;
            methods.erase(
                std::remove_if(methods.begin(), methods.end(),
                    [](const auto& m) { return m->is_constructor; }),
                methods.end());

            return SWAZI_OK;
        }
    }

    // ✅ Try destructor
    if (name_str == ("~" + cls->name) || name_str == "__destructor__") {
        auto dtor_it = cls->static_table->properties.find("__destructor__");
        if (dtor_it != cls->static_table->properties.end()) {
            cls->static_table->properties.erase(dtor_it);

            // Remove synthetic node
            auto& methods = cls->body->methods;
            methods.erase(
                std::remove_if(methods.begin(), methods.end(),
                    [](const auto& m) { return m->is_destructor; }),
                methods.end());

            return SWAZI_OK;
        }
    }

    set_error(env, "ReferenceError", "Method not found");
    return SWAZI_GENERIC_FAILURE;
}

static swazi_status api_class_remove_property(swazi_env env, swazi_value class_val,
    const char* property_name) {
    if (!env || !class_val || !property_name) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    std::string prop_name(property_name);

    // Prevent removal of internal properties
    if (prop_name == "__addon_class__" ||
        prop_name == "__instance_defaults__" ||
        prop_name == "__instance_methods__" ||
        prop_name == "__constructor__" ||
        prop_name == "__destructor__" ||
        prop_name.starts_with("__native_")) {
        set_error(env, "TypeError", "Cannot remove internal property");
        return SWAZI_GENERIC_FAILURE;
    }

    // ✅ Try instance property marker
    std::string marker_name = "__native_property_" + prop_name;
    auto marker_it = cls->static_table->properties.find(marker_name);
    if (marker_it != cls->static_table->properties.end()) {
        cls->static_table->properties.erase(marker_it);

        // Also remove synthetic node from body
        auto& properties = cls->body->properties;
        properties.erase(
            std::remove_if(properties.begin(), properties.end(),
                [&prop_name](const auto& p) { return p->name == prop_name; }),
            properties.end());

        return SWAZI_OK;
    }

    // ✅ Try static properties
    auto static_it = cls->static_table->properties.find(prop_name);
    if (static_it != cls->static_table->properties.end()) {
        cls->static_table->properties.erase(static_it);
        return SWAZI_OK;
    }

    set_error(env, "ReferenceError", "Property not found");
    return SWAZI_GENERIC_FAILURE;
}

static swazi_status api_class_has_method(swazi_env env, swazi_value class_val,
    const char* method_name, bool* result) {
    if (!env || !class_val || !method_name || !result) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    std::string name_str(method_name);

    // ✅ Check instance method marker
    std::string marker_name = "__native_method_" + name_str;
    if (cls->static_table->properties.find(marker_name) != cls->static_table->properties.end()) {
        *result = true;
        return SWAZI_OK;
    }

    // ✅ Check static methods
    auto static_it = cls->static_table->properties.find(name_str);
    if (static_it != cls->static_table->properties.end() &&
        name_str != "__addon_class__" &&
        name_str != "__instance_defaults__" &&
        name_str != "__instance_methods__" &&
        !name_str.starts_with("__native_")) {
        *result = true;
        return SWAZI_OK;
    }

    // ✅ Check constructor
    if ((name_str == cls->name || name_str == "__constructor__") &&
        cls->static_table->properties.find("__constructor__") != cls->static_table->properties.end()) {
        *result = true;
        return SWAZI_OK;
    }

    // ✅ Check destructor
    if ((name_str == ("~" + cls->name) || name_str == "__destructor__") &&
        cls->static_table->properties.find("__destructor__") != cls->static_table->properties.end()) {
        *result = true;
        return SWAZI_OK;
    }

    *result = false;
    return SWAZI_OK;
}

static swazi_status api_class_has_property(swazi_env env, swazi_value class_val,
    const char* property_name, bool* result) {
    if (!env || !class_val || !property_name || !result) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);
    std::string prop_name(property_name);

    // ✅ Check instance property marker
    std::string marker_name = "__native_property_" + prop_name;
    if (cls->static_table->properties.find(marker_name) != cls->static_table->properties.end()) {
        *result = true;
        return SWAZI_OK;
    }

    // ✅ Check static properties (exclude internal)
    auto static_it = cls->static_table->properties.find(prop_name);
    if (static_it != cls->static_table->properties.end() &&
        prop_name != "__addon_class__" &&
        prop_name != "__instance_defaults__" &&
        prop_name != "__instance_methods__" &&
        prop_name != "__constructor__" &&
        prop_name != "__destructor__" &&
        !prop_name.starts_with("__native_")) {
        *result = true;
        return SWAZI_OK;
    }

    *result = false;
    return SWAZI_OK;
}

static swazi_status api_class_get_parent(swazi_env env, swazi_value class_val,
    swazi_value* result) {
    if (!env || !class_val || !result) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);

    if (cls->super) {
        *result = wrap_value(Value{cls->super});
    } else {
        *result = wrap_value(std::monostate{});
    }

    return SWAZI_OK;
}

static swazi_status api_class_set_parent(swazi_env env, swazi_value class_val,
    swazi_value parent_class) {
    if (!env || !class_val) return SWAZI_INVALID_ARG;

    Value cls_value = unwrap_value(class_val);
    if (!std::holds_alternative<ClassPtr>(cls_value)) {
        set_error(env, "TypeError", "Expected class value");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(cls_value);

    if (parent_class) {
        Value parent_val = unwrap_value(parent_class);
        if (std::holds_alternative<ClassPtr>(parent_val)) {
            cls->super = std::get<ClassPtr>(parent_val);
        } else {
            set_error(env, "TypeError", "Parent must be a class value");
            return SWAZI_OBJECT_EXPECTED;
        }
    } else {
        cls->super = nullptr;
    }

    return SWAZI_OK;
}

static swazi_status api_class_call_super_constructor(swazi_env env,
    swazi_callback_info info,
    size_t argc,
    const swazi_value* argv) {
    if (!env || !info) return SWAZI_INVALID_ARG;

    // Get the instance (this/$)
    if (!info->this_object) {
        set_error(env, "ReferenceError", "No receiver available for super call");
        return SWAZI_GENERIC_FAILURE;
    }

    ObjectPtr instance = info->this_object;

    // Get the class from instance
    auto class_it = instance->properties.find("__class__");
    if (class_it == instance->properties.end()) {
        set_error(env, "ReferenceError", "Instance has no class link");
        return SWAZI_GENERIC_FAILURE;
    }

    if (!std::holds_alternative<ClassPtr>(class_it->second.value)) {
        set_error(env, "TypeError", "Invalid class link");
        return SWAZI_OBJECT_EXPECTED;
    }

    ClassPtr cls = std::get<ClassPtr>(class_it->second.value);

    // Get parent class
    if (!cls->super) {
        set_error(env, "ReferenceError", "Class has no parent");
        return SWAZI_GENERIC_FAILURE;
    }

    ClassPtr parent = cls->super;

    // Find parent constructor
    auto parent_ctor_it = parent->static_table->properties.find("__constructor__");
    if (parent_ctor_it == parent->static_table->properties.end()) {
        return SWAZI_OK;  // No parent constructor, that's okay
    }

    if (!std::holds_alternative<FunctionPtr>(parent_ctor_it->second.value)) {
        return SWAZI_OK;  // Not a function, skip
    }

    FunctionPtr parent_ctor = std::get<FunctionPtr>(parent_ctor_it->second.value);

    // Convert argv to vector
    std::vector<Value> args;
    for (size_t i = 0; i < argc; i++) {
        args.push_back(unwrap_value(argv[i]));
    }

    // Call parent constructor with instance as receiver
    try {
        env->evaluator->call_function_with_receiver_public(
            parent_ctor, instance, args, env->env_ptr, Token());
    } catch (const SwaziError& e) {
        set_error(env, "Error", e.what());
        return SWAZI_GENERIC_FAILURE;
    }

    return SWAZI_OK;
}

static swazi_status api_get_receiver(swazi_env env, swazi_callback_info info,
    swazi_value* receiver) {
    if (!env || !info || !receiver) return SWAZI_INVALID_ARG;

    if (!info->this_object) {
        *receiver = nullptr;
        return SWAZI_OK;
    }

    *receiver = wrap_value(Value{info->this_object});
    return SWAZI_OK;
}

static swazi_status api_get_instance_class(swazi_env env, swazi_value instance,
    swazi_value* class_val) {
    if (!env || !instance || !class_val) return SWAZI_INVALID_ARG;

    Value inst_val = unwrap_value(instance);
    if (!std::holds_alternative<ObjectPtr>(inst_val)) {
        set_error(env, "TypeError", "Value is not an object");
        return SWAZI_OBJECT_EXPECTED;
    }

    ObjectPtr obj = std::get<ObjectPtr>(inst_val);
    if (!obj) {
        *class_val = wrap_value(std::monostate{});
        return SWAZI_OK;
    }

    auto it = obj->properties.find("__class__");
    if (it == obj->properties.end()) {
        *class_val = wrap_value(std::monostate{});
        return SWAZI_OK;
    }

    if (!std::holds_alternative<ClassPtr>(it->second.value)) {
        *class_val = wrap_value(std::monostate{});
        return SWAZI_OK;
    }

    *class_val = wrap_value(it->second.value);
    return SWAZI_OK;
}

static swazi_status api_instance_of(swazi_env env, swazi_value instance,
    swazi_value class_val, bool* result) {
    if (!env || !instance || !class_val || !result) return SWAZI_INVALID_ARG;

    Value inst_val = unwrap_value(instance);
    Value cls_val = unwrap_value(class_val);

    *result = false;

    if (!std::holds_alternative<ObjectPtr>(inst_val)) {
        return SWAZI_OK;
    }

    if (!std::holds_alternative<ClassPtr>(cls_val)) {
        return SWAZI_OK;
    }

    ObjectPtr obj = std::get<ObjectPtr>(inst_val);
    ClassPtr target_cls = std::get<ClassPtr>(cls_val);

    if (!obj || !target_cls) {
        return SWAZI_OK;
    }

    auto it = obj->properties.find("__class__");
    if (it == obj->properties.end()) {
        return SWAZI_OK;
    }

    if (!std::holds_alternative<ClassPtr>(it->second.value)) {
        return SWAZI_OK;
    }

    ClassPtr inst_cls = std::get<ClassPtr>(it->second.value);

    // Exact match (no inheritance walk for now, can be extended)
    *result = (inst_cls == target_cls);
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
    g_api.create_bound_function = api_create_bound_function;
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
    g_api.get_last_error = api_get_last_error;

    // Buffer operations
    g_api.create_buffer = api_create_buffer;
    g_api.create_external_buffer = api_create_external_buffer;
    g_api.create_buffer_copy = api_create_buffer_copy;
    g_api.get_buffer_info = api_get_buffer_info;

    // Promise operations
    g_api.create_promise = api_create_promise;
    g_api.resolve_deferred = api_resolve_deferred;
    g_api.reject_deferred = api_reject_deferred;
    g_api.queue_macrotask = api_queue_macrotask;
    g_api.queue_microtask = api_queue_microtask;
    g_api.resolve_deferred_async = api_resolve_deferred_async;
    g_api.reject_deferred_async = api_reject_deferred_async;
    g_api.thread_will_start = api_thread_will_start;
    g_api.thread_did_finish = api_thread_did_finish;
    g_api.get_event_loop = api_get_event_loop;
    g_api.queue_background_work = api_queue_background_work;

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
    g_api.datetime_get_year = api_datetime_get_year;
    g_api.datetime_get_month = api_datetime_get_month;
    g_api.datetime_get_day = api_datetime_get_day;
    g_api.datetime_get_hour = api_datetime_get_hour;
    g_api.datetime_get_minute = api_datetime_get_minute;
    g_api.datetime_get_second = api_datetime_get_second;
    g_api.datetime_set_year = api_datetime_set_year;
    g_api.datetime_set_month = api_datetime_set_month;
    g_api.datetime_set_day = api_datetime_set_day;
    g_api.datetime_add_days = api_datetime_add_days;
    g_api.datetime_add_months = api_datetime_add_months;
    g_api.datetime_add_years = api_datetime_add_years;
    g_api.datetime_add_hours = api_datetime_add_hours;
    g_api.datetime_add_seconds = api_datetime_add_seconds;
    g_api.datetime_format = api_datetime_format;
    g_api.datetime_set_timezone = api_datetime_set_timezone;

    // Range operations
    g_api.create_range = api_create_range;
    g_api.range_has_next = api_range_has_next;
    g_api.range_next = api_range_next;
    g_api.range_reset = api_range_reset;

    // Regex operations
    g_api.create_regex = api_create_regex;
    g_api.regex_test = api_regex_test;
    g_api.regex_exec = api_regex_exec;
    g_api.regex_get_last_index = api_regex_get_last_index;
    g_api.regex_set_last_index = api_regex_set_last_index;

    // Class operations
    g_api.create_class = api_create_class;
    g_api.class_define_method = api_class_define_method;
    g_api.class_define_property = api_class_define_property;
    g_api.class_modify_method = api_class_modify_method;
    g_api.class_modify_property = api_class_modify_property;
    g_api.class_remove_method = api_class_remove_method;
    g_api.class_remove_property = api_class_remove_property;
    g_api.class_has_method = api_class_has_method;
    g_api.class_has_property = api_class_has_property;
    g_api.class_get_parent = api_class_get_parent;
    g_api.class_set_parent = api_class_set_parent;
    g_api.class_call_super_constructor = api_class_call_super_constructor;
    g_api.get_receiver = api_get_receiver;
    g_api.get_instance_class = api_get_instance_class;
    g_api.instance_of = api_instance_of;

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
static bool is_valid_language_value(const Value& v) {
    // Check if the value is one of the supported language types
    return std::holds_alternative<std::monostate>(v) ||
        std::holds_alternative<double>(v) ||
        std::holds_alternative<std::string>(v) ||
        std::holds_alternative<bool>(v) ||
        std::holds_alternative<FunctionPtr>(v) ||
        std::holds_alternative<ArrayPtr>(v) ||
        std::holds_alternative<ObjectPtr>(v) ||
        std::holds_alternative<ClassPtr>(v) ||
        std::holds_alternative<BufferPtr>(v) ||
        std::holds_alternative<FilePtr>(v) ||
        std::holds_alternative<RangePtr>(v) ||
        std::holds_alternative<DateTimePtr>(v) ||
        std::holds_alternative<MapStoragePtr>(v) ||
        std::holds_alternative<RegexPtr>(v) ||
        std::holds_alternative<PromisePtr>(v) ||
        std::holds_alternative<GeneratorPtr>(v) ||
        std::holds_alternative<ProxyPtr>(v) ||
        std::holds_alternative<HoleValue>(v);
}
Value load_addon(const std::string& path, Evaluator* evaluator, EnvPtr env) {
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

    dlerror();  // Clear any existing error

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

    static std::unordered_map<std::string, std::unique_ptr<swazi_env_s>> g_addon_envs;
    static std::mutex g_addon_envs_mutex;

    // Create persistent env wrapper
    auto env_wrapper_ptr = std::make_unique<swazi_env_s>();
    env_wrapper_ptr->evaluator = evaluator;
    env_wrapper_ptr->env_ptr = env;

    swazi_env_s* env_wrapper = env_wrapper_ptr.get();

    auto exports = std::make_shared<ObjectValue>();
    SwaziValuePtr exports_handle(wrap_value(Value{exports}));

    try {
        swazi_value result = register_func(env_wrapper, exports_handle.get());

        if (env_wrapper->exception_pending) {
            throw SwaziError(
                env_wrapper->last_error_code.empty() ? "AddonError" : env_wrapper->last_error_code,
                env_wrapper->last_error_message,
                TokenLocation{});
        }

        if (result && result != exports_handle.get()) {
            SwaziValuePtr result_ptr(result);
            Value result_val = unwrap_value(result);

            // ✅ Validate that it's a known language type
            if (!is_valid_language_value(result_val)) {
                throw SwaziError(
                    "AddonError",
                    "Addon returned an invalid value type that the language doesn't support",
                    TokenLocation{});
            }

            return result_val;
        }

    } catch (const SwaziError&) {
        throw;
    } catch (const std::exception& e) {
        throw SwaziError("AddonError", e.what(), TokenLocation{});
    }

    {
        std::lock_guard<std::mutex> lock(g_addon_envs_mutex);
        g_addon_envs[path] = std::move(env_wrapper_ptr);
    }

    return Value{exports};
}