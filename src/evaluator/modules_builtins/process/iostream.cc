#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../streams/streams.h"
#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

#ifndef _WIN32
#include <unistd.h>
#endif

static std::string value_to_string_simple_iostream(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// ============================================================================
// STDOUT/STDERR STREAM STATE
// ============================================================================

struct StdStreamState : public std::enable_shared_from_this<StdStreamState> {
    long long id;
    int fd;            // 1 for stdout, 2 for stderr
    std::string name;  // "stdout" or "stderr"

    uv_tty_t* tty_handle = nullptr;
    uv_pipe_t* pipe_handle = nullptr;  // For non-TTY (piped) output
    bool is_tty = false;

    std::atomic<bool> writing{false};
    std::atomic<bool> destroyed{false};
    std::atomic<bool> ended{false};

    struct WriteChunk {
        std::vector<char> data;
        FunctionPtr callback;
    };

    std::deque<WriteChunk> write_queue;
    size_t buffered_size = 0;
    size_t high_water_mark = 16384;  // 16KB for terminal output
    size_t bytes_written = 0;

    std::atomic<bool> draining{false};
    std::atomic<bool> corked{false};
    int cork_count = 0;

    std::vector<FunctionPtr> drain_listeners;
    std::vector<FunctionPtr> finish_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> close_listeners;

    EnvPtr env;
    Evaluator* evaluator = nullptr;

    // Keepalive
    uv_async_t* keepalive = nullptr;

    void keep_alive() {
        if (!keepalive) {
            keepalive = new uv_async_t;
            uv_async_init(scheduler_get_loop(), keepalive, [](uv_async_t*) {});
        }
    }

    void release_keepalive() {
        if (keepalive) {
            uv_close((uv_handle_t*)keepalive, [](uv_handle_t* h) {
                delete (uv_async_t*)h;
            });
            keepalive = nullptr;
        }
    }

    ~StdStreamState() {
        if (tty_handle) {
            uv_close((uv_handle_t*)tty_handle, [](uv_handle_t* h) {
                delete (uv_tty_t*)h;
            });
        }
        if (pipe_handle) {
            uv_close((uv_handle_t*)pipe_handle, [](uv_handle_t* h) {
                delete (uv_pipe_t*)h;
            });
        }
        release_keepalive();
    }
};

using StdStreamStatePtr = std::shared_ptr<StdStreamState>;

static std::mutex g_std_streams_mutex;
static std::unordered_map<long long, StdStreamStatePtr> g_std_streams;

// ============================================================================
// EVENT EMISSION
// ============================================================================

static void emit_std_stream_event(StdStreamStatePtr state,
    const std::vector<FunctionPtr>& listeners,
    const std::vector<Value>& args) {
    if (!state || !state->env || !state->evaluator) {
        for (const auto& cb : listeners) {
            if (cb) {
                schedule_listener_call(cb, args);
            }
        }
        return;
    }

    Token tok{};
    tok.loc = TokenLocation("<std-stream>", 0, 0, 0);

    for (const auto& cb : listeners) {
        if (cb) {
            try {
                state->evaluator->invoke_function(cb, args, state->env, tok);
            } catch (...) {
                // Ignore errors in listeners
            }
        }
    }
}

// ============================================================================
// WRITE OPERATIONS
// ============================================================================

struct StdWriteContext {
    StdStreamState* state;
    StdStreamState::WriteChunk chunk;
};

static void on_std_write_complete(uv_write_t* req, int status);

static void schedule_next_std_write(StdStreamStatePtr state) {
    if (!state || state->destroyed || state->writing || state->write_queue.empty() || state->corked) {
        return;
    }

    if (!state->tty_handle && !state->pipe_handle) {
        emit_std_stream_event(state, state->error_listeners,
            {Value{std::string("Stream not initialized")}});
        return;
    }

    state->writing = true;
    state->keep_alive();
    g_active_stream_operations.fetch_add(1);

    StdStreamState::WriteChunk chunk = std::move(state->write_queue.front());
    state->write_queue.pop_front();
    state->buffered_size -= chunk.data.size();

    StdWriteContext* ctx = new StdWriteContext{state.get(), std::move(chunk)};
    uv_buf_t buf = uv_buf_init(ctx->chunk.data.data(), static_cast<unsigned int>(ctx->chunk.data.size()));

    uv_write_t* req = new uv_write_t;
    req->data = ctx;

    uv_stream_t* stream = state->is_tty ? (uv_stream_t*)state->tty_handle : (uv_stream_t*)state->pipe_handle;

    int result = uv_write(req, stream, &buf, 1, on_std_write_complete);

    if (result < 0) {
        delete ctx;
        delete req;
        state->writing = false;
        g_active_stream_operations.fetch_sub(1);
        emit_std_stream_event(state, state->error_listeners,
            {Value{std::string("Write failed: ") + uv_strerror(result)}});
        state->release_keepalive();
    }
}

static void on_std_write_complete(uv_write_t* req, int status) {
    g_active_stream_operations.fetch_sub(1);

    StdWriteContext* ctx = static_cast<StdWriteContext*>(req->data);
    if (!ctx) {
        delete req;
        return;
    }

    StdStreamState* raw_state = ctx->state;
    FunctionPtr callback = ctx->chunk.callback;

    StdStreamStatePtr state;
    {
        std::lock_guard<std::mutex> lock(g_std_streams_mutex);
        auto it = g_std_streams.find(raw_state->id);
        if (it != g_std_streams.end()) {
            state = it->second;
        }
    }

    delete ctx;
    delete req;

    if (!state || state->destroyed) {
        return;
    }

    state->writing = false;

    if (status < 0) {
        std::string error_msg = std::string("Write error: ") + uv_strerror(status);
        emit_std_stream_event(state, state->error_listeners, {Value{error_msg}});

        if (callback) {
            emit_std_stream_event(state, {callback}, {Value{error_msg}});
        }

        state->release_keepalive();
        return;
    }

    state->bytes_written += static_cast<size_t>(status);

    if (callback) {
        emit_std_stream_event(state, {callback}, {});
    }

    if (state->ended && state->write_queue.empty()) {
        emit_std_stream_event(state, state->finish_listeners, {});
        state->release_keepalive();
        return;
    }

    if (!state->write_queue.empty() && !state->corked) {
        schedule_next_std_write(state);
    } else {
        bool should_drain = state->draining;
        state->draining = false;

        if (should_drain && !state->drain_listeners.empty()) {
            emit_std_stream_event(state, state->drain_listeners, {});
        }

        state->release_keepalive();
    }
}

// ============================================================================
// CONVERT VALUE TO BYTES
// ============================================================================

static std::vector<char> std_value_to_bytes(const Value& val, const std::string& encoding) {
    if (std::holds_alternative<BufferPtr>(val)) {
        auto buf = std::get<BufferPtr>(val);
        return std::vector<char>(buf->data.begin(), buf->data.end());
    }

    if (std::holds_alternative<std::string>(val)) {
        std::string str = std::get<std::string>(val);
        return std::vector<char>(str.begin(), str.end());
    }

    std::string str = value_to_string_simple_iostream(val);
    return std::vector<char>(str.begin(), str.end());
}

// ============================================================================
// CREATE STREAM OBJECT
// ============================================================================

static ObjectPtr create_std_stream_object(StdStreamStatePtr state) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<std-stream>", 0, 0, 0);

    // write(data, [encoding], [callback])
    auto write_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->destroyed) {
            throw SwaziError("Error", "Cannot write to destroyed stream", token.loc);
        }

        if (state->ended) {
            throw SwaziError("Error", "Cannot write after end", token.loc);
        }

        if (args.empty()) {
            throw SwaziError("TypeError", "write() requires data argument", token.loc);
        }

        const Value& data = args[0];
        FunctionPtr callback = nullptr;
        std::string encoding = "utf8";

        if (args.size() >= 2) {
            if (std::holds_alternative<FunctionPtr>(args[1])) {
                callback = std::get<FunctionPtr>(args[1]);
            } else if (std::holds_alternative<std::string>(args[1])) {
                encoding = std::get<std::string>(args[1]);
            }
        }

        if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2])) {
            callback = std::get<FunctionPtr>(args[2]);
        }

        std::vector<char> bytes = std_value_to_bytes(data, encoding);

        if (bytes.empty()) {
            if (callback) {
                emit_std_stream_event(state, {callback}, {});
            }
            return Value{true};
        }

        StdStreamState::WriteChunk chunk;
        chunk.data = std::move(bytes);
        chunk.callback = callback;

        state->buffered_size += chunk.data.size();
        state->write_queue.push_back(std::move(chunk));

        bool needs_drain = (state->buffered_size >= state->high_water_mark);

        if (needs_drain) {
            state->draining = true;
        }

        if (!state->writing && !state->corked && !state->write_queue.empty()) {
            schedule_next_std_write(state);
        }

        return Value{!needs_drain};
    };
    obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("stream.write", write_impl, nullptr, tok)},
        false, false, false, tok};

    // end([finalChunk], [callback])
    auto end_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->destroyed) {
            throw SwaziError("Error", "Cannot end destroyed stream", token.loc);
        }

        if (state->ended) {
            return std::monostate{};
        }

        state->ended = true;

        FunctionPtr callback = nullptr;

        if (!args.empty() && !std::holds_alternative<std::monostate>(args[0])) {
            if (std::holds_alternative<FunctionPtr>(args[0])) {
                callback = std::get<FunctionPtr>(args[0]);
            } else {
                std::vector<char> bytes = std_value_to_bytes(args[0], "utf8");
                if (!bytes.empty()) {
                    StdStreamState::WriteChunk chunk;
                    chunk.data = std::move(bytes);
                    state->write_queue.push_back(std::move(chunk));
                }
            }
        }

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        if (callback) {
            state->finish_listeners.push_back(callback);
        }

        if (state->write_queue.empty() && !state->writing) {
            emit_std_stream_event(state, state->finish_listeners, {});
        } else if (!state->writing && !state->corked) {
            schedule_next_std_write(state);
        }

        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("stream.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // on(event, callback)
    auto on_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "stream.on requires (event, callback)", token.loc);
        }

        if (!std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", "event must be string", token.loc);
        }

        if (!std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "callback must be function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr cb = std::get<FunctionPtr>(args[1]);

        if (event == "drain") {
            state->drain_listeners.push_back(cb);
        } else if (event == "finish") {
            state->finish_listeners.push_back(cb);
        } else if (event == "error") {
            state->error_listeners.push_back(cb);
        } else if (event == "close") {
            state->close_listeners.push_back(cb);
        }

        return std::monostate{};
    };
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // cork()
    auto cork_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        state->corked = true;
        state->cork_count++;
        return std::monostate{};
    };
    obj->properties["cork"] = {
        Value{std::make_shared<FunctionValue>("stream.cork", cork_impl, nullptr, tok)},
        false, false, false, tok};

    // uncork()
    auto uncork_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (state->cork_count > 0) {
            state->cork_count--;
        }

        if (state->cork_count == 0) {
            state->corked = false;

            if (!state->write_queue.empty() && !state->writing) {
                schedule_next_std_write(state);
            }
        }

        return std::monostate{};
    };
    obj->properties["uncork"] = {
        Value{std::make_shared<FunctionValue>("stream.uncork", uncork_impl, nullptr, tok)},
        false, false, false, tok};

    // isTTY property
    obj->properties["isTTY"] = {Value{state->is_tty}, false, false, true, tok};

    // _id property
    obj->properties["_id"] = {Value{static_cast<double>(state->id)}, false, false, true, tok};

    // _events array
    auto events_arr = std::make_shared<ArrayValue>();
    events_arr->elements.push_back(Value(std::string("drain")));
    events_arr->elements.push_back(Value(std::string("finish")));
    events_arr->elements.push_back(Value(std::string("error")));
    events_arr->elements.push_back(Value(std::string("close")));
    obj->properties["_events"] = {events_arr, false, false, true, tok};

    return obj;
}

// ============================================================================
// FACTORY FUNCTIONS
// ============================================================================

static StdStreamStatePtr create_std_stream(int fd, const std::string& name, EnvPtr env, Evaluator* evaluator) {
    auto state = std::make_shared<StdStreamState>();
    state->id = g_next_stream_id.fetch_add(1);
    state->fd = fd;
    state->name = name;
    state->env = env;
    state->evaluator = evaluator;

    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) {
        return state;
    }

    // Check if fd is a TTY
    uv_handle_type type = uv_guess_handle(fd);
    state->is_tty = (type == UV_TTY);

    if (state->is_tty) {
        state->tty_handle = new uv_tty_t;
        int r = uv_tty_init(loop, state->tty_handle, fd, 0);
        if (r != 0) {
            delete state->tty_handle;
            state->tty_handle = nullptr;
            state->is_tty = false;
        } else {
            uv_tty_set_mode(state->tty_handle, UV_TTY_MODE_NORMAL);
        }
    }

    if (!state->is_tty) {
        state->pipe_handle = new uv_pipe_t;
        int r = uv_pipe_init(loop, state->pipe_handle, 0);
        if (r != 0) {
            delete state->pipe_handle;
            state->pipe_handle = nullptr;
        } else {
            r = uv_pipe_open(state->pipe_handle, fd);
            if (r != 0) {
                delete state->pipe_handle;
                state->pipe_handle = nullptr;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_std_streams_mutex);
        g_std_streams[state->id] = state;
    }

    return state;
}

Value native_createStdout(EnvPtr env, Evaluator* evaluator) {
    auto state = create_std_stream(1, "stdout", env, evaluator);
    return Value{create_std_stream_object(state)};
}

Value native_createStderr(EnvPtr env, Evaluator* evaluator) {
    auto state = create_std_stream(2, "stderr", env, evaluator);
    return Value{create_std_stream_object(state)};
}
