#pragma once
#include <any>
#include <exception>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "ast.hpp"
#include "evaluator.hpp"

// forward
struct FunctionValue;
using FunctionPtr = std::shared_ptr<FunctionValue>;
using PromisePtr = std::shared_ptr<PromiseValue>;

struct CallFrame {
    FunctionPtr function;
    std::shared_ptr<Environment> env;
    size_t next_statement_index = 0;
    std::any return_value;
    bool did_return = false;
    Token call_token;
    std::string label;
    bool is_async = false;
    bool is_suspended = false;
    // For methods:
    std::shared_ptr<ObjectValue> receiver;

    // --- await bookkeeping (keyed by the AwaitExpressionNode pointer)
    std::unordered_map<size_t, PromisePtr> awaited_promises;
    std::unordered_map<size_t, Value> awaited_results;
    std::unordered_map<size_t, std::exception_ptr> awaited_exceptions;

    PromisePtr pending_promise = nullptr;
    
    Value generator_sent_value = std::monostate{};
    bool generator_has_sent_value = false;
    // pointer to the YieldExpressionNode where the generator is currently paused
    // (null when not paused on a yield).
    YieldExpressionNode* paused_yield = nullptr;
    
    Value generator_return_value = std::monostate{};
    bool generator_requested_return = false;
    
};
using CallFramePtr = std::shared_ptr<CallFrame>;

using Continuation = std::function<void()>;