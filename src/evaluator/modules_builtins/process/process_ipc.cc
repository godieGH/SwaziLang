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

Value process_detach_impl(const std::vector<Value>& args, EnvPtr env, const Token& token);
Value process_ignore_signals_impl(const std::vector<Value>& args, EnvPtr env, const Token& token);

std::vector<SignalInfo> get_all_signals() {
    std::vector<SignalInfo> signals;

#ifndef _WIN32
    // Standard POSIX signals
    signals.push_back({"SIGINT", SIGINT, true, "Interrupt from keyboard (Ctrl+C)"});
    signals.push_back({"SIGTERM", SIGTERM, true, "Termination signal"});
    signals.push_back({"SIGHUP", SIGHUP, true, "Hangup detected on controlling terminal"});
    signals.push_back({"SIGQUIT", SIGQUIT, true, "Quit from keyboard (Ctrl+\\)"});
    signals.push_back({"SIGUSR1", SIGUSR1, true, "User-defined signal 1"});
    signals.push_back({"SIGUSR2", SIGUSR2, true, "User-defined signal 2"});
    signals.push_back({"SIGPIPE", SIGPIPE, true, "Broken pipe"});
    signals.push_back({"SIGALRM", SIGALRM, true, "Timer signal from alarm()"});
    signals.push_back({"SIGCHLD", SIGCHLD, true, "Child stopped or terminated"});
    signals.push_back({"SIGCONT", SIGCONT, true, "Continue if stopped"});
    signals.push_back({"SIGTSTP", SIGTSTP, true, "Stop typed at terminal (Ctrl+Z)"});
    signals.push_back({"SIGTTIN", SIGTTIN, true, "Terminal input for background process"});
    signals.push_back({"SIGTTOU", SIGTTOU, true, "Terminal output for background process"});
    signals.push_back({"SIGWINCH", SIGWINCH, true, "Window resize signal"});
    signals.push_back({"SIGURG", SIGURG, true, "Urgent condition on socket"});
    signals.push_back({"SIGXCPU", SIGXCPU, true, "CPU time limit exceeded"});
    signals.push_back({"SIGXFSZ", SIGXFSZ, true, "File size limit exceeded"});
    signals.push_back({"SIGVTALRM", SIGVTALRM, true, "Virtual alarm clock"});
    signals.push_back({"SIGPROF", SIGPROF, true, "Profiling timer expired"});

    // Uncatchable signals (for documentation)
    signals.push_back({"SIGKILL", SIGKILL, false, "Kill signal (uncatchable)"});
    signals.push_back({"SIGSTOP", SIGSTOP, false, "Stop process (uncatchable)"});

    // Dangerous signals (catchable but risky)
    signals.push_back({"SIGSEGV", SIGSEGV, true, "Invalid memory reference (dangerous to catch)"});
    signals.push_back({"SIGBUS", SIGBUS, true, "Bus error (dangerous to catch)"});
    signals.push_back({"SIGFPE", SIGFPE, true, "Floating-point exception (dangerous to catch)"});
    signals.push_back({"SIGILL", SIGILL, true, "Illegal instruction (dangerous to catch)"});
    signals.push_back({"SIGTRAP", SIGTRAP, true, "Trace/breakpoint trap (dangerous to catch)"});
    signals.push_back({"SIGABRT", SIGABRT, true, "Abort signal from abort()"});
    signals.push_back({"SIGSYS", SIGSYS, true, "Bad system call"});

#else  // Windows
    signals.push_back({"SIGINT", SIGINT, true, "Interrupt from keyboard (Ctrl+C)"});
    signals.push_back({"SIGTERM", SIGTERM, true, "Termination signal"});
    signals.push_back({"SIGBREAK", SIGBREAK, true, "Break signal (Ctrl+Break)"});
    signals.push_back({"SIGABRT", SIGABRT, true, "Abort signal from abort()"});
    signals.push_back({"SIGFPE", SIGFPE, true, "Floating-point exception"});
    signals.push_back({"SIGILL", SIGILL, true, "Illegal instruction"});
    signals.push_back({"SIGSEGV", SIGSEGV, true, "Invalid memory reference"});
#endif

    return signals;
}

// Helper: convert signal name or number to signal number
static int resolve_signal(const Value& v, const Token& token) {
    // If it's a number (signal code), validate and return
    if (std::holds_alternative<double>(v)) {
        int sig = static_cast<int>(std::get<double>(v));
#ifndef _WIN32
        if (sig < 1 || sig > 64) {  // Most systems support 1-64
            throw SwaziError("RangeError",
                "Signal number must be between 1 and 64", token.loc);
        }
#else
        // Windows only supports a handful
        if (sig != SIGINT && sig != SIGTERM && sig != SIGBREAK &&
            sig != SIGABRT && sig != SIGFPE && sig != SIGILL && sig != SIGSEGV) {
            throw SwaziError("ValueError",
                "Invalid signal number for Windows", token.loc);
        }
#endif
        return sig;
    }

    // If it's a string (signal name), look it up
    if (std::holds_alternative<std::string>(v)) {
        std::string sig_name = std::get<std::string>(v);

        // Add "SIG" prefix if missing
        if (sig_name.size() >= 2 && sig_name.substr(0, 3) != "SIG") {
            sig_name = "SIG" + sig_name;
        }

        // Look up in our signal table
        auto signals = get_all_signals();
        for (const auto& s : signals) {
            if (s.name == sig_name) {
                return s.number;
            }
        }

        throw SwaziError("ValueError",
            "Unknown signal name: " + std::get<std::string>(v), token.loc);
    }

    throw SwaziError("TypeError",
        "Signal must be a string name or numeric code", token.loc);
}

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

    std::vector<FunctionPtr> catch_all_listeners;
} g_signal_state;

// Signal callback
static void signal_cb(uv_signal_t* handle, int signum) {
    std::string sig_name = "SIG" + std::to_string(signum);

    // Map common signal numbers to names
    auto signals = get_all_signals();
    for (const auto& s : signals) {
        if (s.number == signum) {
            sig_name = s.name;
            break;
        }
    }

    std::vector<FunctionPtr> listeners;
    std::vector<FunctionPtr> catch_all;

    {
        std::lock_guard<std::mutex> lk(g_signal_state.mutex);

        // Get specific signal listeners
        auto it = g_signal_state.signal_listeners.find(sig_name);
        if (it != g_signal_state.signal_listeners.end()) {
            listeners = it->second;
        }

        // Get catch-all listeners
        catch_all = g_signal_state.catch_all_listeners;
    }

    // Fire specific signal listeners: callback(signalName)
    for (auto& cb : listeners) {
        if (cb) schedule_message_listener(cb, {Value{sig_name}});
    }

    // Fire catch-all "signal" listeners: callback(signalCode, signalName)
    for (auto& cb : catch_all) {
        if (cb) schedule_message_listener(cb, {
                                                  Value{static_cast<double>(signum)},  // code
                                                  Value{sig_name}                      // type/name
                                              });
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
        if (!g_ipc_state.initialized) {
            initialize_child_ipc();
        }
        if (!g_ipc_state.is_forked_child) {
            return std::monostate{};
        }

        std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
        g_ipc_state.message_listeners.push_back(callback);
        return std::monostate{};
    }

    // NEW: Handle "signal" event (catch-all for ALL signals)
    if (event == "signal") {
        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            return std::monostate{};
        }

        std::lock_guard<std::mutex> lk(g_signal_state.mutex);

        // Add to catch-all listeners
        g_signal_state.catch_all_listeners.push_back(callback);

        // Register ALL catchable signals if not already registered
        auto signals = get_all_signals();
        for (const auto& sig : signals) {
            if (!sig.catchable) continue;  // Skip SIGKILL, SIGSTOP

#ifdef _WIN32
            // Windows only supports a few signals
            if (sig.number != SIGINT && sig.number != SIGTERM &&
                sig.number != SIGBREAK && sig.number != SIGABRT) {
                continue;
            }
#endif

            // Create signal handle if not exists
            if (g_signal_state.signal_handles.find(sig.name) ==
                g_signal_state.signal_handles.end()) {
                uv_signal_t* sig_handle = new uv_signal_t;
                uv_signal_init(loop, sig_handle);
                uv_signal_start(sig_handle, signal_cb, sig.number);
                g_signal_state.signal_handles[sig.name] = sig_handle;
            }
        }

        return std::monostate{};
    }

    // Handle specific signal events (SIGTERM, SIGINT, etc.)
    // First try to resolve as a signal name/number
    int signum = -1;
    try {
        signum = resolve_signal(Value{event}, token);
    } catch (...) {
        // Not a valid signal, silently ignore unknown events
        return std::monostate{};
    }

    if (signum != -1) {
        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            return std::monostate{};
        }

        std::lock_guard<std::mutex> lk(g_signal_state.mutex);

        // Register listener
        g_signal_state.signal_listeners[event].push_back(callback);

        // Create signal handle if not exists
        if (g_signal_state.signal_handles.find(event) ==
            g_signal_state.signal_handles.end()) {
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

// process.off(event?, callback?) - remove listeners
// - process.off() -> remove ALL listeners for ALL events
// - process.off(event) -> remove all listeners for specific event
// - process.off(event, callback) -> remove specific callback
// - process.off([events...]) -> remove all listeners for listed events
static Value process_off(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    // Case 1: process.off() - remove ALL listeners
    if (args.empty()) {
        // Clear message listeners
        {
            std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
            g_ipc_state.message_listeners.clear();
        }

        // Clear all signal listeners and stop handles
        {
            std::lock_guard<std::mutex> lk(g_signal_state.mutex);
            g_signal_state.catch_all_listeners.clear();
            g_signal_state.signal_listeners.clear();

            // Stop and cleanup all signal handles
            for (auto& kv : g_signal_state.signal_handles) {
                uv_signal_stop(kv.second);
                uv_close((uv_handle_t*)kv.second, [](uv_handle_t* h) {
                    delete (uv_signal_t*)h;
                });
            }
            g_signal_state.signal_handles.clear();
        }

        return std::monostate{};
    }

    // Case 2: process.off([events...]) - array of events
    if (std::holds_alternative<ArrayPtr>(args[0])) {
        ArrayPtr arr = std::get<ArrayPtr>(args[0]);

        for (const auto& elem : arr->elements) {
            if (!std::holds_alternative<std::string>(elem)) continue;
            std::string event = std::get<std::string>(elem);

            // Remove all listeners for this event (recursive call)
            process_off({Value{event}}, nullptr, token);
        }

        return std::monostate{};
    }

    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError",
            "First argument must be event name (string) or array of event names", token.loc);
    }

    std::string event = std::get<std::string>(args[0]);

    // Case 3: process.off(event, callback) - remove specific callback
    if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
        FunctionPtr callback = std::get<FunctionPtr>(args[1]);

        // Handle "message" event
        if (event == "message") {
            std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
            auto& listeners = g_ipc_state.message_listeners;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&callback](const FunctionPtr& fn) { return fn == callback; }),
                listeners.end());
            return std::monostate{};
        }

        // Handle "signal" catch-all event
        if (event == "signal") {
            std::lock_guard<std::mutex> lk(g_signal_state.mutex);
            auto& listeners = g_signal_state.catch_all_listeners;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&callback](const FunctionPtr& fn) { return fn == callback; }),
                listeners.end());
            return std::monostate{};
        }

        // Handle specific signal
        int signum = -1;
        try {
            signum = resolve_signal(Value{event}, token);
        } catch (...) {
            return std::monostate{};
        }

        if (signum != -1) {
            std::lock_guard<std::mutex> lk(g_signal_state.mutex);
            auto it = g_signal_state.signal_listeners.find(event);
            if (it != g_signal_state.signal_listeners.end()) {
                auto& listeners = it->second;
                listeners.erase(
                    std::remove_if(listeners.begin(), listeners.end(),
                        [&callback](const FunctionPtr& fn) { return fn == callback; }),
                    listeners.end());

                // If no more listeners, stop the handle
                if (listeners.empty()) {
                    auto handle_it = g_signal_state.signal_handles.find(event);
                    if (handle_it != g_signal_state.signal_handles.end()) {
                        uv_signal_stop(handle_it->second);
                        uv_close((uv_handle_t*)handle_it->second, [](uv_handle_t* h) {
                            delete (uv_signal_t*)h;
                        });
                        g_signal_state.signal_handles.erase(handle_it);
                    }
                }
            }
        }

        return std::monostate{};
    }

    // Case 4: process.off(event) - remove ALL listeners for this event
    if (event == "message") {
        std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
        g_ipc_state.message_listeners.clear();
        return std::monostate{};
    }

    if (event == "signal") {
        std::lock_guard<std::mutex> lk(g_signal_state.mutex);
        g_signal_state.catch_all_listeners.clear();

        // Note: Don't stop individual signal handles as they might still have
        // specific listeners. Only clear the catch-all.
        return std::monostate{};
    }

    // Specific signal event
    int signum = -1;
    try {
        signum = resolve_signal(Value{event}, token);
    } catch (...) {
        return std::monostate{};
    }

    if (signum != -1) {
        std::lock_guard<std::mutex> lk(g_signal_state.mutex);

        // Clear listeners
        auto it = g_signal_state.signal_listeners.find(event);
        if (it != g_signal_state.signal_listeners.end()) {
            it->second.clear();
        }

        // Stop and cleanup the signal handle
        auto handle_it = g_signal_state.signal_handles.find(event);
        if (handle_it != g_signal_state.signal_handles.end()) {
            uv_signal_stop(handle_it->second);
            uv_close((uv_handle_t*)handle_it->second, [](uv_handle_t* h) {
                delete (uv_signal_t*)h;
            });
            g_signal_state.signal_handles.erase(handle_it);
        }
    }

    return std::monostate{};
}

// process.listeners(event?) - get count or list of listeners
static Value process_listeners(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    // If no event specified, return total count across all events
    if (args.empty()) {
        size_t total = 0;
        {
            std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
            total += g_ipc_state.message_listeners.size();
        }
        {
            std::lock_guard<std::mutex> lk(g_signal_state.mutex);
            total += g_signal_state.catch_all_listeners.size();
            for (const auto& kv : g_signal_state.signal_listeners) {
                total += kv.second.size();
            }
        }
        return Value{static_cast<double>(total)};
    }

    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "Event name must be a string", token.loc);
    }

    std::string event = std::get<std::string>(args[0]);

    if (event == "message") {
        std::lock_guard<std::mutex> lk(g_ipc_state.listeners_mutex);
        return Value{static_cast<double>(g_ipc_state.message_listeners.size())};
    }

    if (event == "signal") {
        std::lock_guard<std::mutex> lk(g_signal_state.mutex);
        return Value{static_cast<double>(g_signal_state.catch_all_listeners.size())};
    }

    // Specific signal
    std::lock_guard<std::mutex> lk(g_signal_state.mutex);
    auto it = g_signal_state.signal_listeners.find(event);
    if (it != g_signal_state.signal_listeners.end()) {
        return Value{static_cast<double>(it->second.size())};
    }

    return Value{0.0};
}

// process.detach() - detach from parent terminal (daemonize)
static Value process_detach(const std::vector<Value>& /*args*/, EnvPtr /*env*/, const Token& token) {
#ifndef _WIN32
    // Fork and exit parent
    pid_t pid = fork();
    if (pid < 0) {
        throw SwaziError("RuntimeError", "Fork failed during detach", token.loc);
    }
    if (pid > 0) {
        // Parent exits
        exit(0);
    }

    // Child continues - create new session
    if (setsid() < 0) {
        throw SwaziError("RuntimeError", "setsid failed during detach", token.loc);
    }

    // Ignore SIGHUP
    signal(SIGHUP, SIG_IGN);

    // Second fork to prevent acquiring terminal
    pid = fork();
    if (pid < 0) {
        throw SwaziError("RuntimeError", "Second fork failed during detach", token.loc);
    }
    if (pid > 0) {
        exit(0);
    }

    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return Value{true};
#else
    throw SwaziError("NotImplementedError", "process.detach() not supported on Windows", token.loc);
#endif
}

// process.ignoreSignals(signals...) - ignore specified signals
static Value process_ignore_signals(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    for (const auto& arg : args) {
        int signum = resolve_signal(arg, token);

        // Check if uncatchable
#ifndef _WIN32
        if (signum == SIGKILL || signum == SIGSTOP) {
            throw SwaziError("RuntimeError",
                "Cannot ignore SIGKILL or SIGSTOP (uncatchable)", token.loc);
        }

        // Warn about dangerous signals
        if (signum == SIGSEGV || signum == SIGBUS || signum == SIGFPE ||
            signum == SIGILL || signum == SIGTRAP) {
            std::cerr << "Warning: Ignoring signal " << signum
                      << " is dangerous and may cause crashes\n";
        }
#endif

        signal(signum, SIG_IGN);
    }

    return Value{true};
}

Value process_send_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_send(args, env, token);
}

Value process_on_message_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_on_message(args, env, token);
}
Value process_off_impl(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_off(args, env, token);
}
Value process_listeners_impl(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_listeners(args, env, token);
}

Value process_detach_impl(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_detach(args, env, token);
}

Value process_ignore_signals_impl(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    return process_ignore_signals(args, env, token);
}