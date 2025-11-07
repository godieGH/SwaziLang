#pragma once
#include <any>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <exception>

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
    std::unordered_map<const ExpressionNode*, PromisePtr> awaited_promises;
    std::unordered_map<const ExpressionNode*, Value> awaited_results;
    std::unordered_map<const ExpressionNode*, std::exception_ptr> awaited_exceptions;
    
    PromisePtr pending_promise = nullptr;
};
using CallFramePtr = std::shared_ptr<CallFrame>;

using Continuation = std::function<void()>;