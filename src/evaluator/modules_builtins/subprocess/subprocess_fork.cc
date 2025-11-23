// src/evaluator/modules_builtins/subprocess_fork.cc
// Implements fork() with bidirectional IPC using dedicated pipes (fd 3/4)

#include <atomic>
#include <cstring>
#include <map>
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
extern char** environ;
#else
#include <stdlib.h>
#define environ _environ
extern char** _environ;
#endif

// Forward declarations
static void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args);
static std::string value_to_string_simple_local(const Value& v);
static Token make_native_token(const std::string& name);

// --- Fork-specific child entry ---
struct ForkChildEntry {
    long long id;
    uv_process_t* proc = nullptr;

    // Standard I/O pipes (optional - can be inherit/ignore)
    uv_pipe_t* stdout_pipe = nullptr;
    uv_pipe_t* stderr_pipe = nullptr;
    uv_pipe_t* stdin_pipe = nullptr;

    // IPC pipes (always present for fork)
    uv_pipe_t* ipc_read_pipe = nullptr;   // Parent reads child messages (child writes)
    uv_pipe_t* ipc_write_pipe = nullptr;  // Parent writes messages (child reads)

    // Listeners
    std::mutex listeners_mutex;
    std::vector<FunctionPtr> stdout_data_listeners;
    std::vector<FunctionPtr> stderr_data_listeners;
    std::vector<FunctionPtr> message_listeners;  // IPC messages only
    std::vector<FunctionPtr> exit_listeners;

    bool closed = false;
};

// Global registry
static std::mutex g_fork_children_mutex;
static std::atomic<long long> g_next_fork_id{1};
static std::unordered_map<long long, std::shared_ptr<ForkChildEntry>> g_fork_children;

// Helper to schedule JS callback on runtime thread
static void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// Helper to convert Value to string
static std::string value_to_string_simple_local(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::string();
}

// Helper to create token for error reporting
static Token make_native_token(const std::string& name) {
    Token t;
    t.type = TokenType::IDENTIFIER;
    t.loc = TokenLocation(std::string("<fork>"), 0, 0, 0);
    return t;
}

// Allocate buffer for reading
static void alloc_pipe_cb(uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested);
    buf->len = static_cast<unsigned int>(suggested);
}

// IPC message callback - child sent message on fd 4, parent receives
static void ipc_message_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ForkChildEntry* entry_ptr = static_cast<ForkChildEntry*>(stream->data);

    if (nread > 0 && entry_ptr) {
        // Create buffer directly from received bytes (no JSON parsing)
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        // Fire message listeners with raw buffer
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry_ptr->listeners_mutex);
            listeners = entry_ptr->message_listeners;
        }

        for (auto& cb : listeners) {
            if (cb) schedule_listener_call(cb, {Value{buffer}});
        }
    } else if (nread < 0) {
        uv_read_stop(stream);
    }

    if (buf && buf->base) free(buf->base);
}

// Standard output callback - now returns buffers
static void stdout_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ForkChildEntry* entry_ptr = static_cast<ForkChildEntry*>(stream->data);

    if (nread > 0 && entry_ptr) {
        // Create buffer from stdout data
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry_ptr->listeners_mutex);
            listeners = entry_ptr->stdout_data_listeners;
        }
        for (auto& cb : listeners) {
            if (cb) schedule_listener_call(cb, {Value{buffer}});
        }
    } else if (nread < 0) {
        uv_read_stop(stream);
    }

    if (buf && buf->base) free(buf->base);
}

// Standard error callback - now returns buffers
static void stderr_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ForkChildEntry* entry_ptr = static_cast<ForkChildEntry*>(stream->data);

    if (nread > 0 && entry_ptr) {
        // Create buffer from stderr data
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry_ptr->listeners_mutex);
            listeners = entry_ptr->stderr_data_listeners;
        }
        for (auto& cb : listeners) {
            if (cb) schedule_listener_call(cb, {Value{buffer}});
        }
    } else if (nread < 0) {
        uv_read_stop(stream);
    }

    if (buf && buf->base) free(buf->base);
}

// Process exit callback
static void fork_exit_cb(uv_process_t* req, int64_t exit_status, int term_signal) {
    ForkChildEntry* entry_ptr = static_cast<ForkChildEntry*>(req->data);
    std::shared_ptr<ForkChildEntry> entry;
    long long id = 0;

    if (entry_ptr) {
        std::lock_guard<std::mutex> lk(g_fork_children_mutex);
        for (auto& kv : g_fork_children) {
            if (kv.second.get() == entry_ptr) {
                entry = kv.second;
                id = kv.first;
                break;
            }
        }
    }

    // Notify exit listeners
    if (entry) {
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->exit_listeners;
        }
        for (auto& cb : listeners) {
            schedule_listener_call(cb, {Value{static_cast<double>(exit_status)}, Value{static_cast<double>(term_signal)}});
        }
    }

    // Cleanup handles
    uv_loop_t* loop = req ? req->loop : nullptr;
    if (loop) {
        scheduler_run_on_loop([req, entry_ptr]() {
            if (entry_ptr->stdout_pipe) {
                uv_close((uv_handle_t*)entry_ptr->stdout_pipe,
                    [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->stdout_pipe = nullptr;
            }
            if (entry_ptr->stderr_pipe) {
                uv_close((uv_handle_t*)entry_ptr->stderr_pipe,
                    [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->stderr_pipe = nullptr;
            }
            if (entry_ptr->stdin_pipe) {
                uv_close((uv_handle_t*)entry_ptr->stdin_pipe,
                    [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->stdin_pipe = nullptr;
            }
            if (entry_ptr->ipc_read_pipe) {
                uv_close((uv_handle_t*)entry_ptr->ipc_read_pipe,
                    [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->ipc_read_pipe = nullptr;
            }
            if (entry_ptr->ipc_write_pipe) {
                uv_close((uv_handle_t*)entry_ptr->ipc_write_pipe,
                    [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->ipc_write_pipe = nullptr;
            }
            if (req) {
                uv_close((uv_handle_t*)req,
                    [](uv_handle_t* h) { delete reinterpret_cast<uv_process_t*>(h); });
            }
        });
    }

    // Remove from registry
    if (id) {
        std::lock_guard<std::mutex> lk(g_fork_children_mutex);
        g_fork_children.erase(id);
    }
}

// Create the JavaScript child object with .send(), .on(), .stdout, .stderr, .kill()
static ObjectPtr make_fork_child_object(std::shared_ptr<ForkChildEntry> entry) {
    auto child_obj = std::make_shared<ObjectValue>();

    // Helper: create stream objects for stdout/stderr
    auto make_stream_obj = [&](bool is_stdout) {
        auto stream = std::make_shared<ObjectValue>();

        auto on_impl = [entry, is_stdout](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) throw SwaziError("TypeError", "stream.on requires (event, callback)", token.loc);
            if (!std::holds_alternative<std::string>(args[0])) throw SwaziError("TypeError", "event must be string", token.loc);
            if (!std::holds_alternative<FunctionPtr>(args[1])) throw SwaziError("TypeError", "callback must be function", token.loc);

            std::string ev = std::get<std::string>(args[0]);
            FunctionPtr cb = std::get<FunctionPtr>(args[1]);

            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            if (is_stdout && ev == "data") {
                entry->stdout_data_listeners.push_back(cb);
            } else if (!is_stdout && ev == "data") {
                entry->stderr_data_listeners.push_back(cb);
            }
            return std::monostate{};
        };

        Token tok = make_native_token("stream.on");
        auto fn_on = std::make_shared<FunctionValue>("native:stream.on", on_impl, nullptr, tok);
        stream->properties["on"] = PropertyDescriptor{fn_on, false, false, false, tok};

        return stream;
    };

    child_obj->properties["stdout"] = {Value{make_stream_obj(true)}, false, false, true, Token{}};
    child_obj->properties["stderr"] = {Value{make_stream_obj(false)}, false, false, true, Token{}};

    // child.on(event, callback) - for 'exit' and 'message'
    auto on_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) throw SwaziError("TypeError", "child.on requires (event, callback)", token.loc);
        if (!std::holds_alternative<std::string>(args[0])) throw SwaziError("TypeError", "event must be string", token.loc);
        if (!std::holds_alternative<FunctionPtr>(args[1])) throw SwaziError("TypeError", "callback must be function", token.loc);

        std::string ev = std::get<std::string>(args[0]);
        FunctionPtr cb = std::get<FunctionPtr>(args[1]);

        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
        if (ev == "exit") {
            entry->exit_listeners.push_back(cb);
        } else if (ev == "message") {
            entry->message_listeners.push_back(cb);
        }
        return std::monostate{};
    };

    Token tok_on = make_native_token("child.on");
    auto fn_on = std::make_shared<FunctionValue>("native:child.on", on_impl, nullptr, tok_on);
    child_obj->properties["on"] = PropertyDescriptor{fn_on, false, false, false, tok_on};

    // child.send(msg) - sends JSON message to child via IPC
    auto send_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) throw SwaziError("TypeError", "send requires a message", token.loc);

        std::vector<uint8_t> data_bytes;

        // Convert value to bytes
        if (std::holds_alternative<std::string>(args[0])) {
            std::string str = std::get<std::string>(args[0]);
            data_bytes.assign(str.begin(), str.end());
        } else if (std::holds_alternative<double>(args[0])) {
            // Convert number to string, then to bytes
            std::ostringstream ss;
            ss << std::get<double>(args[0]);
            std::string str = ss.str();
            data_bytes.assign(str.begin(), str.end());
        } else if (std::holds_alternative<bool>(args[0])) {
            std::string str = std::get<bool>(args[0]) ? "true" : "false";
            data_bytes.assign(str.begin(), str.end());
        } else if (std::holds_alternative<BufferPtr>(args[0])) {
            // Send buffer directly
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            data_bytes = buf->data;
        } else {
            throw SwaziError("TypeError", "send() requires string, number, boolean, or buffer", token.loc);
        }

        if (data_bytes.empty()) {
            return std::monostate{};
        }

        if (!entry->ipc_write_pipe) {
            throw SwaziError("IOError", "IPC pipe not available", token.loc);
        }

        // Allocate and send raw bytes
        uv_buf_t buf = uv_buf_init((char*)malloc(data_bytes.size()),
            static_cast<unsigned int>(data_bytes.size()));
        memcpy(buf.base, data_bytes.data(), data_bytes.size());

        uv_write_t* req = new uv_write_t;
        req->data = buf.base;

        uv_write(req, (uv_stream_t*)entry->ipc_write_pipe, &buf, 1,
            [](uv_write_t* req, int) {
                if (req->data) free(req->data);
                delete req;
            });

        return std::monostate{};
    };

    Token tok_send = make_native_token("child.send");
    auto fn_send = std::make_shared<FunctionValue>("native:child.send", send_impl, nullptr, tok_send);
    child_obj->properties["send"] = PropertyDescriptor{fn_send, false, false, false, tok_send};

    // child.kill(signal?)
    auto kill_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        int sig = SIGTERM;
        if (!args.empty() && std::holds_alternative<double>(args[0])) {
            sig = static_cast<int>(std::get<double>(args[0]));
        }
        if (entry->proc && entry->proc->pid) {
            uv_process_kill(entry->proc, sig);
        }
        return std::monostate{};
    };

    Token tok_kill = make_native_token("child.kill");
    auto fn_kill = std::make_shared<FunctionValue>("native:child.kill", kill_impl, nullptr, tok_kill);
    child_obj->properties["kill"] = PropertyDescriptor{fn_kill, false, false, false, tok_kill};

    // pid property (will be set after spawn)
    child_obj->properties["pid"] = {std::monostate{}, false, false, true, Token{}};

    return child_obj;
}

// Fork options structure
struct ForkOptions {
    std::string cwd;
    std::vector<std::string> env_vec;  // "KEY=VAL"
    std::vector<std::string> stdio;    // ["pipe"|"inherit"|"ignore", ...]
};

// Main fork implementation
static ObjectPtr do_fork(
    const std::string& script_path,
    const std::vector<std::string>& args,
    const ForkOptions& opts,
    int& out_pid,
    const Token& token) {
    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) {
        throw SwaziError("RuntimeError", "No event loop available for fork", token.loc);
    }

    auto entry = std::make_shared<ForkChildEntry>();
    entry->id = g_next_fork_id.fetch_add(1);

    // Allocate process handle
    uv_process_t* proc = new uv_process_t;
    proc->data = entry.get();
    entry->proc = proc;

    // Determine interpreter path
    std::string interpreter = "/proc/self/exe";  // Linux/Unix
#ifdef _WIN32
    char buf[MAX_PATH];
    GetModuleFileNameA(NULL, buf, MAX_PATH);
    interpreter = buf;
#elif defined(__APPLE__)
    // macOS needs _NSGetExecutablePath
    char buf[1024];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        interpreter = buf;
    }
#endif

    // Create stdio pipes based on options
    bool use_pipe_stdout = true;
    bool use_pipe_stderr = true;
    bool use_pipe_stdin = false;  // Fork doesn't need stdin by default

    if (!opts.stdio.empty()) {
        auto get_stdio = [&](size_t idx) -> std::string {
            return idx < opts.stdio.size() ? opts.stdio[idx] : "pipe";
        };
        use_pipe_stdin = (get_stdio(0) == "pipe");
        use_pipe_stdout = (get_stdio(1) == "pipe");
        use_pipe_stderr = (get_stdio(2) == "pipe");
    }

    // Create standard I/O pipes if needed
    if (use_pipe_stdout) {
        entry->stdout_pipe = new uv_pipe_t;
        uv_pipe_init(loop, entry->stdout_pipe, 0);
        entry->stdout_pipe->data = entry.get();
    }

    if (use_pipe_stderr) {
        entry->stderr_pipe = new uv_pipe_t;
        uv_pipe_init(loop, entry->stderr_pipe, 0);
        entry->stderr_pipe->data = entry.get();
    }

    if (use_pipe_stdin) {
        entry->stdin_pipe = new uv_pipe_t;
        uv_pipe_init(loop, entry->stdin_pipe, 0);
        entry->stdin_pipe->data = entry.get();
    }

    // Create IPC pipes (always for fork)
    entry->ipc_read_pipe = new uv_pipe_t;
    uv_pipe_init(loop, entry->ipc_read_pipe, 0);
    entry->ipc_read_pipe->data = entry.get();

    entry->ipc_write_pipe = new uv_pipe_t;
    uv_pipe_init(loop, entry->ipc_write_pipe, 0);
    entry->ipc_write_pipe->data = entry.get();

    // Build argv: [interpreter, script_path, ...args]
    std::vector<char*> argv;
    std::vector<char*> allocated;

    char* p_interp = strdup(interpreter.c_str());
    argv.push_back(p_interp);
    allocated.push_back(p_interp);

    char* p_script = strdup(script_path.c_str());
    argv.push_back(p_script);
    allocated.push_back(p_script);

    for (const auto& arg : args) {
        char* p = strdup(arg.c_str());
        argv.push_back(p);
        allocated.push_back(p);
    }
    argv.push_back(nullptr);

    // Build environment with parent env + custom env + SWAZI_IPC marker
    std::map<std::string, std::string> env_map;

    // Start with parent environment
    for (char** env = environ; env && *env; ++env) {
        std::string entry_str(*env);
        size_t eq = entry_str.find('=');
        if (eq != std::string::npos) {
            std::string key = entry_str.substr(0, eq);
            std::string val = entry_str.substr(eq + 1);
            env_map[key] = val;
        }
    }

    // Override with user-provided env
    for (const auto& e : opts.env_vec) {
        size_t eq = e.find('=');
        if (eq != std::string::npos) {
            std::string key = e.substr(0, eq);
            std::string val = e.substr(eq + 1);
            env_map[key] = val;
        }
    }

    // Add SWAZI_IPC=1 marker
    env_map["SWAZI_IPC"] = "1";

    // Convert to char* array
    std::vector<char*> envp;
    std::vector<char*> env_allocated;

    for (const auto& kv : env_map) {
        std::string entry_str = kv.first + "=" + kv.second;
        char* p = strdup(entry_str.c_str());
        envp.push_back(p);
        env_allocated.push_back(p);
    }
    envp.push_back(nullptr);

    // Setup stdio containers (5 descriptors: stdin, stdout, stderr, ipc_read, ipc_write)
    uv_stdio_container_t stdio[5];

    // fd 0: stdin
    if (use_pipe_stdin) {
        stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
        stdio[0].data.stream = (uv_stream_t*)entry->stdin_pipe;
    } else {
        stdio[0].flags = UV_IGNORE;
        stdio[0].data.stream = nullptr;
    }

    // fd 1: stdout
    if (use_pipe_stdout) {
        stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
        stdio[1].data.stream = (uv_stream_t*)entry->stdout_pipe;
    } else if (!opts.stdio.empty() && opts.stdio[1] == "inherit") {
        stdio[1].flags = UV_INHERIT_FD;
        stdio[1].data.fd = 1;
    } else {
        stdio[1].flags = UV_IGNORE;
        stdio[1].data.stream = nullptr;
    }

    // fd 2: stderr
    if (use_pipe_stderr) {
        stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
        stdio[2].data.stream = (uv_stream_t*)entry->stderr_pipe;
    } else if (!opts.stdio.empty() && opts.stdio[2] == "inherit") {
        stdio[2].flags = UV_INHERIT_FD;
        stdio[2].data.fd = 2;
    } else {
        stdio[2].flags = UV_IGNORE;
        stdio[2].data.stream = nullptr;
    }

    // fd 3: IPC read (child reads parent's messages)
    stdio[3].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
    stdio[3].data.stream = (uv_stream_t*)entry->ipc_write_pipe;

    // fd 4: IPC write (child writes messages to parent)
    stdio[4].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
    stdio[4].data.stream = (uv_stream_t*)entry->ipc_read_pipe;

    // Spawn options
    uv_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.exit_cb = fork_exit_cb;
    options.file = interpreter.c_str();
    options.args = argv.data();
    options.stdio_count = 5;
    options.stdio = stdio;
    options.cwd = opts.cwd.empty() ? nullptr : opts.cwd.c_str();
    options.env = envp.data();

    // Spawn the process
    int r = uv_spawn(loop, proc, &options);

    // Cleanup allocated strings
    for (char* s : allocated) free(s);
    for (char* s : env_allocated) free(s);

    if (r != 0) {
        // Cleanup on failure
        if (entry->stdout_pipe) delete entry->stdout_pipe;
        if (entry->stderr_pipe) delete entry->stderr_pipe;
        if (entry->stdin_pipe) delete entry->stdin_pipe;
        if (entry->ipc_read_pipe) delete entry->ipc_read_pipe;
        if (entry->ipc_write_pipe) delete entry->ipc_write_pipe;
        delete proc;

        throw SwaziError("IOError",
            std::string("fork failed: ") + uv_strerror(r), token.loc);
    }

    out_pid = proc->pid;

    // Register in global map
    {
        std::lock_guard<std::mutex> lk(g_fork_children_mutex);
        g_fork_children[entry->id] = entry;
    }

    // Start reading pipes on loop thread
    scheduler_run_on_loop([entry]() {
        if (entry->stdout_pipe) {
            uv_read_start((uv_stream_t*)entry->stdout_pipe,
                alloc_pipe_cb, stdout_read_cb);
        }
        if (entry->stderr_pipe) {
            uv_read_start((uv_stream_t*)entry->stderr_pipe,
                alloc_pipe_cb, stderr_read_cb);
        }
        if (entry->ipc_read_pipe) {
            uv_read_start((uv_stream_t*)entry->ipc_read_pipe,
                alloc_pipe_cb, ipc_message_cb);
        }
    });

    // Create and return child object wrapper
    auto child_obj = make_fork_child_object(entry);
    child_obj->properties["pid"] = {
        Value{static_cast<double>(proc->pid)},
        false, false, true, Token{}};

    return child_obj;
}

// Public API: native_fork(script_path, args?, options?)
Value native_fork(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "fork requires script path as first argument", token.loc);
    }

    std::string script_path = std::get<std::string>(args[0]);

    // Parse args array (second parameter)
    std::vector<std::string> script_args;
    size_t options_index = 1;

    if (args.size() >= 2 && std::holds_alternative<ArrayPtr>(args[1])) {
        ArrayPtr arr = std::get<ArrayPtr>(args[1]);
        for (auto& el : arr->elements) {
            script_args.push_back(value_to_string_simple_local(el));
        }
        options_index = 2;
    }

    // Parse options object
    ForkOptions opts;
    if (args.size() > options_index && std::holds_alternative<ObjectPtr>(args[options_index])) {
        ObjectPtr o = std::get<ObjectPtr>(args[options_index]);

        // cwd
        auto itcwd = o->properties.find("cwd");
        if (itcwd != o->properties.end()) {
            opts.cwd = value_to_string_simple_local(itcwd->second.value);
        }

        // env
        auto itenv = o->properties.find("env");
        if (itenv != o->properties.end() && std::holds_alternative<ObjectPtr>(itenv->second.value)) {
            ObjectPtr eobj = std::get<ObjectPtr>(itenv->second.value);
            for (auto& kv : eobj->properties) {
                std::string key = kv.first;
                std::string val = value_to_string_simple_local(kv.second.value);
                opts.env_vec.push_back(key + "=" + val);
            }
        }

        // stdio
        auto itstd = o->properties.find("stdio");
        if (itstd != o->properties.end()) {
            if (std::holds_alternative<std::string>(itstd->second.value)) {
                std::string s = std::get<std::string>(itstd->second.value);
                opts.stdio = {s, s, s};
            } else if (std::holds_alternative<ArrayPtr>(itstd->second.value)) {
                ArrayPtr sarr = std::get<ArrayPtr>(itstd->second.value);
                for (size_t i = 0; i < sarr->elements.size() && i < 3; ++i) {
                    opts.stdio.push_back(value_to_string_simple_local(sarr->elements[i]));
                }
            }
        }
    }

    int pid = 0;
    auto child_obj = do_fork(script_path, script_args, opts, pid, token);
    return Value{child_obj};
}