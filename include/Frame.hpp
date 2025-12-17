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

    // Loop state persistence for async/generator contexts
    struct LoopState {
        EnvPtr loop_env;             // The forEnv/loopEnv
        size_t iteration_count = 0;  // Track which iteration we're on
        size_t body_statement_index = 0;
        Value current_value;          // For for-in loops (current element/key)
        size_t current_index = 0;     // For for-in loops (position in array/object)
        bool is_first_entry = false;  // Track if this is first time entering loop

        // For classic for loops: kwa(init; cond; post)
        bool init_done = false;

        // For range/for-in loops
        std::vector<std::string> keys_snapshot;  // For object iteration
        size_t range_position = 0;               // For range iteration

        // Optional range copy - only initialized if iterating over a range
        std::unique_ptr<RangeValue> range_copy_ptr;

        // Default constructor
        LoopState() = default;

        // Helper to get/set range copy safely
        RangeValue& get_range_copy() {
            if (!range_copy_ptr) {
                // Create a dummy range if not set (should never happen in practice)
                range_copy_ptr = std::make_unique<RangeValue>(0, 0, 1, false);
            }
            return *range_copy_ptr;
        }

        void set_range_copy(const RangeValue& rv) {
            range_copy_ptr = std::make_unique<RangeValue>(rv);
        }

        bool has_range_copy() const {
            return range_copy_ptr != nullptr;
        }
    };

    EnvPtr paused_env = nullptr;

    // Map: statement address -> loop state (supports nested loops)
    std::unordered_map<void*, LoopState> loop_states;

    // Helper to check if we're resuming into a loop
    bool has_loop_state(void* loop_stmt) const {
        return loop_states.find(loop_stmt) != loop_states.end();
    }
};
using CallFramePtr = std::shared_ptr<CallFrame>;

using Continuation = std::function<void()>;