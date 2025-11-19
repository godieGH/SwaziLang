#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "AsyncBridge.hpp"  // -> defines CallbackPayload
#include "Scheduler.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

// libuv
#include "uv.h"

// Timer bookkeeping (unchanged)...
static std::mutex g_timers_mutex;
static std::atomic<long long> g_next_timer_id{1};
struct TimerEntry {
    long long id;
    std::atomic<bool> cancelled;
    long long delay_ms;
    long long interval_ms;  // 0 for single-shot
    FunctionPtr cb;
    std::vector<Value> args;

    // uv handle pointer (if using libuv)
    uv_timer_t* uv_handle = nullptr;
};
static std::unordered_map<long long, std::shared_ptr<TimerEntry>> g_timers;

// Forward declaration: check whether any timers exist (used by run_event_loop).
bool async_timers_exist();

// Enqueue a language callback (used across this file)
// Build a heap-allocated CallbackPayload and hand it to the scheduler bridge.
static void enqueue_callback(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    // allocate a payload copy that the evaluator will delete after use
    CallbackPayload* box = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(box));
}

// Evaluator: schedule callback (exposed)
void Evaluator::schedule_callback(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    // If scheduler present, use it; otherwise fallback to the global bridge.
    if (scheduler()) {
        // create a macrotask that calls call_function on this evaluator instance
        FunctionPtr cb_copy = cb;
        std::vector<Value> args_copy = args;
        scheduler()->enqueue_macrotask([this, cb_copy, args_copy]() {
            try {
                this->call_function(cb_copy, args_copy, cb_copy->closure, cb_copy->token);
            } catch (const std::exception& e) {
                std::cerr << "Unhandled async callback exception: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unhandled async callback unknown exception" << std::endl;
            }
        });
    } else {
        enqueue_callback(cb, args);
    }
}

// Evaluator: drain the queue (existing behavior)
// When a Scheduler exists, run_until_idle takes a predicate that lets the scheduler
// know whether there are active async timers; this allows the scheduler to remain
// running until timers complete.
void Evaluator::run_event_loop() {
    if (scheduler()) {
        scheduler()->run_until_idle([]() {
            return async_timers_exist() || tcp_has_active_work() || net_has_active_work();
        });
        return;
    }
    return;
}

// --- libuv timer support --------------------------------------------------
// We will create uv_timer_t handles on the Scheduler's loop if available.
// If not available (no scheduler / no uv loop) we fall back to the original
// thread-based implementation for compatibility.

// Timer callback called on the scheduler's loop thread.
static void uv_timer_callback(uv_timer_t* handle) {
    if (!handle) return;
    TimerEntry* te = static_cast<TimerEntry*>(handle->data);
    if (!te) return;
    if (te->cancelled.load()) {
        return;
    }
    if (te->cb) enqueue_callback(te->cb, te->args);

    // For single-shot timers, cleanup: stop & close the uv handle and remove entry.
    if (te->interval_ms <= 0) {
        // stop and close the handle
        uv_timer_stop(handle);
        // schedule close with a close callback to free handle memory
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
            uv_timer_t* t = reinterpret_cast<uv_timer_t*>(h);
            // free the uv handle memory
            delete t;
        });

        // remove from g_timers map
        {
            std::lock_guard<std::mutex> lk(g_timers_mutex);
            g_timers.erase(te->id);
        }
    }
}

// Create/cancel timer helpers
static long long create_timer(long long delay_ms, long long interval_ms, FunctionPtr cb, const std::vector<Value>& args) {
    auto te = std::make_shared<TimerEntry>();
    te->id = g_next_timer_id.fetch_add(1);
    te->cancelled.store(false);
    te->delay_ms = delay_ms;
    te->interval_ms = interval_ms;
    te->cb = cb;
    te->args = args;
    {
        std::lock_guard<std::mutex> lk(g_timers_mutex);
        g_timers[te->id] = te;
    }

    // If scheduler + uv loop exists, create the uv_timer_t on the loop thread via scheduler_run_on_loop.
    uv_loop_t* loop = scheduler_get_loop();
    if (loop) {
        // Capture shared_ptr by value so it survives until the scheduled lambda runs.
        std::shared_ptr<TimerEntry> te_copy = te;
        scheduler_run_on_loop([loop, te_copy]() {
            if (!te_copy) return;
            // allocate handle on heap — must be freed by uv_close callback
            uv_timer_t* timer_handle = new uv_timer_t;
            timer_handle->data = te_copy.get();
            te_copy->uv_handle = timer_handle;

            // initialize & start on the loop thread
            int r = uv_timer_init(loop, timer_handle);
            if (r != 0) {
                // initialization failed; clean up and remove entry
                delete timer_handle;
                {
                    std::lock_guard<std::mutex> lk(g_timers_mutex);
                    g_timers.erase(te_copy->id);
                }
                return;
            }

            uint64_t repeat = (te_copy->interval_ms > 0) ? static_cast<uint64_t>(te_copy->interval_ms) : 0;
            uv_timer_start(timer_handle, uv_timer_callback, static_cast<uint64_t>(te_copy->delay_ms), repeat);
        });

        return te->id;
    }

    // Fallback: if no uv loop, create a thread as before (single-shot or repeating).
    std::thread([te]() {
        if (te->delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(te->delay_ms));
        while (true) {
            if (te->cancelled.load()) break;
            if (te->cb) enqueue_callback(te->cb, te->args);
            if (te->interval_ms <= 0) break;  // single-shot done
            long long slept = 0;
            const long long slice = 50;
            while (slept < te->interval_ms) {
                if (te->cancelled.load()) break;
                long long remaining = te->interval_ms - slept;
                std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long long>(std::min<long long>(slice, remaining))));
                slept += slice;
            }
            if (te->cancelled.load()) break;
        }
        {
            std::lock_guard<std::mutex> lk(g_timers_mutex);
            g_timers.erase(te->id);
        }
        // Wake the evaluator/scheduler bridge by enqueueing a noop payload
        CallbackPayload* wake = new CallbackPayload(nullptr, {});
        enqueue_callback_global(static_cast<void*>(wake));
    }).detach();

    return te->id;
}

static void cancel_timer(long long id) {
    std::shared_ptr<TimerEntry> te;
    {
        std::lock_guard<std::mutex> lk(g_timers_mutex);
        auto it = g_timers.find(id);
        if (it == g_timers.end()) return;
        te = it->second;
    }
    if (!te) return;
    te->cancelled.store(true);

    // If a uv handle exists, schedule its stop/close on the loop thread.
    if (te->uv_handle) {
        // capture shared_ptr by value so it lives until the lambda runs
        std::shared_ptr<TimerEntry> te_copy = te;
        scheduler_run_on_loop([te_copy]() {
            if (!te_copy) return;
            uv_timer_t* h = te_copy->uv_handle;
            if (h) {
                uv_timer_stop(h);
                uv_close(reinterpret_cast<uv_handle_t*>(h), [](uv_handle_t* hh) {
                    uv_timer_t* th = reinterpret_cast<uv_timer_t*>(hh);
                    delete th;
                });
                te_copy->uv_handle = nullptr;
            }
            // remove entry from map
            {
                std::lock_guard<std::mutex> lk(g_timers_mutex);
                g_timers.erase(te_copy->id);
            }
        });
        return;
    }

    // thread-based timers will observe cancelled flag and exit; remove entry now.
    {
        std::lock_guard<std::mutex> lk(g_timers_mutex);
        g_timers.erase(id);
    }
}

// Helpers to coerce and detect args
static long long value_to_ms(const Value& v) {
    if (std::holds_alternative<double>(v)) {
        double d = std::get<double>(v);
        if (d < 0) return 0;
        return static_cast<long long>(d);
    }
    throw std::runtime_error("Expected numeric milliseconds.");
}

static bool is_function_value(const Value& v) {
    return std::holds_alternative<FunctionPtr>(v);
}

// Given args vector, try to extract (delay_ms, cb, rest_args).
// Accepts two common forms:
//   1) (ms, cb, ...rest)
//   2) (cb, ms, ...rest)
// Returns tuple(delay_ms, cbPtr, vector<rest>).
static std::tuple<long long, FunctionPtr, std::vector<Value>> parse_timer_args(const std::vector<Value>& args, const Token& token) {
    if (args.size() < 2) throw std::runtime_error("Timer requires at least 2 arguments (ms, cb) or (cb, ms) at " + token.loc.to_string());

    // form A: first is number, second is function
    if (!args.empty() && std::holds_alternative<double>(args[0]) && is_function_value(args[1])) {
        long long ms = value_to_ms(args[0]);
        FunctionPtr cb = std::get<FunctionPtr>(args[1]);
        std::vector<Value> rest;
        for (size_t i = 2; i < args.size(); ++i) rest.push_back(args[i]);
        return {ms, cb, rest};
    }

    // form B: first is function, second is number
    if (is_function_value(args[0]) && args.size() >= 2 && std::holds_alternative<double>(args[1])) {
        FunctionPtr cb = std::get<FunctionPtr>(args[0]);
        long long ms = value_to_ms(args[1]);
        std::vector<Value> rest;
        for (size_t i = 2; i < args.size(); ++i) rest.push_back(args[i]);
        return {ms, cb, rest};
    }

    // Not recognized
    throw std::runtime_error("Timer: expected arguments (ms, cb, ...) or (cb, ms, ...) at " + token.loc.to_string());
}

// Builtin factory: make_timers_exports (extended with tolerant timers and nap)
std::shared_ptr<ObjectValue> make_timers_exports(EnvPtr /*env*/) {
    auto obj = std::make_shared<ObjectValue>();

    // timers.queueMacrotask(cb, ...args)
    auto native_queueMacrotask = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("timers.queueMacrotask requires callback at " + token.loc.to_string());
        if (!is_function_value(args[0])) throw std::runtime_error("timers.queueMacrotask first arg must be a function at " + token.loc.to_string());
        FunctionPtr cb = std::get<FunctionPtr>(args[0]);
        std::vector<Value> cb_args;
        for (size_t i = 1; i < args.size(); ++i) cb_args.push_back(args[i]);
        enqueue_callback(cb, cb_args);
        return std::monostate{};
    };
    Token tsub;
    tsub.type = TokenType::IDENTIFIER;
    tsub.loc = TokenLocation("<timers>", 0, 0, 0);
    auto fn_sub = std::make_shared<FunctionValue>(std::string("native:timers.queueMacrotask"), native_queueMacrotask, nullptr, tsub);
    obj->properties["queueMacrotask"] = PropertyDescriptor{fn_sub, false, false, false, tsub};

    // timers.setTimeout(ms, cb) or (cb, ms)
    auto native_setTimeout = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        long long ms;
        FunctionPtr cb;
        std::vector<Value> rest;
        std::tie(ms, cb, rest) = parse_timer_args(args, token);
        long long id = create_timer(ms, 0, cb, rest);
        return Value{static_cast<double>(id)};
    };
    Token tSet;
    tSet.type = TokenType::IDENTIFIER;
    tSet.loc = TokenLocation("<timers>", 0, 0, 0);
    auto fn_set = std::make_shared<FunctionValue>(std::string("native:timers.setTimeout"), native_setTimeout, nullptr, tSet);
    obj->properties["setTimeout"] = PropertyDescriptor{fn_set, false, false, false, tSet};

    // timers.clearTimeout(id)
    auto native_clearTimeout = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("timers.clearTimeout requires id at " + token.loc.to_string());
        if (!std::holds_alternative<double>(args[0])) throw std::runtime_error("timers.clearTimeout id must be number at " + token.loc.to_string());
        long long id = static_cast<long long>(std::get<double>(args[0]));
        cancel_timer(id);
        return std::monostate{};
    };
    Token tClear;
    tClear.type = TokenType::IDENTIFIER;
    tClear.loc = TokenLocation("<timers>", 0, 0, 0);
    auto fn_clear = std::make_shared<FunctionValue>(std::string("native:timers.clearTimeout"), native_clearTimeout, nullptr, tClear);
    obj->properties["clearTimeout"] = PropertyDescriptor{fn_clear, false, false, false, tClear};

    // timers.setInterval(ms, cb) or (cb, ms)
    auto native_setInterval = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        long long ms;
        FunctionPtr cb;
        std::vector<Value> rest;
        std::tie(ms, cb, rest) = parse_timer_args(args, token);
        long long id = create_timer(ms, ms, cb, rest);
        return Value{static_cast<double>(id)};
    };
    Token tInt;
    tInt.type = TokenType::IDENTIFIER;
    tInt.loc = TokenLocation("<timers>", 0, 0, 0);
    auto fn_int = std::make_shared<FunctionValue>(std::string("native:timers.setInterval"), native_setInterval, nullptr, tInt);
    obj->properties["setInterval"] = PropertyDescriptor{fn_int, false, false, false, tInt};

    // timers.clearInterval(id) -> same as clearTimeout
    obj->properties["clearInterval"] = obj->properties["clearTimeout"];

    // timers.nap(ms, cb?) — accept either (ms, cb) or (cb, ms) or (ms) (no cb).
    // when used with only one arg ms it returns a promise so can be used to suspend awated promise
    auto native_nap = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("timers.nap requires at least ms argument at " + token.loc.to_string());

        // If single numeric arg: schedule timer with no callback and return id
        if (args.size() == 1 && std::holds_alternative<double>(args[0])) {
            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            auto resolver = [promise](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                // Fulfill with undefined after delay
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = std::monostate{};

                // Trigger then callbacks via microtasks
                for (auto& cb : promise->then_callbacks) {
                    scheduler_run_on_loop([cb, v = promise->result]() {
                        try {
                            cb(v);
                        } catch (...) {}
                    });
                }

                return std::monostate{};
            };

            long long ms = value_to_ms(args[0]);
            long long id = create_timer(ms, 0, nullptr, {});
            auto resolver_fn = std::make_shared<FunctionValue>(
                "nap_resolver", resolver, nullptr, token);

            create_timer(ms, 0, resolver_fn, {});
            return promise;
        }

        // If two args in either order where one is number and other function -> schedule
        if (args.size() >= 2) {
            try {
                long long ms;
                FunctionPtr cb;
                std::vector<Value> rest;
                std::tie(ms, cb, rest) = parse_timer_args(args, token);
                long long id = create_timer(ms, 0, cb, rest);
                return Value{static_cast<double>(id)};
            } catch (const std::exception& e) {
                throw;
            }
        }

        throw std::runtime_error("timers.nap invalid arguments at " + token.loc.to_string());
    };
    Token tNap;
    tNap.type = TokenType::IDENTIFIER;
    tNap.loc = TokenLocation("<timers>", 0, 0, 0);
    auto fn_nap = std::make_shared<FunctionValue>(std::string("native:timers.nap"), native_nap, nullptr, tNap);
    obj->properties["nap"] = PropertyDescriptor{fn_nap, false, false, false, tNap};

    // timers.queueMicrotask(cb, ...args)
    auto native_queueMicrotask = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("timers.queueMicrotask requires callback at " + token.loc.to_string());
        if (!is_function_value(args[0])) throw std::runtime_error("timers.queueMicrotask first arg must be a function at " + token.loc.to_string());
        FunctionPtr cb = std::get<FunctionPtr>(args[0]);
        std::vector<Value> cb_args;
        for (size_t i = 1; i < args.size(); ++i) cb_args.push_back(args[i]);

        // Build the boxed payload and hand to the scheduler bridge that enqueues microtasks.
        // CallbackPayload is the same type used by enqueue_callback.
        CallbackPayload* box = new CallbackPayload(cb, cb_args);
        enqueue_microtask_global(static_cast<void*>(box));
        return std::monostate{};
    };
    Token tmicro;
    tmicro.type = TokenType::IDENTIFIER;
    tmicro.loc = TokenLocation("<timers>", 0, 0, 0);
    auto fn_micro = std::make_shared<FunctionValue>(std::string("native:timers.queueMicrotask"), native_queueMicrotask, nullptr, tmicro);
    obj->properties["queueMicrotask"] = PropertyDescriptor{fn_micro, false, false, false, tmicro};

    return obj;
}

// Check whether there are any active timers (used by the scheduler exit predicate)
bool async_timers_exist() {
    std::lock_guard<std::mutex> lk(g_timers_mutex);
    return !g_timers.empty();
}