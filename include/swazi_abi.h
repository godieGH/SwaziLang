// swazi_abi.h - Swazi Native Addon API v1.0.0
// Copyright (c) 2025 Swazi Runtime
//
// This header provides a stable C API for writing native addons.
// Users can compile addons without Swazi source code.

#ifndef SWAZI_H
#define SWAZI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ============================================================================
// C++ Headers (must be OUTSIDE extern "C")
// ============================================================================
#ifdef __cplusplus
#include <string>
#include <vector>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Version Information
// ============================================================================

#define SWAZI_API_VERSION_MAJOR 1
#define SWAZI_API_VERSION_MINOR 0
#define SWAZI_API_VERSION_PATCH 0

// ============================================================================
// Opaque Handle Types
// ============================================================================

// All internal structures are hidden from addon authors
typedef struct swazi_env_s* swazi_env;
typedef struct swazi_value_s* swazi_value;
typedef struct swazi_callback_info_s* swazi_callback_info;
typedef struct swazi_deferred_s* swazi_deferred;
typedef struct swazi_ref_s* swazi_ref;
typedef struct swazi_property_descriptor_s* swazi_property_descriptor;

// ============================================================================
// Status Codes
// ============================================================================

typedef enum {
    SWAZI_OK = 0,
    SWAZI_INVALID_ARG,
    SWAZI_OBJECT_EXPECTED,
    SWAZI_STRING_EXPECTED,
    SWAZI_FUNCTION_EXPECTED,
    SWAZI_NUMBER_EXPECTED,
    SWAZI_BOOLEAN_EXPECTED,
    SWAZI_ARRAY_EXPECTED,
    SWAZI_BUFFER_EXPECTED,
    SWAZI_GENERIC_FAILURE,
    SWAZI_PENDING_EXCEPTION,
    SWAZI_CANCELLED,
    SWAZI_ESCAPE_CALLED,
    SWAZI_HANDLE_SCOPE_MISMATCH,
    SWAZI_CALLBACK_SCOPE_MISMATCH,
    SWAZI_QUEUE_FULL,
    SWAZI_CLOSING,
    SWAZI_BIGINT_EXPECTED,
    SWAZI_DATE_EXPECTED,
} swazi_status;

// ============================================================================
// Value Types
// ============================================================================

typedef enum {
    SWAZI_UNDEFINED,
    SWAZI_NULL,
    SWAZI_BOOLEAN,
    SWAZI_NUMBER,
    SWAZI_STRING,
    SWAZI_SYMBOL,
    SWAZI_OBJECT,
    SWAZI_FUNCTION,
    SWAZI_EXTERNAL,
    SWAZI_BIGINT,
    SWAZI_ARRAY,
    SWAZI_CLASS,
    SWAZI_BUFFER,
    SWAZI_PROMISE,
    SWAZI_DATETIME,
    SWAZI_RANGE,
    SWAZI_REGEX,
    SWAZI_COMPLEX_OBJECT,
} swazi_valuetype;

// ============================================================================
// Callback Signatures
// ============================================================================

// Standard function callback
typedef swazi_value (*swazi_callback)(
    swazi_env env,
    swazi_callback_info info);

// Finalizer callback (called when object is garbage collected)
typedef void (*swazi_finalize)(
    swazi_env env,
    void* finalize_data,
    void* finalize_hint);

typedef struct {
    const char* code;
    const char* message;
} swazi_error_info;

// ============================================================================
// Module Registration
// ============================================================================

// Every addon must export this function
typedef swazi_value (*swazi_addon_register_func)(
    swazi_env env,
    swazi_value exports);

// handle auto release handles when gone out of scope
typedef struct swazi_handle_scope_s* swazi_handle_scope;
typedef struct swazi_escapable_handle_scope_s* swazi_escapable_handle_scope;

// Platform-specific export macro
#ifdef _WIN32
#ifdef SWAZI_ADDON_BUILDING
#define SWAZI_EXTERN __declspec(dllexport)
#else
#define SWAZI_EXTERN __declspec(dllimport)
#endif
#define SWAZI_MODULE_EXPORT __declspec(dllexport)
#else
#define SWAZI_EXTERN __attribute__((visibility("default")))
#define SWAZI_MODULE_EXPORT __attribute__((visibility("default")))
#endif

// Module initialization macro
#define SWAZI_MODULE_INIT()         \
    SWAZI_MODULE_EXPORT swazi_value \
    swazi_addon_register(swazi_env env, swazi_value exports)

// Class member flags (can be combined with bitwise OR)
typedef enum {
    SWAZI_CLASS_MEMBER_NONE = 0,
    SWAZI_CLASS_MEMBER_STATIC = 1 << 0,       // Static member (*)
    SWAZI_CLASS_MEMBER_PRIVATE = 1 << 1,      // Private member (@)
    SWAZI_CLASS_MEMBER_LOCKED = 1 << 2,       // Locked member (&)
    SWAZI_CLASS_MEMBER_READONLY = 1 << 3,     // Read-only (for getters)
    SWAZI_CLASS_MEMBER_GETTER = 1 << 4,       // Method is a getter
    SWAZI_CLASS_MEMBER_CONSTRUCTOR = 1 << 5,  // Method is constructor
    SWAZI_CLASS_MEMBER_DESTRUCTOR = 1 << 6,   // Method is destructor
} swazi_class_member_flags;

// ============================================================================
// Core API Structure
// ============================================================================

typedef struct swazi_api_s {
    // ------------------------------------------------------------------------
    // Environment Operations
    // ------------------------------------------------------------------------

    swazi_status (*get_undefined)(swazi_env env, swazi_value* result);
    swazi_status (*get_null)(swazi_env env, swazi_value* result);
    swazi_status (*get_global)(swazi_env env, swazi_value* result);
    swazi_status (*get_boolean)(swazi_env env, bool value, swazi_value* result);

    // ------------------------------------------------------------------------
    // Type Checking
    // ------------------------------------------------------------------------

    swazi_status (*typeof_value)(swazi_env env, swazi_value value,
        swazi_valuetype* result);
    swazi_status (*is_array)(swazi_env env, swazi_value value, bool* result);
    swazi_status (*is_buffer)(swazi_env env, swazi_value value, bool* result);
    swazi_status (*is_error)(swazi_env env, swazi_value value, bool* result);
    swazi_status (*is_promise)(swazi_env env, swazi_value value, bool* result);
    swazi_status (*is_date)(swazi_env env, swazi_value value, bool* result);

    // ============================================================================
    // Utility Functions
    // ============================================================================

    // Check if a value is callable (function or class)
    swazi_status (*is_callable)(swazi_env env, swazi_value value, bool* result);

    // Check if a value is truthy/falsy
    swazi_status (*is_truthy)(swazi_env env, swazi_value value, bool* result);

    // Get the length of a collection (array, string, buffer, object keys, etc.)
    swazi_status (*get_length)(swazi_env env, swazi_value value, size_t* result);

    // Freeze an object (prevent modifications)
    swazi_status (*freeze_object)(swazi_env env, swazi_value object);

    // Check if an object is frozen
    swazi_status (*is_frozen)(swazi_env env, swazi_value object, bool* result);

    // Get object keys as an array
    swazi_status (*get_own_property_names)(swazi_env env, swazi_value object,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // Boolean Operations
    // ------------------------------------------------------------------------

    swazi_status (*get_value_bool)(swazi_env env, swazi_value value,
        bool* result);
    swazi_status (*create_bool)(swazi_env env, bool value,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // Number Operations
    // ------------------------------------------------------------------------

    swazi_status (*get_value_double)(swazi_env env, swazi_value value,
        double* result);
    swazi_status (*get_value_int32)(swazi_env env, swazi_value value,
        int32_t* result);
    swazi_status (*get_value_uint32)(swazi_env env, swazi_value value,
        uint32_t* result);
    swazi_status (*get_value_int64)(swazi_env env, swazi_value value,
        int64_t* result);

    swazi_status (*create_double)(swazi_env env, double value,
        swazi_value* result);
    swazi_status (*create_int32)(swazi_env env, int32_t value,
        swazi_value* result);
    swazi_status (*create_uint32)(swazi_env env, uint32_t value,
        swazi_value* result);
    swazi_status (*create_int64)(swazi_env env, int64_t value,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // String Operations
    // ------------------------------------------------------------------------

    swazi_status (*get_value_string_utf8)(swazi_env env, swazi_value value,
        char* buf, size_t bufsize,
        size_t* result);
    swazi_status (*get_value_string_length)(swazi_env env, swazi_value value,
        size_t* result);
    swazi_status (*create_string_utf8)(swazi_env env, const char* str,
        size_t length, swazi_value* result);
    swazi_status (*create_string_latin1)(swazi_env env, const char* str,
        size_t length, swazi_value* result);

    // ------------------------------------------------------------------------
    // Object Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_object)(swazi_env env, swazi_value* result);
    swazi_status (*get_property)(swazi_env env, swazi_value object,
        swazi_value key, swazi_value* result);
    swazi_status (*get_named_property)(swazi_env env, swazi_value object,
        const char* utf8name,
        swazi_value* result);
    swazi_status (*set_property)(swazi_env env, swazi_value object,
        swazi_value key, swazi_value value);
    swazi_status (*set_named_property)(swazi_env env, swazi_value object,
        const char* utf8name, swazi_value value);
    swazi_status (*has_property)(swazi_env env, swazi_value object,
        swazi_value key, bool* result);
    swazi_status (*has_named_property)(swazi_env env, swazi_value object,
        const char* utf8name, bool* result);
    swazi_status (*delete_property)(swazi_env env, swazi_value object,
        swazi_value key, bool* result);
    swazi_status (*get_property_names)(swazi_env env, swazi_value object,
        swazi_value* result);
    swazi_status (*descriptor_get_value)(swazi_env env, swazi_property_descriptor desc, swazi_value* result);

    // Property Descriptor Operations
    swazi_status (*create_property_descriptor)(
        swazi_env env,
        swazi_value value,
        bool is_private,
        bool is_locked,
        bool is_readonly,
        swazi_property_descriptor* result);

    swazi_status (*delete_property_descriptor)(
        swazi_env env,
        swazi_property_descriptor desc);

    swazi_status (*get_property_descriptor)(
        swazi_env env,
        swazi_value object,
        const char* property_name,
        swazi_property_descriptor* result);

    swazi_status (*define_property_with_descriptor)(
        swazi_env env,
        swazi_value object,
        const char* property_name,
        swazi_property_descriptor desc);

    // ------------------------------------------------------------------------
    // Array Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_array)(swazi_env env, swazi_value* result);
    swazi_status (*create_array_with_length)(swazi_env env, size_t length,
        swazi_value* result);
    swazi_status (*get_array_length)(swazi_env env, swazi_value value,
        uint32_t* result);
    swazi_status (*get_element)(swazi_env env, swazi_value array,
        uint32_t index, swazi_value* result);
    swazi_status (*set_element)(swazi_env env, swazi_value array,
        uint32_t index, swazi_value value);
    swazi_status (*has_element)(swazi_env env, swazi_value array,
        uint32_t index, bool* result);
    swazi_status (*delete_element)(swazi_env env, swazi_value array,
        uint32_t index, bool* result);

    // ------------------------------------------------------------------------
    // Function Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_function)(swazi_env env, const char* utf8name,
        size_t length, swazi_callback cb,
        void* data, swazi_value* result);
    // Create a function bound to a specific receiver (for object methods)
    swazi_status (*create_bound_function)(
        swazi_env env,
        const char* utf8name,
        size_t length,
        swazi_callback cb,
        void* data,
        swazi_value receiver,
        swazi_value* result);
    swazi_status (*call_function)(swazi_env env, swazi_value recv,
        swazi_value func, size_t argc,
        const swazi_value* argv,
        swazi_value* result);
    swazi_status (*new_instance)(swazi_env env, swazi_value constructor,
        size_t argc, const swazi_value* argv,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // Callback Info
    // ------------------------------------------------------------------------

    swazi_status (*get_cb_info)(swazi_env env, swazi_callback_info cbinfo,
        size_t* argc, swazi_value* argv,
        swazi_value* this_arg, void** data);
    swazi_status (*get_new_target)(swazi_env env, swazi_callback_info cbinfo,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // Error Handling
    // ------------------------------------------------------------------------

    swazi_status (*throw_error)(swazi_env env, const char* code,
        const char* msg);
    swazi_status (*throw_type_error)(swazi_env env, const char* code,
        const char* msg);
    swazi_status (*throw_range_error)(swazi_env env, const char* code,
        const char* msg);
    swazi_status (*is_exception_pending)(swazi_env env, bool* result);
    swazi_status (*get_and_clear_last_exception)(swazi_env env,
        swazi_value* result);
    swazi_status (*create_error)(swazi_env env, swazi_value code,
        swazi_value msg, swazi_value* result);
    swazi_status (*create_type_error)(swazi_env env, swazi_value code,
        swazi_value msg, swazi_value* result);
    swazi_status (*create_range_error)(swazi_env env, swazi_value code,
        swazi_value msg, swazi_value* result);

    // other error handling apis
    swazi_status (*get_last_error)(swazi_env env, swazi_error_info* info);

    // ------------------------------------------------------------------------
    // Buffer Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_buffer)(swazi_env env, size_t length,
        void** data, swazi_value* result);
    swazi_status (*create_external_buffer)(swazi_env env, size_t length,
        void* data, swazi_finalize finalize_cb,
        void* finalize_hint,
        swazi_value* result);
    swazi_status (*create_buffer_copy)(swazi_env env, size_t length,
        const void* data, void** result_data,
        swazi_value* result);
    swazi_status (*get_buffer_info)(swazi_env env, swazi_value value,
        void** data, size_t* length);

    // ------------------------------------------------------------------------
    // Promise Operations
    // ------------------------------------------------------------------------
    // In swazi_abi.h, add to Promise Operations comment:
    // IMPORTANT: swazi_deferred must be resolved or rejected to prevent memory leaks.
    // Always call resolve_deferred/reject_deferred or their async variants.
    // In error paths, call reject_deferred before returning.
    swazi_status (*create_promise)(swazi_env env, swazi_deferred* deferred,
        swazi_value* promise);
    swazi_status (*resolve_deferred)(swazi_env env, swazi_deferred deferred,
        swazi_value resolution);
    swazi_status (*reject_deferred)(swazi_env env, swazi_deferred deferred,
        swazi_value rejection);

    // ============================================================================
    // Async/Scheduler Operations (Thread-Safe)
    // ============================================================================

    // Queue a callback to run on the main event loop thread (macrotask)
    // Safe to call from any thread
    swazi_status (*queue_macrotask)(
        swazi_env env,
        swazi_callback callback,
        void* user_data);

    // Queue a callback to run as a microtask on the main loop thread
    // Safe to call from any thread
    swazi_status (*queue_microtask)(
        swazi_env env,
        swazi_callback callback,
        void* user_data);

    // Resolve a deferred promise from ANY thread (queues resolution to main loop)
    // This is the thread-safe version of resolve_deferred
    swazi_status (*resolve_deferred_async)(
        swazi_env env,
        swazi_deferred deferred,
        swazi_value resolution);

    // Reject a deferred promise from ANY thread (queues rejection to main loop)
    // This is the thread-safe version of reject_deferred
    swazi_status (*reject_deferred_async)(
        swazi_env env,
        swazi_deferred deferred,
        swazi_value rejection);

    // Call before spawning background threads
    void (*thread_will_start)(swazi_env env);

    // Call after thread completes (before returning from thread func)
    void (*thread_did_finish)(swazi_env env);

    void* (*get_event_loop)(swazi_env env);

    // OR: Wrap uv_queue_work in the ABI (better encapsulation)
    swazi_status (*queue_background_work)(
        swazi_env env,
        void (*work_cb)(void* data),   // Runs on thread pool
        void (*after_cb)(void* data),  // Runs on main loop
        void* user_data);

    // ------------------------------------------------------------------------
    // Reference Management
    // ------------------------------------------------------------------------

    swazi_status (*create_reference)(swazi_env env, swazi_value value,
        uint32_t initial_refcount,
        swazi_ref* result);
    swazi_status (*delete_reference)(swazi_env env, swazi_ref ref);
    swazi_status (*reference_ref)(swazi_env env, swazi_ref ref,
        uint32_t* result);
    swazi_status (*reference_unref)(swazi_env env, swazi_ref ref,
        uint32_t* result);
    swazi_status (*get_reference_value)(swazi_env env, swazi_ref ref,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // Type Coercion
    // ------------------------------------------------------------------------

    swazi_status (*coerce_to_bool)(swazi_env env, swazi_value value,
        swazi_value* result);
    swazi_status (*coerce_to_number)(swazi_env env, swazi_value value,
        swazi_value* result);
    swazi_status (*coerce_to_string)(swazi_env env, swazi_value value,
        swazi_value* result);
    swazi_status (*coerce_to_object)(swazi_env env, swazi_value value,
        swazi_value* result);

    // ------------------------------------------------------------------------
    // Strict Equality
    // ------------------------------------------------------------------------

    swazi_status (*strict_equals)(swazi_env env, swazi_value lhs,
        swazi_value rhs, bool* result);

    // ------------------------------------------------------------------------
    // External Data (wrap C/C++ objects)
    // ------------------------------------------------------------------------

    swazi_status (*create_external)(swazi_env env, void* data,
        swazi_finalize finalize_cb,
        void* finalize_hint,
        swazi_value* result);
    swazi_status (*get_value_external)(swazi_env env, swazi_value value,
        void** result);

    swazi_status (*finalize_external)(swazi_env env, swazi_value value);

    // ------------------------------------------------------------------------
    // DateTime Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_date)(swazi_env env, double time,
        swazi_value* result);
    swazi_status (*get_date_value)(swazi_env env, swazi_value value,
        double* result);

    // DateTime field getters
    swazi_status (*datetime_get_year)(swazi_env env, swazi_value dt, int32_t* year);
    swazi_status (*datetime_get_month)(swazi_env env, swazi_value dt, int32_t* month);
    swazi_status (*datetime_get_day)(swazi_env env, swazi_value dt, int32_t* day);
    swazi_status (*datetime_get_hour)(swazi_env env, swazi_value dt, int32_t* hour);
    swazi_status (*datetime_get_minute)(swazi_env env, swazi_value dt, int32_t* minute);
    swazi_status (*datetime_get_second)(swazi_env env, swazi_value dt, int32_t* second);

    // DateTime field setters (return new DateTime)
    swazi_status (*datetime_set_year)(swazi_env env, swazi_value dt, int32_t year,
        swazi_value* result);
    swazi_status (*datetime_set_month)(swazi_env env, swazi_value dt, int32_t month,
        swazi_value* result);
    swazi_status (*datetime_set_day)(swazi_env env, swazi_value dt, int32_t day,
        swazi_value* result);

    // DateTime arithmetic
    swazi_status (*datetime_add_days)(swazi_env env, swazi_value dt, int days,
        swazi_value* result);
    swazi_status (*datetime_add_months)(swazi_env env, swazi_value dt, int months,
        swazi_value* result);
    swazi_status (*datetime_add_years)(swazi_env env, swazi_value dt, int years,
        swazi_value* result);
    swazi_status (*datetime_add_hours)(swazi_env env, swazi_value dt, double hours,
        swazi_value* result);
    swazi_status (*datetime_add_seconds)(swazi_env env, swazi_value dt, double seconds,
        swazi_value* result);

    // DateTime formatting
    swazi_status (*datetime_format)(swazi_env env, swazi_value dt,
        const char* format, char* buf,
        size_t bufsize, size_t* result);

    // DateTime timezone
    swazi_status (*datetime_set_timezone)(swazi_env env, swazi_value dt,
        const char* tz, swazi_value* result);

    // ------------------------------------------------------------------------
    // Range Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_range)(swazi_env env, int32_t start, int32_t end,
        size_t step, bool inclusive, swazi_value* result);
    swazi_status (*range_has_next)(swazi_env env, swazi_value range, bool* result);
    swazi_status (*range_next)(swazi_env env, swazi_value range, int32_t* result);
    swazi_status (*range_reset)(swazi_env env, swazi_value range);

    // ------------------------------------------------------------------------
    // Regex Operations
    // ------------------------------------------------------------------------

    swazi_status (*create_regex)(swazi_env env, const char* pattern,
        const char* flags, swazi_value* result);
    swazi_status (*regex_test)(swazi_env env, swazi_value regex,
        swazi_value str, bool* result);
    swazi_status (*regex_exec)(swazi_env env, swazi_value regex,
        swazi_value str, swazi_value* result);
    swazi_status (*regex_get_last_index)(swazi_env env, swazi_value regex,
        size_t* result);
    swazi_status (*regex_set_last_index)(swazi_env env, swazi_value regex,
        size_t index);

    // ------------------------------------------------------------------------
    // Instance Checking
    // ------------------------------------------------------------------------

    swazi_status (*instanceof)(swazi_env env, swazi_value object,
        swazi_value constructor, bool* result);

    // ============================================================================
    // Class Operations - Full-Featured Class Support
    // ============================================================================

    // Create a new class (optionally with parent class for inheritance)
    swazi_status (*create_class)(
        swazi_env env,
        const char* name,          // Class name (UTF-8)
        swazi_value parent_class,  // Parent class or NULL for no inheritance
        swazi_value* result        // Output: new class
    );

    // Define instance or static method
    // - Constructor: use SWAZI_CLASS_MEMBER_CONSTRUCTOR flag
    // - Destructor: use SWAZI_CLASS_MEMBER_DESTRUCTOR flag
    // - Getters: use SWAZI_CLASS_MEMBER_GETTER flag
    // - Methods automatically receive 'this' ($) as receiver
    swazi_status (*class_define_method)(
        swazi_env env,
        swazi_value class_val,
        const char* method_name,
        swazi_callback callback,
        void* user_data,
        uint32_t flags  // Combination of swazi_class_member_flags
    );

    // Define instance or static property with initial value
    swazi_status (*class_define_property)(
        swazi_env env,
        swazi_value class_val,
        const char* property_name,
        swazi_value initial_value,  // NULL for no initializer
        uint32_t flags              // Combination of swazi_class_member_flags
    );

    // Modify existing method (change callback, user_data, or flags)
    swazi_status (*class_modify_method)(
        swazi_env env,
        swazi_value class_val,
        const char* method_name,
        swazi_callback new_callback,  // NULL to keep existing
        void* new_user_data,          // Ignored if new_callback is NULL
        uint32_t flags                // New flags (0 to keep existing)
    );

    // Modify existing property (change value or flags)
    swazi_status (*class_modify_property)(
        swazi_env env,
        swazi_value class_val,
        const char* property_name,
        swazi_value new_value,  // NULL to keep existing value
        uint32_t flags          // New flags (0 to keep existing)
    );

    // Remove method from class
    swazi_status (*class_remove_method)(
        swazi_env env,
        swazi_value class_val,
        const char* method_name);

    // Remove property from class
    swazi_status (*class_remove_property)(
        swazi_env env,
        swazi_value class_val,
        const char* property_name);

    // Check if class has a method
    swazi_status (*class_has_method)(
        swazi_env env,
        swazi_value class_val,
        const char* method_name,
        bool* result);

    // Check if class has a property
    swazi_status (*class_has_property)(
        swazi_env env,
        swazi_value class_val,
        const char* property_name,
        bool* result);

    // Get parent class (returns NULL if no parent)
    swazi_status (*class_get_parent)(
        swazi_env env,
        swazi_value class_val,
        swazi_value* result  // Output: parent class or NULL
    );

    // Set parent class (for dynamic inheritance - use with caution)
    swazi_status (*class_set_parent)(
        swazi_env env,
        swazi_value class_val,
        swazi_value parent_class  // Parent class or NULL to remove
    );

    // Call parent constructor from within a native constructor
    // - Only valid inside constructor callback
    // - Automatically passes 'this' ($) to parent constructor
    swazi_status (*class_call_super_constructor)(
        swazi_env env,
        swazi_callback_info info,
        size_t argc,
        const swazi_value* argv);

    // Get receiver ('this' / $) from callback info
    // - Available in: methods, constructor, destructor, getters
    swazi_status (*get_receiver)(
        swazi_env env,
        swazi_callback_info info,
        swazi_value* receiver);

    // Get class from instance
    swazi_status (*get_instance_class)(
        swazi_env env,
        swazi_value instance,
        swazi_value* class_val);

    // Check if instance is of a given class (exact match, no inheritance check)
    swazi_status (*instance_of)(
        swazi_env env,
        swazi_value instance,
        swazi_value class_val,
        bool* result);

    // HandleScope Management (Automatic Cleanup)
    swazi_status (*open_handle_scope)(
        swazi_env env,
        swazi_handle_scope* scope);

    swazi_status (*close_handle_scope)(
        swazi_env env,
        swazi_handle_scope scope);

    // For values that need to outlive the scope
    swazi_status (*open_escapable_handle_scope)(
        swazi_env env,
        swazi_escapable_handle_scope* scope);

    swazi_status (*close_escapable_handle_scope)(
        swazi_env env,
        swazi_escapable_handle_scope scope);

    swazi_status (*escape_handle)(
        swazi_env env,
        swazi_escapable_handle_scope scope,
        swazi_value escapee,
        swazi_value* result);

} swazi_api;

// ============================================================================
// Global API Access
// ============================================================================

// Get the API table (call this once at addon initialization)
SWAZI_EXTERN const swazi_api* swazi_get_api(void);

// Convenience macro for accessing API
#define SWAZI_API swazi_get_api()

// ============================================================================
// Helper Macros for Common Patterns
// ============================================================================

// Check status and return early on error
#define SWAZI_ASSERT_STATUS(call)              \
    do {                                       \
        swazi_status __status = (call);        \
        if (__status != SWAZI_OK) return NULL; \
    } while (0)

// Check argument count
#define SWAZI_ASSERT_ARGC(env, argc, expected)             \
    do {                                                   \
        if ((argc) < (expected)) {                         \
            swazi_get_api()->throw_type_error((env), NULL, \
                "Wrong number of arguments");              \
            return NULL;                                   \
        }                                                  \
    } while (0)

// Extract callback info
#define SWAZI_GET_CB_INFO(env, info, argc, argv, this, data)     \
    do {                                                         \
        size_t __argc = (argc);                                  \
        SWAZI_ASSERT_STATUS(                                     \
            swazi_get_api()->get_cb_info((env), (info), &__argc, \
                (argv), (this), (data)));                        \
        (argc) = __argc;                                         \
    } while (0)

// Create and throw error
#define SWAZI_THROW_ERROR(env, msg)                       \
    do {                                                  \
        swazi_get_api()->throw_error((env), NULL, (msg)); \
        return NULL;                                      \
    } while (0)

#define SWAZI_THROW_TYPE_ERROR(env, msg)                       \
    do {                                                       \
        swazi_get_api()->throw_type_error((env), NULL, (msg)); \
        return NULL;                                           \
    } while (0)

#ifdef __cplusplus
}  // extern "C"
#endif

// ============================================================================
// C++ Helper Class (must be OUTSIDE extern "C")
// ============================================================================

#ifdef __cplusplus

namespace swazi {

// RAII wrapper for string extraction
class String {
   public:
    String(swazi_env env, swazi_value value) : env_(env), value_(value) {
        const swazi_api* api = swazi_get_api();
        size_t length = 0;
        api->get_value_string_length(env, value, &length);
        buffer_.resize(length + 1);
        api->get_value_string_utf8(env, value, buffer_.data(),
            buffer_.size(), &length);
    }

    const char* c_str() const { return buffer_.data(); }
    std::string str() const { return std::string(buffer_.data()); }

   private:
    swazi_env env_;
    swazi_value value_;
    std::vector<char> buffer_;
};

// Helper to check if value is a specific type
inline bool IsNumber(swazi_env env, swazi_value value) {
    swazi_valuetype type;
    swazi_get_api()->typeof_value(env, value, &type);
    return type == SWAZI_NUMBER;
}

inline bool IsString(swazi_env env, swazi_value value) {
    swazi_valuetype type;
    swazi_get_api()->typeof_value(env, value, &type);
    return type == SWAZI_STRING;
}

inline bool IsObject(swazi_env env, swazi_value value) {
    swazi_valuetype type;
    swazi_get_api()->typeof_value(env, value, &type);
    return type == SWAZI_OBJECT;
}

inline bool IsArray(swazi_env env, swazi_value value) {
    bool result;
    swazi_get_api()->is_array(env, value, &result);
    return result;
}

class HandleScope {
    swazi_env env_;
    swazi_handle_scope scope_;

   public:
    HandleScope(swazi_env env) : env_(env) {
        swazi_get_api()->open_handle_scope(env_, &scope_);
    }
    ~HandleScope() {
        swazi_get_api()->close_handle_scope(env_, scope_);
    }
    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;
};

class EscapableHandleScope {
    swazi_env env_;
    swazi_escapable_handle_scope scope_;

   public:
    EscapableHandleScope(swazi_env env) : env_(env) {
        swazi_get_api()->open_escapable_handle_scope(env_, &scope_);
    }
    ~EscapableHandleScope() {
        swazi_get_api()->close_escapable_handle_scope(env_, scope_);
    }
    swazi_value escape(swazi_value value) {
        swazi_value result;
        swazi_get_api()->escape_handle(env_, scope_, value, &result);
        return result;
    }
};

}  // namespace swazi

#endif  // __cplusplus

#endif  // SWAZI_H
