#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#else
#include <io.h>
#endif

// Global state for child IPC
static struct IPCState {
    bool initialized = false;
    bool is_forked_child = false;

    uv_pipe_t* read_pipe = nullptr;   // Read from fd 3 (parent sends)
    uv_pipe_t* write_pipe = nullptr;  // Write to fd 4 (child sends)

    std::mutex listeners_mutex;
    std::vector<FunctionPtr> message_listeners;

    std::string read_buffer;  // Accumulates incoming JSON messages
} g_ipc_state;

// Helper to schedule JS callback on runtime thread
static void schedule_message_listener(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// Helper to convert Value to string
static std::string value_to_string_ipc(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::string();
}

// Check if this process is a forked child
static bool detect_forked_child() {
    // Method 1: Check environment variable
    const char* ipc_env = std::getenv("SWAZI_IPC");
    if (ipc_env && std::strcmp(ipc_env, "1") == 0) {
        return true;
    }

    // Method 2: Check if fd 3 is open and valid (Unix only)
#ifndef _WIN32
    if (fcntl(3, F_GETFD) != -1) {
        return true;
    }
#endif

    return false;
}

// Allocate buffer for reading
static void alloc_ipc_cb(uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested);
    buf->len = static_cast<unsigned int>(suggested);
}

// Read callback for fd 3 (parent sends messages)
static void ipc_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        // Create buffer from raw bytes
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        // Fire message listeners with buffer
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
            listeners = g_ipc_state.message_listeners;
        }

        for (auto& cb : listeners) {
            if (cb) schedule_message_listener(cb, {Value{buffer}});
        }
    } else if (nread < 0) {
        uv_read_stop(stream);
    }

    if (buf && buf->base) free(buf->base);
}

// Initialize IPC in child process
static void initialize_child_ipc() {
    if (g_ipc_state.initialized) return;

    // Check if this is a forked child
    if (!detect_forked_child()) {
        g_ipc_state.is_forked_child = false;
        g_ipc_state.initialized = true;
        return;
    }

    g_ipc_state.is_forked_child = true;

    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) {
        g_ipc_state.initialized = true;
        return;  // No scheduler - can't do async I/O
    }

    // Open fd 3 (read from parent) as a pipe
    g_ipc_state.read_pipe = new uv_pipe_t;
    uv_pipe_init(loop, g_ipc_state.read_pipe, 0);

#ifndef _WIN32
    // Unix: open existing fd 3
    int r = uv_pipe_open(g_ipc_state.read_pipe, 3);
    if (r != 0) {
        delete g_ipc_state.read_pipe;
        g_ipc_state.read_pipe = nullptr;
    } else {
        // Start reading messages from parent
        uv_read_start((uv_stream_t*)g_ipc_state.read_pipe, alloc_ipc_cb, ipc_read_cb);
    }
#else
    // Windows: would need handle passing (TODO)
    delete g_ipc_state.read_pipe;
    g_ipc_state.read_pipe = nullptr;
#endif

    // Open fd 4 (write to parent) as a pipe
    g_ipc_state.write_pipe = new uv_pipe_t;
    uv_pipe_init(loop, g_ipc_state.write_pipe, 0);

#ifndef _WIN32
    // Unix: open existing fd 4
    r = uv_pipe_open(g_ipc_state.write_pipe, 4);
    if (r != 0) {
        delete g_ipc_state.write_pipe;
        g_ipc_state.write_pipe = nullptr;
    }
#else
    // Windows: would need handle passing (TODO)
    delete g_ipc_state.write_pipe;
    g_ipc_state.write_pipe = nullptr;
#endif

    g_ipc_state.initialized = true;
}

// process.send(message) - child sends message to parent
static Value process_send(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    // Initialize IPC if not already done
    if (!g_ipc_state.initialized) {
        initialize_child_ipc();
    }

    // SILENT MODE: If not a forked child, just return (do nothing)
    if (!g_ipc_state.is_forked_child) {
        return std::monostate{};
    }

    if (args.empty()) {
        throw SwaziError("TypeError", "process.send() requires a message argument", token.loc);
    }

    if (!g_ipc_state.write_pipe) {
        // Silent failure - pipe not available
        return std::monostate{};
    }

    // Convert value to bytes
    std::vector<uint8_t> data_bytes;

    if (std::holds_alternative<std::string>(args[0])) {
        std::string str = std::get<std::string>(args[0]);
        data_bytes.assign(str.begin(), str.end());
    } else if (std::holds_alternative<double>(args[0])) {
        std::ostringstream ss;
        ss << std::get<double>(args[0]);
        std::string str = ss.str();
        data_bytes.assign(str.begin(), str.end());
    } else if (std::holds_alternative<bool>(args[0])) {
        std::string str = std::get<bool>(args[0]) ? "true" : "false";
        data_bytes.assign(str.begin(), str.end());
    } else if (std::holds_alternative<BufferPtr>(args[0])) {
        BufferPtr buf = std::get<BufferPtr>(args[0]);
        data_bytes = buf->data;
    } else {
        throw SwaziError("TypeError", "send() requires string, number, boolean, or buffer", token.loc);
    }

    if (data_bytes.empty()) {
        return std::monostate{};
    }

    // Send raw bytes
    uv_buf_t buf = uv_buf_init((char*)malloc(data_bytes.size()),
        static_cast<unsigned int>(data_bytes.size()));
    memcpy(buf.base, data_bytes.data(), data_bytes.size());

    uv_write_t* req = new uv_write_t;
    req->data = buf.base;

    uv_write(req, (uv_stream_t*)g_ipc_state.write_pipe, &buf, 1,
        [](uv_write_t* req, int status) {
            if (req->data) free(req->data);
            delete req;
        });

    return std::monostate{};
}

static struct SignalState {
    std::mutex mutex;
    std::unordered_map<std::string, std::vector<FunctionPtr>> signal_listeners;
    std::unordered_map<std::string, uv_signal_t*> signal_handles;
} g_signal_state;

// Signal callback
static void signal_cb(uv_signal_t* handle, int signum) {
    std::string sig_name = "SIG" + std::to_string(signum);

    // Map common signal numbers to names
    if (signum == SIGTERM)
        sig_name = "SIGTERM";
    else if (signum == SIGINT)
        sig_name = "SIGINT";
#ifndef _WIN32
    else if (signum == SIGHUP)
        sig_name = "SIGHUP";
    else if (signum == SIGUSR1)
        sig_name = "SIGUSR1";
    else if (signum == SIGUSR2)
        sig_name = "SIGUSR2";
#endif

    std::vector<FunctionPtr> listeners;
    {
        std::lock_guard<std::mutex> lk(g_signal_state.mutex);
        auto it = g_signal_state.signal_listeners.find(sig_name);
        if (it != g_signal_state.signal_listeners.end()) {
            listeners = it->second;
        }
    }

    for (auto& cb : listeners) {
        if (cb) schedule_message_listener(cb, {Value{sig_name}});
    }
}

// process.on(event, callback) - register listener for IPC messages
static Value process_on_message(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    if (args.size() < 2) {
        throw SwaziError("TypeError",
            "process.on() requires two arguments: event name and callback", token.loc);
    }

    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "Event name must be a string", token.loc);
    }

    if (!std::holds_alternative<FunctionPtr>(args[1])) {
        throw SwaziError("TypeError", "Callback must be a function", token.loc);
    }

    std::string event = std::get<std::string>(args[0]);
    FunctionPtr callback = std::get<FunctionPtr>(args[1]);

    // Handle "message" event (IPC)
    if (event == "message") {
        // Initialize IPC if not already done
        if (!g_ipc_state.initialized) {
            initialize_child_ipc();
        }

        // SILENT MODE: If not a forked child, just return (do nothing)
        if (!g_ipc_state.is_forked_child) {
            return std::monostate{};
        }

        // Register the listener
        {
            std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
            g_ipc_state.message_listeners.push_back(callback);
        }

        return std::monostate{};
    }

    // Handle signal events (SIGTERM, SIGINT, etc.)
    int signum = -1;
    if (event == "SIGTERM")
        signum = SIGTERM;
    else if (event == "SIGINT")
        signum = SIGINT;
#ifndef _WIN32
    else if (event == "SIGHUP")
        signum = SIGHUP;
    else if (event == "SIGUSR1")
        signum = SIGUSR1;
    else if (event == "SIGUSR2")
        signum = SIGUSR2;
#endif

    if (signum != -1) {
        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            // No loop - can't register signal handlers
            return std::monostate{};
        }

        std::lock_guard<std::mutex> lk(g_signal_state.mutex);

        // Register listener
        g_signal_state.signal_listeners[event].push_back(callback);

        // Create signal handle if not exists
        if (g_signal_state.signal_handles.find(event) == g_signal_state.signal_handles.end()) {
            uv_signal_t* sig_handle = new uv_signal_t;
            uv_signal_init(loop, sig_handle);
            uv_signal_start(sig_handle, signal_cb, signum);
            g_signal_state.signal_handles[event] = sig_handle;
        }

        return std::monostate{};
    }

    // Unknown event - silently ignore
    return std::monostate{};
}

Value process_send_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_send(args, env, token);
}

Value process_on_message_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_on_message(args, env, token);
}