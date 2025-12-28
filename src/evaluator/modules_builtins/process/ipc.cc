#include <fcntl.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

// Track active pipe handles
static std::atomic<int> g_active_pipes{0};
static std::mutex g_pipes_mutex;
static std::atomic<long long> g_next_pipe_id{1};

struct PipeHandle {
    long long id;
    uv_pipe_t* pipe = nullptr;
    int fd = -1;
    std::string path;
    bool is_reader = false;
    std::atomic<bool> closed{false};
    std::atomic<bool> ready{false};  // Pipe is ready for I/O

    std::mutex listeners_mutex;
    std::vector<FunctionPtr> data_listeners;
    std::vector<FunctionPtr> end_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> ready_listeners;  // Called when pipe is ready

    // Queue for writes that happen before pipe is ready
    std::mutex pending_mutex;
    struct PendingWrite {
        std::vector<uint8_t> data;
        FunctionPtr callback;
    };
    std::vector<PendingWrite> pending_writes;
};

static std::unordered_map<long long, std::shared_ptr<PipeHandle>> g_pipe_handles;

// Helper to schedule callbacks
static void schedule_pipe_callback(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// Allocate buffer for reading
static void alloc_ipc_cb(uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested);
    buf->len = static_cast<unsigned int>(suggested);
}

// Read callback
static void pipe_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    PipeHandle* handle = static_cast<PipeHandle*>(stream->data);

    if (nread > 0 && handle) {
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(handle->listeners_mutex);
            listeners = handle->data_listeners;
        }

        for (auto& cb : listeners) {
            if (cb) schedule_pipe_callback(cb, {Value{buffer}});
        }
    } else if (nread < 0) {
        // EOF or error
        uv_read_stop(stream);

        if (handle) {
            std::vector<FunctionPtr> end_listeners;
            std::vector<FunctionPtr> error_listeners;
            {
                std::lock_guard<std::mutex> lk(handle->listeners_mutex);
                end_listeners = handle->end_listeners;
                error_listeners = handle->error_listeners;
            }

            if (nread == UV_EOF) {
                for (auto& cb : end_listeners) {
                    if (cb) schedule_pipe_callback(cb, {});
                }
            } else {
                std::string error_msg = uv_strerror(static_cast<int>(nread));
                for (auto& cb : error_listeners) {
                    if (cb) schedule_pipe_callback(cb, {Value{error_msg}});
                }
            }
        }
    }

    if (buf && buf->base) free(buf->base);
}

// Helper to execute a pending write
static void execute_write(PipeHandle* handle, const std::vector<uint8_t>& data_bytes, FunctionPtr callback) {
    uv_buf_t buf = uv_buf_init((char*)malloc(data_bytes.size()),
        static_cast<unsigned int>(data_bytes.size()));
    memcpy(buf.base, data_bytes.data(), data_bytes.size());

    uv_write_t* req = new uv_write_t;

    struct WriteCtx {
        FunctionPtr cb;
        void* buffer;
    };

    WriteCtx* ctx = new WriteCtx{callback, buf.base};
    req->data = ctx;

    uv_write(req, (uv_stream_t*)handle->pipe, &buf, 1,
        [](uv_write_t* req, int status) {
            WriteCtx* ctx = static_cast<WriteCtx*>(req->data);

            if (ctx->buffer) free(ctx->buffer);

            if (ctx->cb) {
                if (status < 0) {
                    schedule_pipe_callback(ctx->cb,
                        {Value{std::string("Write error: ") + uv_strerror(status)}});
                } else {
                    schedule_pipe_callback(ctx->cb, {});
                }
            }

            delete ctx;
            delete req;
        });
}

// Helper: create native function
template <typename F>
static FunctionPtr make_ipc_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    return std::make_shared<FunctionValue>(name, native_impl, env, Token());
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

std::shared_ptr<ObjectValue> make_ipc_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // ipc.openPipe(path, mode) -> pipe object
    // mode: "r" (read), "w" (write)
    {
        auto fn = make_ipc_fn("ipc.openPipe", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError",
                    "ipc.openPipe requires path and mode ('r' or 'w')", token.loc);
            }

            std::string path = value_to_string_ipc(args[0]);
            std::string mode = value_to_string_ipc(args[1]);

            bool is_reader = (mode == "r" || mode == "read");
            bool is_writer = (mode == "w" || mode == "write");

            if (!is_reader && !is_writer) {
                throw SwaziError("RuntimeError",
                    "Mode must be 'r' (read) or 'w' (write)", token.loc);
            }

#ifndef _WIN32
            // Open the FIFO/pipe
            int flags = is_reader ? O_RDONLY | O_NONBLOCK : O_WRONLY;
            int fd = open(path.c_str(), flags);

            if (fd < 0) {
                // Provide more detailed error information
                std::string err_msg = "Failed to open pipe '" + path + "': " + std::strerror(errno);
                if (errno == ENXIO && !is_reader) {
                    err_msg += " (no reader connected yet - ensure reader opens first)";
                }
                throw SwaziError("IOError", err_msg, token.loc);
            }

            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                close(fd);
                throw SwaziError("RuntimeError", "No event loop available", token.loc);
            }

            auto handle = std::make_shared<PipeHandle>();
            handle->id = g_next_pipe_id.fetch_add(1);
            handle->fd = fd;
            handle->path = path;
            handle->is_reader = is_reader;

            // Store handle
            {
                std::lock_guard<std::mutex> lk(g_pipes_mutex);
                g_pipe_handles[handle->id] = handle;
            }

            // Initialize libuv pipe on loop thread
            scheduler_run_on_loop([handle, fd, is_reader, loop]() {
                handle->pipe = new uv_pipe_t;
                handle->pipe->data = handle.get();

                uv_pipe_init(loop, handle->pipe, 0);

                int r = uv_pipe_open(handle->pipe, fd);
                if (r != 0) {
                    delete handle->pipe;
                    handle->pipe = nullptr;
                    close(fd);

                    std::lock_guard<std::mutex> lk(g_pipes_mutex);
                    g_pipe_handles.erase(handle->id);

                    // Notify error listeners
                    std::vector<FunctionPtr> error_listeners;
                    {
                        std::lock_guard<std::mutex> lk2(handle->listeners_mutex);
                        error_listeners = handle->error_listeners;
                    }
                    for (auto& cb : error_listeners) {
                        if (cb) schedule_pipe_callback(cb, {Value{std::string("Failed to initialize pipe")}});
                    }
                    return;
                }

                // Start reading if reader mode
                if (is_reader) {
                    uv_read_start((uv_stream_t*)handle->pipe, alloc_ipc_cb, pipe_read_cb);
                }

                g_active_pipes.fetch_add(1);

                // Mark as ready
                handle->ready.store(true);

                // Notify ready listeners
                std::vector<FunctionPtr> ready_listeners;
                {
                    std::lock_guard<std::mutex> lk(handle->listeners_mutex);
                    ready_listeners = handle->ready_listeners;
                }
                for (auto& cb : ready_listeners) {
                    if (cb) schedule_pipe_callback(cb, {});
                }

                // Flush pending writes (for writers)
                if (!is_reader) {
                    std::vector<PipeHandle::PendingWrite> writes_to_execute;
                    {
                        std::lock_guard<std::mutex> lk(handle->pending_mutex);
                        writes_to_execute = std::move(handle->pending_writes);
                        handle->pending_writes.clear();
                    }

                    for (auto& pending : writes_to_execute) {
                        execute_write(handle.get(), pending.data, pending.callback);
                    }
                }
            });

            // Create pipe object
            auto pipe_obj = std::make_shared<ObjectValue>();
            Token tok;
            tok.loc = TokenLocation("<ipc>", 0, 0, 0);

            // on(event, callback)
            auto on_fn = make_ipc_fn("pipe.on", [handle](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.size() < 2) {
                    throw SwaziError("TypeError", "on() requires event and callback", token.loc);
                }
                
                std::string event = value_to_string_ipc(args[0]);
                if (!std::holds_alternative<FunctionPtr>(args[1])) {
                    throw SwaziError("TypeError", "callback must be a function", token.loc);
                }
                
                FunctionPtr cb = std::get<FunctionPtr>(args[1]);
                
                std::lock_guard<std::mutex> lk(handle->listeners_mutex);
                if (event == "data") {
                    handle->data_listeners.push_back(cb);
                } else if (event == "end") {
                    handle->end_listeners.push_back(cb);
                } else if (event == "error") {
                    handle->error_listeners.push_back(cb);
                } else if (event == "ready") {
                    handle->ready_listeners.push_back(cb);
                    // If already ready, call immediately
                    if (handle->ready.load()) {
                        schedule_pipe_callback(cb, {});
                    }
                }
                
                return std::monostate{}; }, nullptr);
            pipe_obj->properties["on"] = PropertyDescriptor{on_fn, false, false, false, tok};

            // write(data, callback?)
            auto write_fn = make_ipc_fn("pipe.write", [handle](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (handle->is_reader) {
                    throw SwaziError("IOError", "Cannot write to read-only pipe", token.loc);
                }
                
                if (args.empty()) {
                    throw SwaziError("TypeError", "write() requires data argument", token.loc);
                }
                
                if (handle->closed.load()) {
                    throw SwaziError("IOError", "Pipe is closed", token.loc);
                }
                
                // Convert data to bytes
                std::vector<uint8_t> data_bytes;
                
                if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    data_bytes.assign(str.begin(), str.end());
                } else if (std::holds_alternative<BufferPtr>(args[0])) {
                    BufferPtr buf = std::get<BufferPtr>(args[0]);
                    data_bytes = buf->data;
                } else {
                    throw SwaziError("TypeError", "write() requires string or buffer", token.loc);
                }
                
                if (data_bytes.empty()) {
                    return Value{true};
                }
                
                // Optional callback
                FunctionPtr callback = nullptr;
                if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                    callback = std::get<FunctionPtr>(args[1]);
                }
                
                // If pipe not ready yet, queue the write
                if (!handle->ready.load()) {
                    std::lock_guard<std::mutex> lk(handle->pending_mutex);
                    handle->pending_writes.push_back({data_bytes, callback});
                    return Value{true};
                }
                
                // Pipe is ready, write immediately on the loop thread
                scheduler_run_on_loop([handle, data_bytes, callback]() {
                    if (handle->pipe && !handle->closed.load()) {
                        execute_write(handle.get(), data_bytes, callback);
                    } else if (callback) {
                        schedule_pipe_callback(callback, {Value{std::string("Pipe closed before write")}});
                    }
                });
                
                return Value{true}; }, nullptr);
            pipe_obj->properties["write"] = PropertyDescriptor{write_fn, false, false, false, tok};

            // close()
            auto close_fn = make_ipc_fn("pipe.close", [handle](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (handle->closed.exchange(true)) {
                    return std::monostate{};
                }
                
                scheduler_run_on_loop([handle]() {
                    if (handle->pipe) {
                        uv_read_stop((uv_stream_t*)handle->pipe);
                        
                        uv_close((uv_handle_t*)handle->pipe, [](uv_handle_t* h) {
                            PipeHandle* handle = static_cast<PipeHandle*>(h->data);
                            if (handle) {
                                g_active_pipes.fetch_sub(1);
                                
                                if (handle->fd >= 0) {
                                    close(handle->fd);
                                    handle->fd = -1;
                                }
                                
                                std::lock_guard<std::mutex> lk(g_pipes_mutex);
                                g_pipe_handles.erase(handle->id);
                            }
                            delete (uv_pipe_t*)h;
                        });
                        
                        handle->pipe = nullptr;
                    }
                });
                
                return std::monostate{}; }, nullptr);
            pipe_obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};

            // path property
            pipe_obj->properties["path"] = PropertyDescriptor{Value{path}, false, false, true, tok};

            // mode property
            pipe_obj->properties["mode"] = PropertyDescriptor{Value{mode}, false, false, true, tok};

            return Value{pipe_obj};
#else
            throw SwaziError("NotSupportedError",
                "ipc.openPipe is not supported on Windows (use named pipes)", token.loc);
#endif
        },
            env);
        obj->properties["openPipe"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}

bool ipc_has_active_work() {
    return g_active_pipes.load() > 0;
}