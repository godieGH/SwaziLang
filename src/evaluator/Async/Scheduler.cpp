#include "Scheduler.hpp"

Scheduler::Scheduler() = default;
Scheduler::~Scheduler() = default;

void Scheduler::enqueue_microtask(const Continuation& task) {
    if (!task) return;
    microtasks.push_back(task);
}

void Scheduler::enqueue_macrotask(const Continuation& task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lk(macrotasks_mutex);
        macrotasks.push_back(task);
    }
    macrotasks_cv.notify_one();
}

bool Scheduler::run_one() {
    while (!microtasks.empty()) {
        Continuation t = microtasks.front();
        microtasks.pop_front();
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
        try { mtask(); } catch (...) {}
        return true;
    }
    return false;
}

void Scheduler::run_until_idle(const std::function<bool()>& has_pending) {
    // Keep running until should_stop or until no macrotasks and has_pending==false.
    while (!should_stop) {
        bool did_work = run_one();
        if (did_work) continue;

        // No immediate work: wait for macrotask arrival or for a change in external pending state.
        std::unique_lock<std::mutex> lk(macrotasks_mutex);

        // If a predicate is provided, wake when either:
        // - macrotasks is not empty (someone enqueued)
        // - should_stop requested
        // - predicate says there are no pending external tasks (i.e., we're allowed to exit)
        macrotasks_cv.wait(lk, [&]() {
            if (should_stop) return true;
            if (!macrotasks.empty()) return true;
            if (has_pending) {
                // if has_pending()==false -> safe to stop/wake and re-check exit conditions
                bool pending = has_pending();
                return !pending;
            }
            // no predicate: wake only when macrotasks non-empty or should_stop
            return false;
        });

        // After wake, loop continues and will re-evaluate run_one() and should_stop.
        // If the predicate returned true because there are no pending external tasks and
        // there are no macrotasks, the next run_one() will do nothing and the while will
        // loop again; since macrotasks still empty and has_pending() returned false,
        // the wait condition will be satisfied and we will break out via the loop condition.
        if (should_stop) break;
        // If we returned from wait because has_pending()==false and macrotasks empty,
        // the next iteration will call run_one() (no work) and then go back to wait,
        // but since has_pending()==false the wait will immediately return and we will exit.
        if (!macrotasks.empty()) continue;
        if (has_pending && !has_pending()) {
            // no more external work and no macrotasks -> done
            break;
        }
    }
}

void Scheduler::stop() {
    {
        std::lock_guard<std::mutex> lk(macrotasks_mutex);
        should_stop = true;
    }
    macrotasks_cv.notify_all();
}

void Scheduler::notify() {
    macrotasks_cv.notify_one();
}

// -------------------------
// Global bridge implementation (type-erased to avoid header cycles).
// -------------------------
static Scheduler* g_scheduler_instance = nullptr;
static std::function<void(void*)> g_scheduler_runner = nullptr;

void register_scheduler_runner(Scheduler* s, std::function<void(void*)> runner) {
    g_scheduler_instance = s;
    g_scheduler_runner = std::move(runner);
}

void enqueue_callback_global(void* boxed_payload) {
    if (!boxed_payload) return;
    if (!g_scheduler_instance) return;

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