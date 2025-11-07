#pragma once
// Small bridge payload type used to transfer callbacks from timer threads into the scheduler.
// Only translation units that need to build or consume the payload should include this header.

#include "evaluator.hpp"

struct CallbackPayload {
    FunctionPtr cb;
    std::vector<Value> args;
    CallbackPayload(FunctionPtr f, const std::vector<Value>& a) : cb(std::move(f)), args(a) {}
};

// Exported helper used by Evaluator::run_event_loop to tell the scheduler whether timers remain.
// Implemented in AsyncApi.cpp
bool async_timers_exist();