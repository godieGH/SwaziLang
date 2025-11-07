#ifndef SWAZI_FRAME_HPP
#define SWAZI_FRAME_HPP

#include <memory>
#include <functional>
#include <vector>
#include <string>
#include <any>

#include "token.hpp"

// Forward declarations used here — keep header minimal so it doesn't pull evaluator.hpp
struct FunctionValue;
using FunctionPtr = std::shared_ptr<FunctionValue>;
using EnvPtr = std::shared_ptr<class Environment>;

// Continuation: a small callable scheduled by the scheduler.
using Continuation = std::function<void()>;

// Simple CallFrame placeholder: we will expand this during the full refactor.
// Keep it POD-like and non-owning for now.
struct CallFrame {
    // function being executed (nullable for top-level tasks)
    FunctionPtr function;

    // lexical environment for the frame (closure / locals parent)
    EnvPtr env;
    
    // Execution cursor (index of next statement to execute)
    size_t next_statement_index = 0;

    // Whether the frame is currently suspended waiting for something
    bool is_suspended = false;

    // Opaque slot to store the awaited promise or any pending value (std::any avoids
    // depending on Value here, concrete code will std::any_cast<Value> where needed)
    std::any awaited_slot;

    // Optional continuation to call with a result when resuming (opaque typed)
    // Use std::any for the result parameter to avoid header dependency on Value.
    std::function<void(std::any)> resume_with_result;

    // token where the call originated (useful for diagnostics)
    Token call_token;

    // optional receiver ($) for method calls: forward-declare ObjectValue as incomplete
    std::shared_ptr<struct ObjectValue> receiver;

    // storage slot for return value / temporary while running (opaque)
    std::any return_value;

    // execution flags
    bool did_return = false;
    bool is_async = false;

    // user-visible stack label (function name or "<top>")
    std::string label;

    CallFrame() = default;
};

using CallFramePtr = std::shared_ptr<CallFrame>;

#endif // SWAZI_FRAME_HPP