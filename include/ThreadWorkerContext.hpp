#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

struct ThreadMessage {
    std::string data;
    bool is_binary;
    std::vector<uint8_t> binary_data;
};

/**
 * ThreadWorkerContext - Isolated execution context for worker threads
 *
 * Provides a complete Swazi runtime environment for a worker thread,
 * including its own Evaluator, environment, and message passing interface.
 */
class ThreadWorkerContext {
   public:
    ThreadWorkerContext(long long worker_id);
    ~ThreadWorkerContext();

    // Execute a function with arguments
    void execute_function(FunctionPtr fn, const std::vector<Value>& args);

    // Execute a script file
    void execute_script(const std::string& script_path);

    // Set worker data (available as global `workerData`)
    void set_worker_data(const Value& data);

    // Set command line arguments (available as global `argv`)
    void set_argv(const std::vector<std::string>& args);

    // Get the worker's evaluator (for advanced usage)
    Evaluator* get_evaluator() { return evaluator_.get(); }

    // Get the worker's global environment
    EnvPtr get_global_env();

    // Set a global variable in the worker
    void set_global(const std::string& name, const Value& value, bool is_constant = false);

    // Get a global variable from the worker
    Value get_global(const std::string& name);

    // Message queue access (thread-safe)
    void push_outbound_message(const ThreadMessage& msg);
    bool pop_inbound_message(ThreadMessage& msg);

    // Register message handler (called when messages arrive from main thread)
    void set_message_handler(FunctionPtr handler);

    // Message queues (shared with worker thread structure)
    std::mutex inbound_mutex_;
    std::queue<ThreadMessage>* inbound_queue_;

    std::mutex outbound_mutex_;
    std::queue<ThreadMessage>* outbound_queue_;

   private:
    long long worker_id_;
    std::unique_ptr<Evaluator> evaluator_;
    EnvPtr global_env_;

    FunctionPtr message_handler_;

    // Initialize the worker environment with special globals
    void initialize_worker_environment();

    // Create the parentPort object for communication
    ObjectPtr create_parent_port();
};