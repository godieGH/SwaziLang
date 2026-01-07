#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "./uv.hpp"
#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "ThreadWorkerContext.hpp"
#include "evaluator.hpp"
#include "uv.h"

namespace fs = std::filesystem;

// Helper to coerce Value -> string
static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Helper: create native function
template <typename F>
static FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    return std::make_shared<FunctionValue>(name, native_impl, env, Token());
}

// Worker thread state
struct WorkerThread {
    long long id;
    std::unique_ptr<std::thread> thread;
    std::atomic<bool> should_stop{false};
    std::atomic<bool> running{false};

    std::mutex inbox_mutex;
    std::queue<ThreadMessage> inbox;
    std::condition_variable inbox_cv;

    std::mutex outbox_mutex;
    std::queue<ThreadMessage> outbox;

    FunctionPtr message_callback;
    FunctionPtr error_callback;
    std::mutex callback_mutex;

    // Worker context
    std::unique_ptr<ThreadWorkerContext> context;

    // Store function/script for execution
    FunctionPtr worker_fn;
    std::vector<Value> worker_args;
    std::string worker_script;
    bool is_script_mode = false;
};

// Global worker registry
static std::mutex g_workers_mutex;
static std::unordered_map<long long, std::shared_ptr<WorkerThread>> g_workers;

std::shared_ptr<ObjectValue> make_threads_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // threads.hardwareConcurrency() -> number
    {
        auto fn = make_native_fn("threads.hardwareConcurrency", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            unsigned int n = std::thread::hardware_concurrency();
            return Value{static_cast<double>(n == 0 ? 1 : n)}; }, env);
        obj->properties["hardwareConcurrency"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // threads.currentId() -> number (thread ID)
    {
        auto fn = make_native_fn("threads.currentId", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            std::ostringstream ss;
            ss << std::this_thread::get_id();
            return Value{ss.str()}; }, env);
        obj->properties["currentId"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // threads.worker(script, data?) -> worker object
    // Creates a new OS thread running the provided script
    {
        auto fn = make_native_fn("threads.worker", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", 
                "threads.worker requires script path (string)", token.loc);
        }
        
        std::string script_path = std::get<std::string>(args[0]);
        
        if (!fs::exists(script_path)) {
            throw SwaziError("Error", 
                "Worker script not found: " + script_path, token.loc);
        }
        
        // Capture workerData from args[1] if provided
        Value worker_data = std::monostate{};
        if (args.size() >= 2) {
            worker_data = args[1];
        }
        
        auto worker = std::make_shared<WorkerThread>();
        worker->id = g_next_worker_id.fetch_add(1);
        worker->worker_script = script_path;
        worker->is_script_mode = true;
        
        {
            std::lock_guard<std::mutex> lock(g_workers_mutex);
            g_workers[worker->id] = worker;
        }
        
        // Spawn worker thread - capture worker_data
        worker->thread = std::make_unique<std::thread>([worker, worker_data]() {
            worker->running.store(true);
            
            try {
                worker->context = std::make_unique<ThreadWorkerContext>(worker->id);
                
                // Connect queues
                worker->context->inbound_queue_ = &worker->inbox;
                worker->context->outbound_queue_ = &worker->outbox;
                
                // Set workerData before executing script
                if (!std::holds_alternative<std::monostate>(worker_data)) {
                    worker->context->set_worker_data(worker_data);
                }
                
                // Execute the worker script
                worker->context->execute_script(worker->worker_script);
                
                // Message processing loop
                while (worker->running.load() && !worker->should_stop.load()) {
                    std::unique_lock<std::mutex> lock(worker->inbox_mutex);
                    worker->inbox_cv.wait_for(lock, std::chrono::milliseconds(100), 
                        [&]() {
                            return !worker->inbox.empty() || worker->should_stop.load();
                        });
                    
                    while (!worker->inbox.empty()) {
                        ThreadMessage msg = worker->inbox.front();
                        worker->inbox.pop();
                        lock.unlock();
                        
                        // Invoke message handler in worker context
                        Value handler = worker->context->get_global("__messageHandler__");
                        if (std::holds_alternative<FunctionPtr>(handler)) {
                            try {
                                Value msg_val;
                                if (msg.is_binary) {
                                    auto buf = std::make_shared<BufferValue>();
                                    buf->data = msg.binary_data;
                                    msg_val = buf;
                                } else {
                                    msg_val = msg.data;
                                }
                                
                                Token dummy;
                                worker->context->get_evaluator()->invoke_function(
                                    std::get<FunctionPtr>(handler), 
                                    {msg_val}, 
                                    worker->context->get_global_env(), 
                                    dummy);
                            } catch (const std::exception&) {
                                // Silently ignore handler errors
                            }
                        }
                        
                        lock.lock();
                    }
                }
                
                // Small delay before marking as not running to allow final messages to be polled
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                
            } catch (const std::exception& e) {
                // Send error to main thread
                ThreadMessage error_msg;
                error_msg.is_binary = false;
                error_msg.data = std::string("__ERROR__:") + e.what();
                
                {
                    std::lock_guard<std::mutex> lock(worker->outbox_mutex);
                    worker->outbox.push(error_msg);
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            
            worker->running.store(false);
        });
        
        // Set up polling timer for message passing
        uv_loop_t* loop = scheduler_get_loop();
        if (loop) {
            struct Poller {
                uv_timer_t timer;
                std::weak_ptr<WorkerThread> worker;
            };
            
            Poller* p = new Poller();
            p->worker = worker;
            p->timer.data = p;
            
            uv_timer_init(loop, &p->timer);
            uv_timer_start(&p->timer, 
                [](uv_timer_t* h) {
                    Poller* p = static_cast<Poller*>(h->data);
                    auto w = p->worker.lock();
                    if (!w || !w->running.load()) {
                        uv_timer_stop(h);
                        uv_close((uv_handle_t*)h, [](uv_handle_t* handle) {
                            delete static_cast<Poller*>(handle->data);
                        });
                        return;
                    }
                    
                    std::vector<ThreadMessage> msgs;
                    {
                        std::lock_guard<std::mutex> lock(w->outbox_mutex);
                        while (!w->outbox.empty()) {
                            msgs.push_back(w->outbox.front());
                            w->outbox.pop();
                        }
                    }
                    
                    FunctionPtr msg_cb;
                    FunctionPtr err_cb;
                    {
                        std::lock_guard<std::mutex> lock(w->callback_mutex);
                        msg_cb = w->message_callback;
                        err_cb = w->error_callback;
                    }
                    
                    if (!msgs.empty()) {
                        for (const auto& msg : msgs) {
                            // Check if this is an error message
                            if (!msg.is_binary) {
                                // msg.data is already a string, not a variant
                                if (msg.data.substr(0, 10) == "__ERROR__:") {
                                    // This is an error
                                    if (err_cb) {
                                        std::string error_text = msg.data.substr(10);
                                        enqueue_callback_global(new CallbackPayload(err_cb, {Value{error_text}}));
                                    }
                                    continue;
                                }
                            }
                            
                            // Regular message
                            if (msg_cb) {
                                Value val = msg.is_binary ? 
                                    Value{std::make_shared<BufferValue>(BufferValue{msg.binary_data, "binary"})} : 
                                    msg.data;
                                enqueue_callback_global(new CallbackPayload(msg_cb, {val}));
                            }
                        }
                    }
                }, 50, 50);
        }

        // Create control object
        auto control = std::make_shared<ObjectValue>();
        Token tok;
        
        control->properties["id"] = PropertyDescriptor{
            Value{static_cast<double>(worker->id)}, false, false, true, tok};
        
        auto post_fn = make_native_fn("worker.postMessage", 
            [worker](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("TypeError", "postMessage requires data", token.loc);
                }
                
                ThreadMessage msg;
                if (std::holds_alternative<std::string>(args[0])) {
                    msg.is_binary = false;
                    msg.data = std::get<std::string>(args[0]);
                } else if (std::holds_alternative<BufferPtr>(args[0])) {
                    msg.is_binary = true;
                    msg.binary_data = std::get<BufferPtr>(args[0])->data;
                } else {
                    throw SwaziError("TypeError", "postMessage requires string or Buffer", token.loc);
                }
                
                {
                    std::lock_guard<std::mutex> lock(worker->inbox_mutex);
                    worker->inbox.push(msg);
                }
                worker->inbox_cv.notify_one();
                return std::monostate{};
            }, nullptr);
        
        control->properties["postMessage"] = PropertyDescriptor{post_fn, false, false, false, tok};
        
        // worker.on(event, callback) -> undefined
        auto on_fn = make_native_fn("worker.on", 
            [worker](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.size() < 2) {
                    throw SwaziError("TypeError", "on requires (event, callback)", token.loc);
                }
                
                if (!std::holds_alternative<std::string>(args[0])) {
                    throw SwaziError("TypeError", "on requires event name as string", token.loc);
                }
                
                if (!std::holds_alternative<FunctionPtr>(args[1])) {
                    throw SwaziError("TypeError", "on requires callback function", token.loc);
                }
                
                std::string event = std::get<std::string>(args[0]);
                FunctionPtr callback = std::get<FunctionPtr>(args[1]);
                
                {
                    std::lock_guard<std::mutex> lock(worker->callback_mutex);
                    if (event == "message") {
                        worker->message_callback = callback;
                    } else if (event == "error") {
                        worker->error_callback = callback;
                    } else {
                        throw SwaziError("TypeError", 
                            "Unknown event type: " + event + ". Supported: 'message', 'error'", 
                            token.loc);
                    }
                }
                
                return std::monostate{};
            }, nullptr);
        
        control->properties["on"] = PropertyDescriptor{on_fn, false, false, false, tok};
        
        auto terminate_fn = make_native_fn("worker.terminate", 
            [worker](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                // Signal worker to stop
                worker->should_stop.store(true);
                worker->inbox_cv.notify_all();
                
                // Don't join synchronously - schedule it asynchronously
                if (worker->thread && worker->thread->joinable()) {
                    // Move thread ownership to a detached cleanup thread
                    std::thread([worker]() {
                        if (worker->thread && worker->thread->joinable()) {
                            worker->thread->join();
                        }
                        
                        // Remove from registry after join completes
                        {
                            std::lock_guard<std::mutex> lock(g_workers_mutex);
                            g_workers.erase(worker->id);
                        }
                    }).detach();
                }
                
                return std::monostate{};
            }, nullptr);
        control->properties["terminate"] = PropertyDescriptor{terminate_fn, false, false, false, tok};
        
        auto is_running_fn = make_native_fn("worker.isRunning", 
            [worker](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                return Value{worker->running.load()};
            }, nullptr);
        
        control->properties["isRunning"] = PropertyDescriptor{is_running_fn, false, false, false, tok};
        
        return Value{control}; }, env);

        obj->properties["worker"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // threads.processEvents(ms) -> undefined
    {
        auto fn = make_native_fn("threads.processEvents", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            unsigned int ms = 100;
            if (!args.empty() && std::holds_alternative<double>(args[0])) {
                ms = static_cast<unsigned int>(std::get<double>(args[0]));
            }
            
            Scheduler* sched = get_global_scheduler();
            if (!sched) {
                throw SwaziError("RuntimeError", "No scheduler available", token.loc);
            }
            
            auto start = std::chrono::steady_clock::now();
            while (true) {
                // Process scheduler tasks - THIS EXECUTES THE WORKER CALLBACKS!
                sched->run_one();
                
                // Also process UV events
                uv_run(sched->get_uv_loop(), UV_RUN_NOWAIT);
                
                auto elapsed = std::chrono::steady_clock::now() - start;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= ms) {
                    break;
                }
                
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            
            return std::monostate{}; }, env);
        obj->properties["processEvents"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}
