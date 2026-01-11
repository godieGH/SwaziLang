#pragma once
// Small bridge payload type used to transfer callbacks from timer threads into the scheduler.
// Only translation units that need to build or consume the payload should include this header.

#include <uv.h>

#include <functional>

#include "evaluator.hpp"

// Box passed across thread boundaries. The receiver is responsible for deleting it.
struct CallbackPayload {
    FunctionPtr cb;
    std::vector<Value> args;
    CallbackPayload(FunctionPtr f, const std::vector<Value>& a) : cb(std::move(f)), args(a) {}
};

// Exported helper used by Evaluator::run_event_loop to tell the scheduler whether timers remain.
// Implemented in AsyncApi.cpp
bool async_timers_exist();

// Bridge functions implemented in Scheduler.cpp
// Enqueue a boxed payload (type-erased pointer) to be delivered on the scheduler loop.
// The implementation expects a pointer produced by `new CallbackPayload(...)` (but uses void* to avoid header cycles).
void enqueue_callback_global(void* boxed_payload);
// Enqueue a boxed payload (type-erased pointer) to be delivered as a microtask on the scheduler.
// The implementation expects a pointer produced by `new CallbackPayload(...)` (but uses void* to avoid header cycles).
void enqueue_microtask_global(void* boxed_payload);

// Return the scheduler uv loop, or nullptr if no scheduler is available.
uv_loop_t* scheduler_get_loop();

// Schedule a function to run on the scheduler loop thread. If no scheduler exists, the implementation
// may run the function inline as a fallback.
void scheduler_run_on_loop(const std::function<void()>& fn);

// Optional: register a tick callback invoked on the scheduler loop thread (implemented in Scheduler.cpp).
void register_tick_callback(const std::function<void()>& cb);

// fotward decl for work running on addons
bool addon_threads_exist();