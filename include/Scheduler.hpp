#pragma once

#ifndef SWAZI_SCHEDULER_HPP
#define SWAZI_SCHEDULER_HPP

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "Frame.hpp"

// libuv (use local header path)
#include "uv.h"

// Simple scheduler that holds microtask and macrotask queues.
// This Scheduler implementation now uses libuv under the hood.
class Scheduler {
   public:
    Scheduler();
    ~Scheduler();

    void enqueue_microtask(const Continuation& task);
    void enqueue_macrotask(const Continuation& task);

    // Run one "tick": drain microtasks then run one macrotask if available.
    bool run_one();

    // Run until idle. If has_pending is non-null it will be consulted to know whether
    // external work sources (timers) still exist. The scheduler returns when:
    //   - no macrotasks are available AND (has_pending == nullptr || has_pending() == false)
    // The predicate will be rechecked each time the wait is woken.
    void run_until_idle(const std::function<bool()>& has_pending = nullptr);

    void stop();

    // Notify the scheduler (wake any waiting run_until_idle). Useful for external
    // subsystems (timers) to wake the scheduler when their state changes.
    void notify();

    // Access underlying uv_loop_t (or nullptr if none). Exported so timer subsystem
    // (AsyncApi.cpp) can create uv timers on the same loop.
    uv_loop_t* get_uv_loop() { return loop_; }

   private:
    std::deque<Continuation> microtasks;
    std::deque<Continuation> macrotasks;

    // protect both queues (microtasks need protection too)
    std::mutex macrotasks_mutex;
    std::mutex microtasks_mutex;

    // libuv loop + async wake handle for cross-thread wakeups.
    uv_loop_t* loop_ = nullptr;
    uv_async_t async_handle_;
    bool async_initialized = false;

    bool should_stop = false;
};

// Bridge functions (type-erased) — unchanged.
void register_scheduler_runner(Scheduler* s, std::function<void(void*)> runner);
void enqueue_callback_global(void* boxed_payload);

// Helper to let other translation units access the global scheduler loop (returns nullptr if none)
uv_loop_t* scheduler_get_loop();

// NEW: schedule a function to run on the scheduler's loop thread.
// If the scheduler is not available the function will be invoked synchronously inline.
void scheduler_run_on_loop(const std::function<void()>& fn);

// NEW: register a callback that the scheduler will invoke at the end of each tick
// (i.e., after a macrotask completes) and once more before run_until_idle returns.
// The callback is invoked on the scheduler's loop thread.
void register_tick_callback(const std::function<void()>& cb);

#endif  // SWAZI_SCHEDULER_HPP