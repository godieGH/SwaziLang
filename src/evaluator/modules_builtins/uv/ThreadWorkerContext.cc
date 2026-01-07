#include "ThreadWorkerContext.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "SwaziError.hpp"
#include "globals.hpp"

#define WORKER_CTX_DEBUG(msg) std::cerr << "[WORKER_CTX] " << msg << std::endl

namespace fs = std::filesystem;

ThreadWorkerContext::ThreadWorkerContext(long long worker_id)
    : worker_id_(worker_id),
      evaluator_(std::make_unique<Evaluator>()),
      inbound_queue_(nullptr),
      outbound_queue_(nullptr) {
    initialize_worker_environment();
}

ThreadWorkerContext::~ThreadWorkerContext() = default;

void ThreadWorkerContext::initialize_worker_environment() {
    // Get access to evaluator's global environment
    // We'll use a friend declaration or public accessor
    global_env_ = evaluator_->global_env;  // Assuming we add public accessor

    if (!global_env_) {
        throw std::runtime_error("Failed to access worker global environment");
    }

    // Create and inject parentPort object
    ObjectPtr parent_port = create_parent_port();
    Environment::Variable parent_port_var;
    parent_port_var.value = parent_port;
    parent_port_var.is_constant = true;
    global_env_->set("parentPort", parent_port_var);

    // Set worker-specific metadata
    Environment::Variable is_worker_var;
    is_worker_var.value = true;
    is_worker_var.is_constant = true;
    global_env_->set("__isWorker__", is_worker_var);

    Environment::Variable worker_id_var;
    worker_id_var.value = static_cast<double>(worker_id_);
    worker_id_var.is_constant = true;
    global_env_->set("__workerId__", worker_id_var);
}

ObjectPtr ThreadWorkerContext::create_parent_port() {
    auto parent_port = std::make_shared<ObjectValue>();
    Token tok;

    // parentPort.postMessage(data)
    auto post_message_fn = std::make_shared<FunctionValue>(
        "parentPort.postMessage",
        [this](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "postMessage requires data argument", token.loc);
            }

            ThreadMessage msg;
            const Value& data = args[0];

            if (std::holds_alternative<std::string>(data)) {
                msg.is_binary = false;
                msg.data = std::get<std::string>(data);
            } else if (std::holds_alternative<BufferPtr>(data)) {
                msg.is_binary = true;
                msg.binary_data = std::get<BufferPtr>(data)->data;
            } else {
                // Serialize to string representation
                msg.is_binary = false;
                msg.data = evaluator_->value_to_string(data);
            }

            push_outbound_message(msg);
            return std::monostate{};
        },
        nullptr,
        tok);

    parent_port->properties["postMessage"] =
        PropertyDescriptor{post_message_fn, false, false, false, tok};

    // parentPort.on(event, callback)
    auto on_fn = std::make_shared<FunctionValue>(
        "parentPort.on",
        [this](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "on requires (event, callback)", token.loc);
            }

            std::string event = "message";
            if (std::holds_alternative<std::string>(args[0])) {
                event = std::get<std::string>(args[0]);
            }

            if (event != "message") {
                return std::monostate{};  // Only support 'message' for now
            }

            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "callback must be a function", token.loc);
            }

            set_global("__messageHandler__", args[1], false);
            set_message_handler(std::get<FunctionPtr>(args[1]));
            return std::monostate{};
        },
        nullptr,
        tok);

    parent_port->properties["on"] =
        PropertyDescriptor{on_fn, false, false, false, tok};

    return parent_port;
}

void ThreadWorkerContext::execute_function(FunctionPtr fn, const std::vector<Value>& args) {
    if (!fn) {
        throw std::runtime_error("Cannot execute null function");
    }

    try {
        Token call_token;
        evaluator_->invoke_function(fn, args, global_env_, call_token);

        evaluator_->run_event_loop();
    } catch (const std::exception& e) {
        ThreadMessage error_msg;
        error_msg.is_binary = false;
        error_msg.data = std::string("Worker error: ") + e.what();
        push_outbound_message(error_msg);
    }
}
void ThreadWorkerContext::execute_script(const std::string& script_path) {
    if (!fs::exists(script_path)) {
        throw std::runtime_error("Worker script not found: " + script_path);
    }

    try {
        // Read script file
        std::ifstream file(script_path);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open worker script: " + script_path);
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // Parse the script (use correct Lexer constructor signature)
        Lexer lexer(source, script_path);  // CHANGED: pass filename as second arg
        auto tokens = lexer.tokenize();

        Parser parser(tokens);
        auto program = parser.parse();  // CHANGED: use parse() not parse_program()

        // Set entry point for proper __main__, __file__, etc.
        evaluator_->set_entry_point(script_path);

        // Evaluate the program
        evaluator_->evaluate(program.get());

        // Run event loop
        evaluator_->run_event_loop();

    } catch (const std::exception& e) {
        ThreadMessage error_msg;
        error_msg.is_binary = false;
        error_msg.data = std::string("Worker script error: ") + e.what();
        push_outbound_message(error_msg);
        throw;
    }
}
void ThreadWorkerContext::set_worker_data(const Value& data) {
    Environment::Variable var;
    var.value = data;
    var.is_constant = true;
    global_env_->set("workerData", var);
}

void ThreadWorkerContext::set_argv(const std::vector<std::string>& args) {
    evaluator_->set_cli_args(args);
}

EnvPtr ThreadWorkerContext::get_global_env() {
    return global_env_;
}

void ThreadWorkerContext::set_global(const std::string& name, const Value& value, bool is_constant) {
    Environment::Variable var;
    var.value = value;
    var.is_constant = is_constant;
    global_env_->set(name, var);
}

Value ThreadWorkerContext::get_global(const std::string& name) {
    if (global_env_->has(name)) {
        return global_env_->get(name).value;
    }
    return std::monostate{};
}

void ThreadWorkerContext::push_outbound_message(const ThreadMessage& msg) {
    if (!outbound_queue_) {
        return;
    }

    std::lock_guard<std::mutex> lock(outbound_mutex_);
    outbound_queue_->push(msg);
}
bool ThreadWorkerContext::pop_inbound_message(ThreadMessage& msg) {
    if (!inbound_queue_) return false;

    std::lock_guard<std::mutex> lock(inbound_mutex_);
    if (inbound_queue_->empty()) return false;

    msg = inbound_queue_->front();
    inbound_queue_->pop();
    return true;
}

void ThreadWorkerContext::set_message_handler(FunctionPtr handler) {
    message_handler_ = handler;
}