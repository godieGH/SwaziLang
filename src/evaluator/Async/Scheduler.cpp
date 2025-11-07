#include "Scheduler.hpp"

Scheduler::Scheduler() = default;
Scheduler::~Scheduler() = default;

void Scheduler::enqueue_microtask(const Continuation& task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lk(microtasks_mutex);
        microtasks.push_back(task);
    }
    // Microtasks may be enqueued from other threads (timers etc.) — wake run loop.
    macrotasks_cv.notify_one();
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
        try { mtask(); } catch (...) {}
        return true;
    }
    return false;
}

void Scheduler::run_until_idle(const std::function<bool()>& has_pending) {
    while (!should_stop) {
        bool did_work = run_one();
        if (did_work) continue;

        std::unique_lock<std::mutex> lk(macrotasks_mutex);

        macrotasks_cv.wait(lk, [&]() {
            if (should_stop) return true;
            {
                std::lock_guard<std::mutex> ml(microtasks_mutex);
                if (!microtasks.empty()) return true;
            }
            if (!macrotasks.empty()) return true;
            if (has_pending) {
                bool pending = has_pending();
                return !pending;
            }
            return false;
        });

        if (should_stop) break;
        if (!macrotasks.empty()) continue;
        {
            std::lock_guard<std::mutex> ml(microtasks_mutex);
            if (!microtasks.empty()) continue;
        }
        if (has_pending && !has_pending()) break;
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