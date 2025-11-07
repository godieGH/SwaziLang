#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "builtins.hpp"
#include "evaluator.hpp"
#include "Scheduler.hpp"
#include "AsyncBridge.hpp" // -> defines CallbackPayload

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
        // Drain scheduler (blocks until idle). Provide a predicate so scheduler can
        // exit when both macrotasks empty AND there are no active async timers.
        scheduler()->run_until_idle([]() {
            return async_timers_exist();
        });
        return;
    }

    return;
}

// Internal: spawn a timer worker thread for this entry (detached)
static void spawn_timer_thread(std::shared_ptr<TimerEntry> te) {
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
            // We intentionally do not try to access internal scheduler globals here.
            // Instead we will notify the scheduler via the public bridge below.
        }
        // Wake the evaluator/scheduler bridge so run_until_idle's predicate can re-check timers.
        // We create a small no-op CallbackPayload (null cb) that the global bridge will deliver
        // to the evaluator; the evaluator's callback runner is responsible for deleting the payload.
        CallbackPayload* wake = new CallbackPayload(nullptr, {});
        enqueue_callback_global(static_cast<void*>(wake));
    }).detach();
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
    spawn_timer_thread(te);
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
    if (te) te->cancelled.store(true);
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

// Builtin factory: make_async_exports (extended with tolerant timers and nap)
std::shared_ptr<ObjectValue> make_async_exports(EnvPtr /*env*/) {
    auto obj = std::make_shared<ObjectValue>();

    // Async.subiri(cb, ...args)
    auto native_subiri = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("Async.subiri requires callback at " + token.loc.to_string());
        if (!is_function_value(args[0])) throw std::runtime_error("Async.subiri first arg must be a function at " + token.loc.to_string());
        FunctionPtr cb = std::get<FunctionPtr>(args[0]);
        std::vector<Value> cb_args;
        for (size_t i = 1; i < args.size(); ++i) cb_args.push_back(args[i]);
        enqueue_callback(cb, cb_args);
        return std::monostate{};
    };
    Token tsub;
    tsub.type = TokenType::IDENTIFIER;
    tsub.loc = TokenLocation("<async>", 0, 0, 0);
    auto fn_sub = std::make_shared<FunctionValue>(std::string("native:async.subiri"), native_subiri, nullptr, tsub);
    obj->properties["subiri"] = PropertyDescriptor{fn_sub, false, false, false, tsub};

    // Async.setTimeout(ms, cb) or (cb, ms)
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
    tSet.loc = TokenLocation("<async>", 0, 0, 0);
    auto fn_set = std::make_shared<FunctionValue>(std::string("native:async.setTimeout"), native_setTimeout, nullptr, tSet);
    obj->properties["setTimeout"] = PropertyDescriptor{fn_set, false, false, false, tSet};

    // Async.clearTimeout(id)
    auto native_clearTimeout = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("Async.clearTimeout requires id at " + token.loc.to_string());
        if (!std::holds_alternative<double>(args[0])) throw std::runtime_error("Async.clearTimeout id must be number at " + token.loc.to_string());
        long long id = static_cast<long long>(std::get<double>(args[0]));
        cancel_timer(id);
        return std::monostate{};
    };
    Token tClear;
    tClear.type = TokenType::IDENTIFIER;
    tClear.loc = TokenLocation("<async>", 0, 0, 0);
    auto fn_clear = std::make_shared<FunctionValue>(std::string("native:async.clearTimeout"), native_clearTimeout, nullptr, tClear);
    obj->properties["clearTimeout"] = PropertyDescriptor{fn_clear, false, false, false, tClear};

    // Async.setInterval(ms, cb) or (cb, ms)
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
    tInt.loc = TokenLocation("<async>", 0, 0, 0);
    auto fn_int = std::make_shared<FunctionValue>(std::string("native:async.setInterval"), native_setInterval, nullptr, tInt);
    obj->properties["setInterval"] = PropertyDescriptor{fn_int, false, false, false, tInt};

    // Async.clearInterval(id) -> same as clearTimeout
    obj->properties["clearInterval"] = obj->properties["clearTimeout"];

    // Async.nap(ms, cb?) — accept either (ms, cb) or (cb, ms) or (ms) (no cb).
    auto native_nap = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw std::runtime_error("Async.nap requires at least ms argument at " + token.loc.to_string());

        // If single numeric arg: schedule timer with no callback and return id
        if (args.size() == 1 && std::holds_alternative<double>(args[0])) {
            long long ms = value_to_ms(args[0]);
            long long id = create_timer(ms, 0, nullptr, {});
            return Value{static_cast<double>(id)};
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

        throw std::runtime_error("Async.nap invalid arguments at " + token.loc.to_string());
    };
    Token tNap;
    tNap.type = TokenType::IDENTIFIER;
    tNap.loc = TokenLocation("<async>", 0, 0, 0);
    auto fn_nap = std::make_shared<FunctionValue>(std::string("native:async.nap"), native_nap, nullptr, tNap);
    obj->properties["nap"] = PropertyDescriptor{fn_nap, false, false, false, tNap};

    return obj;
}

// Check whether there are any active timers (used by the scheduler exit predicate)
bool async_timers_exist() {
    std::lock_guard<std::mutex> lk(g_timers_mutex);
    return !g_timers.empty();
}