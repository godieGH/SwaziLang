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
// It can be a lambda that resumes a suspended frame or any task.
using Continuation = std::function<void()>;

// Simple CallFrame placeholder: we will expand this during the full refactor.
// Keep it POD-like and non-owning for now.
struct CallFrame {
    // function being executed (nullable for top-level tasks)
    FunctionPtr function;

    // lexical environment for the frame (closure / locals parent)
    EnvPtr env;

    // token where the call originated (useful for diagnostics)
    Token call_token;

    // optional receiver ($) for method calls
    std::shared_ptr<struct ObjectValue> receiver;

    // storage slot for return value / temporary while running.
    // Use std::any here to avoid a hard dependency on the interpreter's Value
    // type in this header (breaks include cycle). When we fully reify frames
    // we will replace this with the actual Value or a boxed Value type.
    std::any return_value;

    // execution flags
    bool did_return = false;
    bool is_async = false;  // will be used later

    // user-visible stack label (function name or "<top>")
    std::string label;

    CallFrame() = default;
};

using CallFramePtr = std::shared_ptr<CallFrame>;

#endif // SWAZI_FRAME_HPP