// Place this file at: src/evaluator/modules_builtins/subprocess.cpp
// Build with -luv (project already links libuv)
// Implements a minimal subprocess builtin with exec, spawn, fork.
// - exec(cmd[, cb]) -> Promise resolving { stdout, stderr, code } OR if cb provided calls cb(err, result).
// - spawn(cmd, ...args) -> returns a child object with .stdout/.stderr objects that have .on(event, cb)
// - fork(script, [...args]) -> spawn the same interpreter with provided script and return child with .send(msg) and 'message' listener
//
// This implementation uses libuv (uv_spawn + pipes) and the AsyncBridge bridge functions:
//   enqueue_callback_global(void*), scheduler_run_on_loop(...), CallbackPayload
// It follows the project's existing pattern: create CallbackPayload instances to deliver FunctionPtr callbacks
// back into the runtime thread via Scheduler's runner.

#include <atomic>
#include <cstring>
#include <iostream>
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

// Add this for environ access
#ifndef _WIN32
extern char** environ;
#else
// Windows equivalent
#include <stdlib.h>
// Use _environ on Windows
#define environ _environ
extern char** _environ;
#endif

// Small helper to make Token placeholders for native functions
static Token make_native_token(const std::string& name) {
    Token t;
    t.type = TokenType::IDENTIFIER;
    t.loc = TokenLocation(std::string("<subprocess>"), 0, 0, 0);
    return t;
}

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

// Globals for bookkeeping
static std::mutex g_children_mutex;
static std::atomic<long long> g_next_child_id{1};

struct ChildEntry {
    long long id;
    uv_process_t* proc = nullptr;
    uv_pipe_t* stdout_pipe = nullptr;
    uv_pipe_t* stderr_pipe = nullptr;
    uv_pipe_t* stdin_pipe = nullptr;

    std::mutex listeners_mutex;
    std::vector<FunctionPtr> stdout_data_listeners;
    std::vector<FunctionPtr> stderr_data_listeners;
    std::vector<FunctionPtr> exit_listeners;
    std::vector<FunctionPtr> message_listeners;  // for fork IPC

    bool closed = false;
};
static std::unordered_map<long long, std::shared_ptr<ChildEntry>> g_children;

// Utility to schedule invocation of a JS function on runtime thread (via CallbackPayload)
static void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// Helper to create ObjectPtr child wrapper with JS API
static ObjectPtr make_child_object(std::shared_ptr<ChildEntry> entry) {
    auto child_obj = std::make_shared<ObjectValue>();

    // Helper to expose .stdout and .stderr as objects with .on(event, cb)
    auto make_stream_obj = [&](bool is_stdout) {
        auto stream = std::make_shared<ObjectValue>();

        // stream.on(event, cb)
        auto on_impl = [entry, is_stdout](const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) -> Value {
            if (args.size() < 2) throw SwaziError("TypeError", "stream.on requires (event, cb)", token.loc);
            if (!std::holds_alternative<std::string>(args[0])) throw SwaziError("TypeError", "event name must be string", token.loc);
            if (!std::holds_alternative<FunctionPtr>(args[1])) throw SwaziError("TypeError", "callback must be function", token.loc);
            std::string ev = std::get<std::string>(args[0]);
            FunctionPtr cb = std::get<FunctionPtr>(args[1]);

            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            if (is_stdout) {
                if (ev == "data") entry->stdout_data_listeners.push_back(cb);
            } else {
                if (ev == "data") entry->stderr_data_listeners.push_back(cb);
            }
            return std::monostate{};
        };
        Token tok = make_native_token("child_stream.on");
        auto fn_on = std::make_shared<FunctionValue>("native:child_stream.on", on_impl, nullptr, tok);
        stream->properties["on"] = PropertyDescriptor{fn_on, false, false, false, tok};

        return stream;
    };

    child_obj->properties["stdout"] = {Value{make_stream_obj(true)}, false, false, true, Token{}};
    child_obj->properties["stderr"] = {Value{make_stream_obj(false)}, false, false, true, Token{}};

    // child.on(event, cb) for 'exit' and 'message'
    auto on_impl = [entry](const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) -> Value {
        if (args.size() < 2) throw SwaziError("TypeError", "child.on requires (event, cb)", token.loc);
        if (!std::holds_alternative<std::string>(args[0])) throw SwaziError("TypeError", "event must be string", token.loc);
        if (!std::holds_alternative<FunctionPtr>(args[1])) throw SwaziError("TypeError", "cb must be function", token.loc);
        std::string ev = std::get<std::string>(args[0]);
        FunctionPtr cb = std::get<FunctionPtr>(args[1]);

        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
        if (ev == "exit") {
            entry->exit_listeners.push_back(cb);
        } else if (ev == "message") {
            entry->message_listeners.push_back(cb);
        } else {
            // unknown event, accept for forward compatibility
        }
        return std::monostate{};
    };
    Token tok_on = make_native_token("child.on");
    auto fn_on = std::make_shared<FunctionValue>("native:child.on", on_impl, nullptr, tok_on);
    child_obj->properties["on"] = PropertyDescriptor{fn_on, false, false, false, tok_on};

    // child.kill(signal?)
    auto kill_impl = [entry](const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) -> Value {
        int sig = SIGTERM;
        if (!args.empty() && std::holds_alternative<double>(args[0])) sig = static_cast<int>(std::get<double>(args[0]));
        if (entry->proc && entry->proc->pid) {
            uv_process_kill(entry->proc, sig);
        }
        return std::monostate{};
    };
    Token tok_kill = make_native_token("child.kill");
    auto fn_kill = std::make_shared<FunctionValue>("native:child.kill", kill_impl, nullptr, tok_kill);
    child_obj->properties["kill"] = PropertyDescriptor{fn_kill, false, false, false, tok_kill};

    // store pid property (may be set later)
    child_obj->properties["pid"] = {std::monostate{}, false, false, true, Token{}};

    return child_obj;
}

// Read callback for pipe: deliver data to listeners
static void alloc_pipe_cb(uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested);
    buf->len = static_cast<unsigned int>(suggested);
}
static void stdout_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ChildEntry* entry_ptr = static_cast<ChildEntry*>(stream->data);
    if (nread > 0) {
        // Create buffer instead of string
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        if (entry_ptr) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry_ptr->listeners_mutex);
                listeners = entry_ptr->stdout_data_listeners;
            }
            for (auto& cb : listeners) {
                if (!cb) continue;
                schedule_listener_call(cb, {Value{buffer}});
            }
        }
    } else if (nread < 0) {
        uv_read_stop(stream);
    }

    if (buf && buf->base) free(buf->base);
}

static void stderr_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    ChildEntry* entry_ptr = static_cast<ChildEntry*>(stream->data);
    if (nread > 0) {
        // Create buffer instead of string
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        if (entry_ptr) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry_ptr->listeners_mutex);
                listeners = entry_ptr->stderr_data_listeners;
            }
            for (auto& cb : listeners) schedule_listener_call(cb, {Value{buffer}});
        }
    } else if (nread < 0) {
        uv_read_stop(stream);
    }

    if (buf && buf->base) free(buf->base);
}

// Process exit callback
static void exit_cb(uv_process_t* req, int64_t exit_status, int term_signal) {
    ChildEntry* entry_ptr = static_cast<ChildEntry*>(req->data);
    std::shared_ptr<ChildEntry> entry;
    long long id = 0;
    if (entry_ptr) {
        // find shared_ptr
        std::lock_guard<std::mutex> lk(g_children_mutex);
        for (auto& kv : g_children) {
            if (kv.second.get() == entry_ptr) {
                entry = kv.second;
                id = kv.first;
                break;
            }
        }
    }
    // notify listeners
    if (entry) {
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->exit_listeners;
        }
        for (auto& cb : listeners) {
            // pass (code, signal)
            schedule_listener_call(cb, {Value{static_cast<double>(exit_status)}, Value{static_cast<double>(term_signal)}});
        }
    }

    // cleanup: close handles on loop thread
    uv_loop_t* loop = nullptr;
    if (req && req->loop) loop = req->loop;
    if (loop) {
        // schedule close on loop
        scheduler_run_on_loop([req, entry_ptr]() {
            if (entry_ptr->stdout_pipe) {
                uv_close((uv_handle_t*)entry_ptr->stdout_pipe, [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->stdout_pipe = nullptr;
            }
            if (entry_ptr->stderr_pipe) {
                uv_close((uv_handle_t*)entry_ptr->stderr_pipe, [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->stderr_pipe = nullptr;
            }
            if (entry_ptr->stdin_pipe) {
                uv_close((uv_handle_t*)entry_ptr->stdin_pipe, [](uv_handle_t* h) { delete reinterpret_cast<uv_pipe_t*>(h); });
                entry_ptr->stdin_pipe = nullptr;
            }
            if (req) {
                uv_close((uv_handle_t*)req, [](uv_handle_t* h) { /* uv will free uv_process_t memory only if allocated on heap, we allocated on heap below */ delete reinterpret_cast<uv_process_t*>(h); });
            }
        });
    }

    // erase from map
    if (id) {
        std::lock_guard<std::mutex> lk(g_children_mutex);
        g_children.erase(id);
    }
}

struct SpawnOptions {
    std::string cwd;
    std::vector<std::string> env_vec;  // "KEY=VAL"
    std::vector<std::string> stdio;    // entries for fd0,1,2 -> "pipe"|"inherit"|"ignore"
};
// spawn implementation (low-level)
static ObjectPtr do_spawn(const std::string& file,
    const std::vector<std::string>& args,
    int& out_pid,
    const Token& token,
    const SpawnOptions& opts) {
    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) throw SwaziError("RuntimeError", "No event loop available to spawn process", token.loc);

    auto entry = std::make_shared<ChildEntry>();
    entry->id = g_next_child_id.fetch_add(1);

    // allocate process & pipes (we'll create pipes conditionally)
    uv_process_t* proc = new uv_process_t;
    proc->data = entry.get();
    entry->proc = proc;

    // We'll create pipe handles only if requested by stdio config (or default to pipe)
    bool use_pipe_stdin = true, use_pipe_stdout = true, use_pipe_stderr = true;
    if (!opts.stdio.empty()) {
        auto pick = [&](size_t i) {
            if (i >= opts.stdio.size()) return std::string("pipe");
            return opts.stdio[i];
        };
        use_pipe_stdin = (pick(0) == "pipe");
        use_pipe_stdout = (pick(1) == "pipe");
        use_pipe_stderr = (pick(2) == "pipe");
    }

    uv_pipe_t* stdout_pipe = nullptr;
    uv_pipe_t* stderr_pipe = nullptr;
    uv_pipe_t* stdin_pipe = nullptr;

    if (use_pipe_stdout) {
        stdout_pipe = new uv_pipe_t;
        uv_pipe_init(loop, stdout_pipe, 0);
        stdout_pipe->data = entry.get();
        entry->stdout_pipe = stdout_pipe;
    }
    if (use_pipe_stderr) {
        stderr_pipe = new uv_pipe_t;
        uv_pipe_init(loop, stderr_pipe, 0);
        stderr_pipe->data = entry.get();
        entry->stderr_pipe = stderr_pipe;
    }
    if (use_pipe_stdin) {
        stdin_pipe = new uv_pipe_t;
        uv_pipe_init(loop, stdin_pipe, 0);
        stdin_pipe->data = entry.get();
        entry->stdin_pipe = stdin_pipe;
    }

    // prepare argv (heap-allocated copies)
    std::vector<char*> argv;
    std::vector<char*> allocated;  // to free after spawn

    char* p0 = strdup(file.c_str());
    argv.push_back(p0);
    allocated.push_back(p0);
    for (const auto& a : args) {
        char* p = strdup(a.c_str());
        argv.push_back(p);
        allocated.push_back(p);
    }
    argv.push_back(nullptr);

    // prepare env array if provided
    std::vector<char*> envp;
    std::vector<char*> env_allocated;

    if (!opts.env_vec.empty()) {
        // Build map of user-defined env vars for easy lookup
        std::map<std::string, std::string> user_env;
        for (const auto& e : opts.env_vec) {
            size_t eq = e.find('=');
            if (eq != std::string::npos) {
                std::string key = e.substr(0, eq);
                std::string val = e.substr(eq + 1);
                user_env[key] = val;
            }
        }

        // First, add all parent environment variables
        extern char** environ;  // POSIX standard
        for (char** env = environ; env && *env; ++env) {
            std::string entry(*env);
            size_t eq = entry.find('=');
            if (eq != std::string::npos) {
                std::string key = entry.substr(0, eq);
                // Only add if NOT overridden by user
                if (user_env.find(key) == user_env.end()) {
                    char* pe = strdup(*env);
                    envp.push_back(pe);
                    env_allocated.push_back(pe);
                }
            }
        }

        // Then add user-defined/override env vars
        for (const auto& kv : user_env) {
            std::string entry = kv.first + "=" + kv.second;
            char* pe = strdup(entry.c_str());
            envp.push_back(pe);
            env_allocated.push_back(pe);
        }

        envp.push_back(nullptr);
    }

    // setup stdio containers according to opts.stdio
    uv_stdio_container_t stdio[3];
    // default to pipe if opts.stdio doesn't specify
    auto stdio_kind = [&](size_t i) -> std::string {
        if (i < opts.stdio.size()) return opts.stdio[i];
        return "pipe";
    };

    // Helper to set a container
    for (int i = 0; i < 3; ++i) {
        stdio[i].flags = UV_IGNORE;
        stdio[i].data.stream = nullptr;
    }

    // STDIN (0)
    {
        std::string k = stdio_kind(0);
        if (k == "pipe") {
            stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
            stdio[0].data.stream = (uv_stream_t*)stdin_pipe;
        } else if (k == "inherit") {
            stdio[0].flags = UV_INHERIT_FD;
            stdio[0].data.fd = 0;
        } else if (k == "ignore") {
            stdio[0].flags = UV_IGNORE;
        } else {
            // unknown -> default to pipe
            stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
            stdio[0].data.stream = (uv_stream_t*)stdin_pipe;
        }
    }

    // STDOUT (1)
    {
        std::string k = stdio_kind(1);
        if (k == "pipe") {
            stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[1].data.stream = (uv_stream_t*)stdout_pipe;
        } else if (k == "inherit") {
            stdio[1].flags = UV_INHERIT_FD;
            stdio[1].data.fd = 1;
        } else if (k == "ignore") {
            stdio[1].flags = UV_IGNORE;
        } else {
            stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[1].data.stream = (uv_stream_t*)stdout_pipe;
        }
    }

    // STDERR (2)
    {
        std::string k = stdio_kind(2);
        if (k == "pipe") {
            stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[2].data.stream = (uv_stream_t*)stderr_pipe;
        } else if (k == "inherit") {
            stdio[2].flags = UV_INHERIT_FD;
            stdio[2].data.fd = 2;
        } else if (k == "ignore") {
            stdio[2].flags = UV_IGNORE;
        } else {
            stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[2].data.stream = (uv_stream_t*)stderr_pipe;
        }
    }

    uv_process_options_t options;
    memset(&options, 0, sizeof(options));
    options.exit_cb = exit_cb;
    options.file = file.c_str();
    options.args = argv.data();
    options.stdio_count = 3;
    options.stdio = stdio;
    options.cwd = opts.cwd.empty() ? nullptr : opts.cwd.c_str();
    options.env = envp.empty() ? nullptr : envp.data();

    int r = uv_spawn(loop, proc, &options);

    // free our duplicated C strings
    for (char* s : allocated) free(s);
    for (char* s : env_allocated) free(s);

    if (r != 0) {
        // cleanup allocated handles on loop thread
        if (stdout_pipe) delete stdout_pipe;
        if (stderr_pipe) delete stderr_pipe;
        if (stdin_pipe) delete stdin_pipe;
        delete proc;
        throw SwaziError("IOError", std::string("uv_spawn failed: ") + uv_strerror(r), token.loc);
    }

    out_pid = proc->pid;

    // register in map
    {
        std::lock_guard<std::mutex> lk(g_children_mutex);
        g_children[entry->id] = entry;
    }

    // start reading stdout/stderr on the loop thread (only when pipe)
    scheduler_run_on_loop([entry]() {
        if (entry->stdout_pipe) uv_read_start((uv_stream_t*)entry->stdout_pipe, alloc_pipe_cb, stdout_read_cb);
        if (entry->stderr_pipe) uv_read_start((uv_stream_t*)entry->stderr_pipe, alloc_pipe_cb, stderr_read_cb);
    });

    // create JS wrapper
    auto child_obj = make_child_object(entry);

    // set pid property
    child_obj->properties["pid"] = {Value{static_cast<double>(proc->pid)}, false, false, true, Token{}};

    return child_obj;
}

// Builtin: spawn(cmd, ...args)
static Value native_spawn(const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) {
    if (args.empty()) throw SwaziError("TypeError", "spawn requires command", token.loc);
    if (!std::holds_alternative<std::string>(args[0])) throw SwaziError("TypeError", "spawn first arg must be string", token.loc);

    std::string cmd = std::get<std::string>(args[0]);

    // parse optional args array (second parameter)
    std::vector<std::string> argv;
    if (args.size() >= 2 && std::holds_alternative<ArrayPtr>(args[1])) {
        ArrayPtr arr = std::get<ArrayPtr>(args[1]);
        for (auto& el : arr->elements) {
            argv.push_back(value_to_string_simple_local(el));
        }
    } else {
        // support old signature spawn(cmd, "a", "b", ...) for backward compatibility
        size_t start_idx = 1;
        // If second argument is options object, we won't parse it as arg list below.
        bool second_is_options = (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1]));
        if (!second_is_options) {
            for (size_t i = 1; i < args.size(); ++i) {
                if (std::holds_alternative<std::string>(args[i]))
                    argv.push_back(std::get<std::string>(args[i]));
                else if (std::holds_alternative<double>(args[i])) {
                    std::ostringstream ss;
                    ss << std::get<double>(args[i]);
                    argv.push_back(ss.str());
                } else
                    argv.push_back(value_to_string_simple_local(args[i]));
            }
        }
    }

    // parse optional options object (third param OR second if args array omitted)
    SpawnOptions opts;
    Value optVal;
    if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
        optVal = args[2];
    } else if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1]) && !std::holds_alternative<ArrayPtr>(args[1])) {
        optVal = args[1];
    }

    if (std::holds_alternative<ObjectPtr>(optVal)) {
        ObjectPtr o = std::get<ObjectPtr>(optVal);
        // cwd
        auto itcwd = o->properties.find("cwd");
        if (itcwd != o->properties.end()) opts.cwd = value_to_string_simple_local(itcwd->second.value);

        // env object -> build KEY=VALUE strings
        auto itenv = o->properties.find("env");
        if (itenv != o->properties.end() && std::holds_alternative<ObjectPtr>(itenv->second.value)) {
            ObjectPtr eobj = std::get<ObjectPtr>(itenv->second.value);
            for (auto& kv : eobj->properties) {
                std::string key = kv.first;
                std::string val = value_to_string_simple_local(kv.second.value);
                opts.env_vec.push_back(key + "=" + val);
            }
        }

        // stdio: can be string or array
        auto itstd = o->properties.find("stdio");
        if (itstd != o->properties.end()) {
            if (std::holds_alternative<std::string>(itstd->second.value)) {
                std::string s = std::get<std::string>(itstd->second.value);
                // if single string like "inherit" apply to all fd
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
    auto child_obj = do_spawn(cmd, argv, pid, token, opts);
    return Value{child_obj};
}

// Factory
std::shared_ptr<ObjectValue> make_subprocess_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token t = make_native_token("subprocess");

    // Capture evaluator in native_exec closure
    auto native_exec_impl = [evaluator](const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", "exec requires a string command", token.loc);
        }
        std::string cmd = std::get<std::string>(args[0]);
        FunctionPtr cb = nullptr;
        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1]))
            cb = std::get<FunctionPtr>(args[1]);

        std::string shell = "/bin/sh";
        std::vector<std::string> argv = {shell, "-c", cmd};

        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        struct ExecCtx {
            std::string out;
            std::string err;
            std::shared_ptr<PromiseValue> promise;
            FunctionPtr cb;
            Evaluator* eval;  // ADD THIS
        };
        auto ctx = std::make_shared<ExecCtx>();
        ctx->promise = promise;
        ctx->cb = cb;
        ctx->eval = evaluator;  // ADD THIS

        scheduler_run_on_loop([argv = std::move(argv), ctx, token]() {
            int pid = 0;
            Token t = token;
            try {
                auto child_obj = do_spawn(argv[0],
                    std::vector<std::string>(argv.begin() + 1, argv.end()), pid, t, SpawnOptions{});

                std::shared_ptr<ChildEntry> entry;
                {
                    std::lock_guard<std::mutex> lk(g_children_mutex);
                    for (auto& kv : g_children) {
                        if (kv.second->proc && kv.second->proc->pid == pid) {
                            entry = kv.second;
                            break;
                        }
                    }
                }

                if (!entry) {
                    // CRITICAL FIX: Use evaluator helper
                    if (ctx->eval && ctx->promise) {
                        ctx->eval->reject_promise(ctx->promise,
                            Value{std::string("spawn failed")});
                    }
                    return;
                }

                {
                    std::lock_guard<std::mutex> lk(entry->listeners_mutex);

                    entry->stdout_data_listeners.push_back(
                        std::make_shared<FunctionValue>(
                            "internal:exec_stdout_collector",
                            [entry, ctx](const std::vector<Value>& a, EnvPtr, const Token&) -> Value {
                                if (!a.empty() && std::holds_alternative<BufferPtr>(a[0])) {
                                    BufferPtr buf = std::get<BufferPtr>(a[0]);
                                    ctx->out.append(buf->data.begin(), buf->data.end());
                                }
                                return std::monostate{};
                            },
                            nullptr, Token{}));

                    entry->stderr_data_listeners.push_back(
                        std::make_shared<FunctionValue>(
                            "internal:exec_stderr_collector",
                            [entry, ctx](const std::vector<Value>& a, EnvPtr, const Token&) -> Value {
                                if (!a.empty() && std::holds_alternative<BufferPtr>(a[0])) {
                                    BufferPtr buf = std::get<BufferPtr>(a[0]);
                                    ctx->err.append(buf->data.begin(), buf->data.end());
                                }
                                return std::monostate{};
                            },
                            nullptr, Token{}));

                    entry->exit_listeners.push_back(
                        std::make_shared<FunctionValue>(
                            "internal:exec_exit_handler",
                            [entry, ctx](const std::vector<Value>& a, EnvPtr, const Token&) -> Value {
                                int code = 0;
                                if (!a.empty() && std::holds_alternative<double>(a[0]))
                                    code = static_cast<int>(std::get<double>(a[0]));

                                // Build result object
                                auto res = std::make_shared<ObjectValue>();
                                res->properties["stdout"] = {Value{ctx->out}, false, false, true, Token{}};
                                res->properties["stderr"] = {Value{ctx->err}, false, false, true, Token{}};
                                res->properties["code"] = {Value{static_cast<double>(code)}, false, false, true, Token{}};

                                // Invoke callback if provided
                                if (ctx->cb) {
                                    schedule_listener_call(ctx->cb,
                                        {Value{std::monostate{}}, Value{res}});
                                }

                                // CRITICAL FIX: Use evaluator helper instead of direct mutation
                                if (ctx->eval && ctx->promise) {
                                    ctx->eval->fulfill_promise(ctx->promise, Value{res});
                                } else if (ctx->promise) {
                                    // Fallback (shouldn't happen)
                                    ctx->promise->state = PromiseValue::State::FULFILLED;
                                    ctx->promise->result = res;
                                }

                                return std::monostate{};
                            },
                            nullptr, Token{}));
                }
            } catch (...) {
                if (ctx->cb) {
                    schedule_listener_call(ctx->cb,
                        {Value{std::string("spawn failed")}, Value{std::monostate{}}});
                }
                // CRITICAL FIX: Use evaluator helper
                if (ctx->eval && ctx->promise) {
                    ctx->eval->reject_promise(ctx->promise,
                        Value{std::string("spawn failed")});
                } else if (ctx->promise) {
                    ctx->promise->state = PromiseValue::State::REJECTED;
                    ctx->promise->result = std::string("spawn failed");
                }
            }
        });

        return promise;
    };

    auto fn_exec = std::make_shared<FunctionValue>(
        "native:subprocess.exec", native_exec_impl, nullptr, t);
    obj->properties["exec"] = PropertyDescriptor{Value{fn_exec}, false, false, false, t};

    auto fn_spawn = std::make_shared<FunctionValue>("native:subprocess.spawn", native_spawn, nullptr, t);
    obj->properties["spawn"] = PropertyDescriptor{Value{fn_spawn}, false, false, false, t};

    auto fn_fork = std::make_shared<FunctionValue>("native:subprocess.fork", native_fork, nullptr, t);
    obj->properties["fork"] = PropertyDescriptor{Value{fn_fork}, false, false, false, t};

    return obj;
}