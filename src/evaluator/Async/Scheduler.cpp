#include "Scheduler.hpp"

#include <iostream>

// Use local header path for libuv
#include "uv.h"

static Scheduler* g_scheduler_instance = nullptr;
static std::function<void(void*)> g_scheduler_runner = nullptr;

// Tick callback registered by owner (e.g., Evaluator) to run unhandled-rejection checks.
// Invoked on the scheduler/loop thread.
static std::function<void()> g_tick_callback = nullptr;

// libuv async callback used to wake the scheduler loop and make it process tasks.
// This runs on the loop thread.
//
// NOTE: We must not use the identifier `uv_async_cb` here because libuv's header
// defines a typedef with that name. Use a unique name to avoid the collision.
static void scheduler_uv_async_cb(uv_async_t* handle) {
    // No-op: uv_async_send will wake the loop so run_until_idle can process tasks.
    (void)handle;
}

Scheduler::Scheduler() {
    // create and initialize a private loop
    loop_ = new uv_loop_t;
    uv_loop_init(loop_);

    // init async handle to wake loop when new tasks are pushed
    async_handle_.data = this;
    uv_async_init(loop_, &async_handle_, scheduler_uv_async_cb);
    async_initialized = true;

    // register global pointer so other modules (AsyncApi.cpp) can get the loop
    g_scheduler_instance = this;
}

Scheduler::~Scheduler() {
    // stop accepting work
    should_stop = true;

    // close async handle and run loop to process close callbacks
    if (async_initialized) {
        uv_close(reinterpret_cast<uv_handle_t*>(&async_handle_), nullptr);
        // Run loop in non-blocking mode to let close callbacks run
        uv_run(loop_, UV_RUN_NOWAIT);
        async_initialized = false;
    }

    if (loop_) {
        uv_loop_close(loop_);
        delete loop_;
        loop_ = nullptr;
    }

    // unregister global
    if (g_scheduler_instance == this) g_scheduler_instance = nullptr;
}

void Scheduler::enqueue_microtask(const Continuation& task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lk(microtasks_mutex);
        microtasks.push_back(task);
    }
    // Wake loop so microtasks can be drained
    if (async_initialized) uv_async_send(&async_handle_);
}

void Scheduler::enqueue_macrotask(const Continuation& task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lk(macrotasks_mutex);
        macrotasks.push_back(task);
    }
    if (async_initialized) uv_async_send(&async_handle_);
}

// Drain microtasks then run one macrotask (same semantics as before)
bool Scheduler::run_one() {
    // Drain microtasks first (thread-safe snapshot drain)
    while (true) {
        Continuation t;
        {
            std::lock_guard<std::mutex> lk(microtasks_mutex);
            if (microtasks.empty()) break;
            t = microtasks.front();
            microtasks.pop_front();
        }
        try {
            if (t) t();
        } catch (...) {
            // swallow
        }
    }

    Continuation mtask;
    {
        std::lock_guard<std::mutex> lk(macrotasks_mutex);
        if (!macrotasks.empty()) {
            mtask = macrotasks.front();
            macrotasks.pop_front();
        }
    }

    if (mtask) {
        try {
            mtask();
        } catch (...) {}

        // Invoke tick callback after a macrotask finishes so the owner (Evaluator)
        // can perform unhandled-rejection checks or other housekeeping.
        if (g_tick_callback) {
            try {
                g_tick_callback();
            } catch (...) {
                // swallow errors from user-provided callback
            }
        }

        return true;
    }
    return false;
}

// Helper used with uv_walk to count active handles excluding the scheduler's async handle.
struct WalkData {
    uv_handle_t* exclude_handle;
    size_t active_count;
};

static void walk_count_cb(uv_handle_t* handle, void* arg) {
    WalkData* wd = static_cast<WalkData*>(arg);
    if (!wd) return;
    if (handle == wd->exclude_handle) return;  // ignore the scheduler's async handle
    // uv_is_active returns non-zero for active handles (e.g., timers, io, etc.)
    if (uv_is_active(handle)) {
        wd->active_count++;
    }
}

void Scheduler::run_until_idle(const std::function<bool()>& /*has_pending*/) {
    // Note: we intentionally ignore the external has_pending predicate and instead
    // consult libuv's active handles (excluding the scheduler async handle).
    // This avoids duplication and keeps the loop authoritative for I/O/timers.
    while (!should_stop) {
        bool did_work = run_one();
        if (did_work) continue;

        // If no pending tasks locally, check whether the libuv loop has any active handles
        {
            std::lock_guard<std::mutex> lk1(macrotasks_mutex);
            std::lock_guard<std::mutex> lk2(microtasks_mutex);
            bool local_empty = macrotasks.empty() && microtasks.empty();
            if (local_empty) {
                // Count active handles on the loop excluding our async wake handle.
                WalkData wd;
                wd.exclude_handle = reinterpret_cast<uv_handle_t*>(&async_handle_);
                wd.active_count = 0;
                uv_walk(loop_, walk_count_cb, &wd);

                // If no active handles besides our async handle, we are idle.
                if (wd.active_count == 0) {
                    // Before returning, allow the owner to perform a last tick check.
                    if (g_tick_callback) {
                        try {
                            g_tick_callback();
                        } catch (...) {}
                    }
                    break;
                }
            }
        }

        // Block running the libuv loop once - we'll be woken by uv_async_send or an active timer/io
        uv_run(loop_, UV_RUN_ONCE);

        // loop iteration returns either because of work or wake; re-check queues.
    }
}

void Scheduler::stop() {
    should_stop = true;
    // Wake the loop so run_until_idle can observe should_stop
    if (async_initialized) uv_async_send(&async_handle_);
}

void Scheduler::notify() {
    if (async_initialized) uv_async_send(&async_handle_);
}

// -------------------------
// Global bridge implementation (type-erased to avoid header cycles).
// -------------------------

void register_scheduler_runner(Scheduler* s, std::function<void(void*)> runner) {
    g_scheduler_instance = s;
    g_scheduler_runner = std::move(runner);
}

void enqueue_callback_global(void* boxed_payload) {
    if (!boxed_payload) return;
    if (!g_scheduler_instance) return;

    // Use scheduler enqueue_macrotask to schedule the runner to be invoked on the loop thread.
    // The runner will be called with boxed_payload as its argument.
    g_scheduler_instance->enqueue_macrotask([boxed_payload]() {
        if (g_scheduler_runner) {
            try {
                g_scheduler_runner(boxed_payload);
            } catch (...) {
                // runner should handle errors
            }
        }
    });
}
void enqueue_microtask_global(void* boxed_payload) {
    if (!boxed_payload) return;
    if (!g_scheduler_instance) return;

    // Use scheduler enqueue_microtask so the runner runs as a microtask on the loop thread.
    // The runner will be called with boxed_payload as its argument.
    g_scheduler_instance->enqueue_microtask([boxed_payload]() {
        if (g_scheduler_runner) {
            try {
                g_scheduler_runner(boxed_payload);
            } catch (...) {
                // runner should handle errors
            }
        }
    });
}

uv_loop_t* scheduler_get_loop() {
    if (!g_scheduler_instance) return nullptr;
    return g_scheduler_instance->get_uv_loop();
}

// NEW: schedule a function to run on the scheduler loop thread.
// If the scheduler is not available this will run fn inline (fallback).
void scheduler_run_on_loop(const std::function<void()>& fn) {
    if (!fn) return;
    if (!g_scheduler_instance) {
        // fallback: run inline
        try {
            fn();
        } catch (...) {}
        return;
    }
    // enqueue on macrotask queue so it will run on the loop thread
    g_scheduler_instance->enqueue_macrotask(fn);
}

// NEW: register a per-tick callback invoked on the loop thread.
void register_tick_callback(const std::function<void()>& cb) {
    g_tick_callback = cb;
}