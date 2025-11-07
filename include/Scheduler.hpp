#ifndef SWAZI_SCHEDULER_HPP
#define SWAZI_SCHEDULER_HPP

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

#include "Frame.hpp"

// Simple scheduler that holds microtask and macrotask queues.
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

private:
    std::deque<Continuation> microtasks;
    std::deque<Continuation> macrotasks;
    std::mutex macrotasks_mutex;
    std::condition_variable macrotasks_cv;
    bool should_stop = false;
};

// Bridge functions (type-erased) — unchanged.
void register_scheduler_runner(Scheduler* s, std::function<void(void*)> runner);
void enqueue_callback_global(void* boxed_payload);

#endif // SWAZI_SCHEDULER_HPP