// src/evaluator/modules_builtins/threads.cpp
// Threading module with worker creation and mutex locking
// Enforces maximum of 2 worker threads (main + 2 = 3 total)

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

// Thread-safe shared value store with mutex locking
struct SharedValueStore {
    std::map<std::string, Value> values;
    std::map<std::string, std::unique_ptr<std::mutex>> mutexes;
    std::mutex store_mutex;

    void ensure_mutex(const std::string& key) {
        std::lock_guard<std::mutex> lk(store_mutex);
        if (mutexes.find(key) == mutexes.end()) {
            mutexes[key] = std::make_unique<std::mutex>();
        }
    }

    void set(const std::string& key, const Value& val) {
        std::lock_guard<std::mutex> lk(store_mutex);
        values[key] = val;
        ensure_mutex(key);
    }

    Value get(const std::string& key) {
        std::lock_guard<std::mutex> lk(store_mutex);
        auto it = values.find(key);
        if (it != values.end()) return it->second;
        return std::monostate{};
    }

    void lock(const std::string& key) {
        ensure_mutex(key);
        mutexes[key]->lock();
    }

    void unlock(const std::string& key) {
        std::lock_guard<std::mutex> lk(store_mutex);
        auto it = mutexes.find(key);
        if (it != mutexes.end()) {
            it->second->unlock();
        }
    }
};

static std::shared_ptr<SharedValueStore> g_shared_store = std::make_shared<SharedValueStore>();

// Worker thread bookkeeping
struct WorkerEntry {
    long long id;
    std::thread thread;
    std::atomic<bool> finished{false};
    Value result;
    bool detached = false;
};

static std::mutex g_workers_mutex;
static std::atomic<long long> g_next_worker_id{1};
static std::atomic<int> g_active_workers{0};
static std::map<long long, std::shared_ptr<WorkerEntry>> g_workers;

// Maximum worker limit (main thread + 2 workers = 3 total)
static constexpr int MAX_WORKERS = 2;

// Helper to create native function
static Token make_thread_token(const std::string& name) {
    Token t;
    t.type = TokenType::IDENTIFIER;
    t.loc = TokenLocation("<threads>", 0, 0, 0);
    return t;
}

static std::string value_to_string_local(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// threads.worker(fn, ...args) -> worker object
static Value native_worker(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
        throw SwaziError("TypeError", "threads.worker requires a function as first argument", token.loc);
    }

    // Check worker limit
    int current = g_active_workers.load();
    if (current >= MAX_WORKERS) {
        throw SwaziError("ThreadError", 
            "Maximum worker limit reached (2 workers max). Wait for existing workers to complete.", 
            token.loc);
    }

    FunctionPtr fn = std::get<FunctionPtr>(args[0]);
    std::vector<Value> fn_args;
    for (size_t i = 1; i < args.size(); ++i) {
        fn_args.push_back(args[i]);
    }

    auto entry = std::make_shared<WorkerEntry>();
    entry->id = g_next_worker_id.fetch_add(1);

    // Increment active worker count
    g_active_workers.fetch_add(1);

    // Create worker thread that executes the function
    entry->thread = std::thread([entry, fn, fn_args, env]() {
        try {
            // Execute function in worker thread context
            // Note: This is simplified - in production you'd need a proper evaluator instance
            // For now we'll just mark it as completed
            entry->result = std::monostate{}; // Placeholder - would call evaluator
            entry->finished.store(true);
        } catch (const std::exception& e) {
            entry->result = Value{std::string("Worker error: ") + e.what()};
            entry->finished.store(true);
        } catch (...) {
            entry->result = Value{std::string("Unknown worker error")};
            entry->finished.store(true);
        }
    });

    // Register worker
    {
        std::lock_guard<std::mutex> lk(g_workers_mutex);
        g_workers[entry->id] = entry;
    }

    // Create worker object with API methods
    auto worker_obj = std::make_shared<ObjectValue>();

    // worker.join() -> waits for completion and returns result
    auto join_impl = [entry](const std::vector<Value>&, EnvPtr, const Token& tok) -> Value {
        if (entry->detached) {
            throw SwaziError("ThreadError", "Cannot join a detached worker", tok.loc);
        }
        if (entry->thread.joinable()) {
            entry->thread.join();
            g_active_workers.fetch_sub(1);
        }
        return entry->result;
    };
    Token tok_join = make_thread_token("worker.join");
    auto fn_join = std::make_shared<FunctionValue>("native:worker.join", join_impl, nullptr, tok_join);
    worker_obj->properties["join"] = PropertyDescriptor{fn_join, false, false, false, tok_join};

    // worker.detach() -> detaches thread (fire and forget)
    auto detach_impl = [entry](const std::vector<Value>&, EnvPtr, const Token& tok) -> Value {
        if (entry->detached) {
            throw SwaziError("ThreadError", "Worker already detached", tok.loc);
        }
        if (entry->thread.joinable()) {
            entry->thread.detach();
            entry->detached = true;
            // Schedule cleanup when finished
            scheduler_run_on_loop([entry]() {
                if (entry->finished.load()) {
                    g_active_workers.fetch_sub(1);
                }
            });
        }
        return std::monostate{};
    };
    Token tok_detach = make_thread_token("worker.detach");
    auto fn_detach = std::make_shared<FunctionValue>("native:worker.detach", detach_impl, nullptr, tok_detach);
    worker_obj->properties["detach"] = PropertyDescriptor{fn_detach, false, false, false, tok_detach};

    // worker.isFinished() -> bool
    auto finished_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{entry->finished.load()};
    };
    Token tok_finished = make_thread_token("worker.isFinished");
    auto fn_finished = std::make_shared<FunctionValue>("native:worker.isFinished", finished_impl, nullptr, tok_finished);
    worker_obj->properties["isFinished"] = PropertyDescriptor{fn_finished, false, false, false, tok_finished};

    return worker_obj;
}

// threads.lock(key) -> acquires mutex for shared value
static Value native_lock(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "threads.lock requires a key (variable name)", token.loc);
    }
    std::string key = value_to_string_local(args[0]);
    g_shared_store->lock(key);
    return std::monostate{};
}

// threads.unlock(key) -> releases mutex for shared value
static Value native_unlock(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "threads.unlock requires a key (variable name)", token.loc);
    }
    std::string key = value_to_string_local(args[0]);
    g_shared_store->unlock(key);
    return std::monostate{};
}

// threads.setShared(key, value) -> stores value in shared store
static Value native_set_shared(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "threads.setShared requires (key, value)", token.loc);
    }
    std::string key = value_to_string_local(args[0]);
    g_shared_store->set(key, args[1]);
    return std::monostate{};
}

// threads.getShared(key) -> retrieves value from shared store
static Value native_get_shared(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "threads.getShared requires a key", token.loc);
    }
    std::string key = value_to_string_local(args[0]);
    return g_shared_store->get(key);
}

// threads.sleep(ms) -> sleeps current thread
static Value native_thread_sleep(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "threads.sleep requires milliseconds", token.loc);
    }
    long long ms = static_cast<long long>(std::holds_alternative<double>(args[0]) ? std::get<double>(args[0]) : 0);
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    return std::monostate{};
}

// threads.activeCount() -> returns number of active workers
static Value native_active_count(const std::vector<Value>&, EnvPtr, const Token&) {
    return Value{static_cast<double>(g_active_workers.load())};
}

// Export factory
std::shared_ptr<ObjectValue> make_threads_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok = make_thread_token("threads");

    auto make_fn = [&](const std::string& name, 
                       std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, tok);
        obj->properties[name] = PropertyDescriptor{fn, false, false, false, tok};
    };

    make_fn("worker", native_worker);
    make_fn("lock", native_lock);
    make_fn("unlock", native_unlock);
    make_fn("setShared", native_set_shared);
    make_fn("getShared", native_get_shared);
    make_fn("sleep", native_thread_sleep);
    make_fn("activeCount", native_active_count);

    // Expose MAX_WORKERS constant
    obj->properties["MAX_WORKERS"] = PropertyDescriptor{Value{static_cast<double>(MAX_WORKERS)}, false, true, true, tok};

    return obj;
}