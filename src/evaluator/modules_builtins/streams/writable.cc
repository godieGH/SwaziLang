#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "./streams.h"
#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// ============================================================================
// GLOBAL STATE DEFINITIONS
// ============================================================================
std::mutex g_writable_streams_mutex;
static std::atomic<long long> g_next_writable_stream_id{1};
std::unordered_map<long long, WritableStreamStatePtr> g_writable_streams;

// ============================================================================
// WRITABLE STREAM OPTIONS
// ============================================================================

struct WritableOptions {
    size_t high_water_mark = 65536;  // 64KB default
    std::string flags = "w";         // w or a ... truncate or append
    std::string encoding = "utf8";   // if binary writes to the file in binary mode
    bool auto_destroy = true;
};

static WritableOptions parse_writable_options(const Value& opts_val) {
    WritableOptions opts;
    if (!std::holds_alternative<ObjectPtr>(opts_val)) {
        return opts;
    }

    ObjectPtr opts_obj = std::get<ObjectPtr>(opts_val);

    // highWaterMark
    auto hwm_it = opts_obj->properties.find("highWaterMark");
    if (hwm_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(hwm_it->second.value)) {
            double val = std::get<double>(hwm_it->second.value);
            if (val > 0 && val <= 50e6) {
                opts.high_water_mark = static_cast<size_t>(val);
            }
        }
    }

    // flags
    auto flags_it = opts_obj->properties.find("flags");
    if (flags_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(flags_it->second.value)) {
            std::string flags = std::get<std::string>(flags_it->second.value);
            if (flags == "w" || flags == "wx" || flags == "a" || flags == "ax" ||
                flags == "w+" || flags == "wx+" || flags == "a+" || flags == "ax+") {
                opts.flags = flags;
            }
        }
    }

    // encoding
    auto enc_it = opts_obj->properties.find("encoding");
    if (enc_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(enc_it->second.value)) {
            std::string enc = std::get<std::string>(enc_it->second.value);
            if (enc == "utf8" || enc == "utf-8" || enc == "binary") {
                opts.encoding = enc;
            }
        }
    }

    // autoDestroy
    auto ad_it = opts_obj->properties.find("autoDestroy");
    if (ad_it != opts_obj->properties.end()) {
        if (std::holds_alternative<bool>(ad_it->second.value)) {
            opts.auto_destroy = std::get<bool>(ad_it->second.value);
        }
    }

    return opts;
}

// ============================================================================
// HELPER: Convert flags string to open() flags
// ============================================================================

static int flags_to_open_mode(const std::string& flags) {
    if (flags == "w") return O_WRONLY | O_CREAT | O_TRUNC;
    if (flags == "wx") return O_WRONLY | O_CREAT | O_TRUNC | O_EXCL;
    if (flags == "a") return O_WRONLY | O_CREAT | O_APPEND;
    if (flags == "ax") return O_WRONLY | O_CREAT | O_APPEND | O_EXCL;
    if (flags == "w+") return O_RDWR | O_CREAT | O_TRUNC;
    if (flags == "wx+") return O_RDWR | O_CREAT | O_TRUNC | O_EXCL;
    if (flags == "a+") return O_RDWR | O_CREAT | O_APPEND;
    if (flags == "ax+") return O_RDWR | O_CREAT | O_APPEND | O_EXCL;

    return O_WRONLY | O_CREAT | O_TRUNC;  // default to 'w'
}

// ============================================================================
// EVENT EMISSION - SYNCHRONOUS
// ============================================================================

static void emit_writable_event_sync(WritableStreamStatePtr state,
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
    tok.loc = TokenLocation("<stream-event>", 0, 0, 0);

    for (const auto& cb : listeners) {
        if (cb) {
            try {
                state->evaluator->invoke_function(cb, args, state->env, tok);
            } catch (const SwaziError& e) {
                schedule_listener_call(nullptr, {Value{std::string(e.what())}});
            } catch (...) {
                // Ignore
            }
        }
    }
}

// ============================================================================
// WRITE CONTEXT
// ============================================================================

struct WriteContext {
    WritableStreamState* state;
    WriteChunk chunk;
    size_t offset;
};

// Forward declaration
void schedule_next_write(WritableStreamStatePtr state);

// ============================================================================
// WRITE COMPLETION CALLBACK
// ============================================================================

static void on_write_complete(uv_fs_t* req) {
    g_active_stream_operations.fetch_sub(1);

    WriteContext* ctx = static_cast<WriteContext*>(req->data);
    if (!ctx) {
        uv_fs_req_cleanup(req);
        delete req;
        return;
    }

    WritableStreamState* raw_state = ctx->state;
    ssize_t result = req->result;
    FunctionPtr callback = ctx->chunk.callback;

    WritableStreamStatePtr state;
    {
        std::lock_guard<std::mutex> lock(g_writable_streams_mutex);
        auto it = g_writable_streams.find(raw_state->id);
        if (it != g_writable_streams.end()) {
            state = it->second;
        }
    }

    uv_fs_req_cleanup(req);
    delete ctx;
    delete req;

    if (!state || state->destroyed) {
        return;
    }

    state->writing = false;

    if (result < 0) {
        std::string error_msg = std::string("Write error: ") + uv_strerror(static_cast<int>(result));
        emit_writable_event_sync(state, state->error_listeners, {Value{error_msg}});

        if (callback) {
            emit_writable_event_sync(state, {callback}, {Value{error_msg}});
        }

        if (state->auto_destroy) {
            state->destroyed = true;
            state->close_file();
            emit_writable_event_sync(state, state->close_listeners, {});
        }

        state->release_keepalive();
        return;
    }

    // Update bytes written
    state->bytes_written += result;

    // Call chunk callback if provided
    if (callback) {
        emit_writable_event_sync(state, {callback}, {});
    }

    // Check if we finished all writes and end was called
    if (state->ended && state->write_queue.empty() && !state->finished) {
        state->finished = true;
        emit_writable_event_sync(state, state->finish_listeners, {});

        if (state->auto_destroy) {
            state->close_file();
            emit_writable_event_sync(state, state->close_listeners, {});
        }

        state->release_keepalive();
        return;
    }

    // Continue draining if there are more chunks
    if (!state->write_queue.empty() && !state->corked) {
        schedule_next_write(state);
    } else {
        // Queue is empty or corked - emit drain if needed
        bool should_drain = state->draining;
        state->draining = false;

        if (should_drain && !state->drain_listeners.empty()) {
            emit_writable_event_sync(state, state->drain_listeners, {});
        }

        state->release_keepalive();
    }
}

// ============================================================================
// SCHEDULE NEXT WRITE
// ============================================================================

void schedule_next_write(WritableStreamStatePtr state) {
    if (!state || state->destroyed || state->writing || state->write_queue.empty() || state->corked) {
        return;
    }

    if (state->fd < 0) {
        emit_writable_event_sync(state, state->error_listeners,
            {Value{std::string("File not open")}});
        return;
    }

    state->writing = true;
    state->keep_alive();
    g_active_stream_operations.fetch_add(1);

    WriteChunk chunk = std::move(state->write_queue.front());
    state->write_queue.pop_front();
    state->buffered_size -= chunk.data.size();

    WriteContext* ctx = new WriteContext{state.get(), std::move(chunk), 0};
    uv_buf_t buf = uv_buf_init(ctx->chunk.data.data(), static_cast<unsigned int>(ctx->chunk.data.size()));

    uv_fs_t* req = new uv_fs_t;
    req->data = ctx;

    int result = uv_fs_write(scheduler_get_loop(), req, state->fd, &buf, 1, -1, on_write_complete);

    if (result < 0) {
        delete ctx;
        delete req;
        state->writing = false;
        g_active_stream_operations.fetch_sub(1);
        emit_writable_event_sync(state, state->error_listeners,
            {Value{std::string("Write failed: ") + uv_strerror(result)}});
        state->release_keepalive();
    }
}

// ============================================================================
// HELPER: Convert Value to bytes
// ============================================================================
static std::vector<char> value_to_bytes(const Value& val, const std::string& encoding) {
    if (std::holds_alternative<BufferPtr>(val)) {
        auto buf = std::get<BufferPtr>(val);
        // Convert uint8_t vector to char vector
        return std::vector<char>(buf->data.begin(), buf->data.end());
    }

    if (std::holds_alternative<std::string>(val)) {
        std::string str = std::get<std::string>(val);
        return std::vector<char>(str.begin(), str.end());
    }

    // Try to convert other types to string
    std::string str = value_to_string_simple(val);
    return std::vector<char>(str.begin(), str.end());
}

// ============================================================================
// PIPE CLEANUP
// ============================================================================

void cleanup_pipe(PipeContextPtr ctx) {
    if (!ctx || ctx->cleanup_done) return;
    ctx->cleanup_done = true;
    ctx->piping = false;

    // Remove event listeners from readable stream
    if (ctx->readable && ctx->data_handler) {
        auto& listeners = ctx->readable->data_listeners;
        listeners.erase(
            std::remove(listeners.begin(), listeners.end(), ctx->data_handler),
            listeners.end());
    }

    if (ctx->readable && ctx->end_handler) {
        auto& listeners = ctx->readable->end_listeners;
        listeners.erase(
            std::remove(listeners.begin(), listeners.end(), ctx->end_handler),
            listeners.end());
    }

    if (ctx->readable && ctx->error_handler) {
        auto& listeners = ctx->readable->error_listeners;
        listeners.erase(
            std::remove(listeners.begin(), listeners.end(), ctx->error_handler),
            listeners.end());
    }

    if (ctx->readable && ctx->close_handler) {
        auto& listeners = ctx->readable->close_listeners;
        listeners.erase(
            std::remove(listeners.begin(), listeners.end(), ctx->close_handler),
            listeners.end());
    }

    // Remove drain listener from writable stream
    if (ctx->writable && ctx->drain_handler) {
        auto& listeners = ctx->writable->drain_listeners;
        listeners.erase(
            std::remove(listeners.begin(), listeners.end(), ctx->drain_handler),
            listeners.end());
    }
}
// ============================================================================
// PIPE IMPLEMENTATION
// ============================================================================

Value implement_pipe(ReadableStreamStatePtr readable_state,
    WritableStreamStatePtr writable_state,
    bool end_on_finish,
    const Token& token) {
    if (!readable_state || !writable_state) {
        throw SwaziError("TypeError", "Invalid stream objects for pipe", token.loc);
    }

    if (readable_state->destroyed) {
        throw SwaziError("Error", "Cannot pipe from destroyed readable stream", token.loc);
    }

    if (writable_state->destroyed) {
        throw SwaziError("Error", "Cannot pipe to destroyed writable stream", token.loc);
    }

    if (writable_state->ended) {
        throw SwaziError("Error", "Cannot pipe to ended writable stream", token.loc);
    }

    // Create pipe context
    auto ctx = std::make_shared<PipeContext>();
    ctx->readable = readable_state;
    ctx->writable = writable_state;
    ctx->end_on_finish = end_on_finish;
    ctx->piping = true;

    Token evt_tok{};
    evt_tok.loc = TokenLocation("<pipe-event>", 0, 0, 0);

    // ========================================================================
    // DATA HANDLER - Write data to writable, handle backpressure
    // ========================================================================

    auto data_impl = [ctx](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (!ctx->piping || !ctx->writable || !ctx->readable) {
            return std::monostate{};
        }

        if (ctx->writable->destroyed || ctx->writable->ended) {
            cleanup_pipe(ctx);
            if (ctx->readable) {
                ctx->readable->pause();
            }
            return std::monostate{};
        }

        if (args.empty()) {
            return std::monostate{};
        }

        const Value& chunk = args[0];

        // Convert chunk to bytes for writing
        std::vector<char> bytes;

        if (std::holds_alternative<BufferPtr>(chunk)) {
            auto buf = std::get<BufferPtr>(chunk);
            bytes = std::vector<char>(buf->data.begin(), buf->data.end());
        } else if (std::holds_alternative<std::string>(chunk)) {
            std::string str = std::get<std::string>(chunk);
            bytes = std::vector<char>(str.begin(), str.end());
        } else {
            // Skip non-writable data
            return std::monostate{};
        }

        if (bytes.empty()) {
            return std::monostate{};
        }

        // Add to writable's queue
        WriteChunk write_chunk;
        write_chunk.data = std::move(bytes);
        write_chunk.callback = nullptr;

        ctx->writable->buffered_size += write_chunk.data.size();
        ctx->writable->write_queue.push_back(std::move(write_chunk));

        bool needs_drain = (ctx->writable->buffered_size >= ctx->writable->high_water_mark);

        // Start writing if not already writing and not corked
        if (!ctx->writable->writing && !ctx->writable->corked &&
            !ctx->writable->write_queue.empty()) {
            schedule_next_write(ctx->writable);
        }

        // Handle backpressure - pause readable if writable buffer is full
        if (needs_drain) {
            ctx->writable->draining = true;
            if (ctx->readable && !ctx->readable->paused) {
                ctx->readable->pause();
            }
        }

        return std::monostate{};
    };

    ctx->data_handler = std::make_shared<FunctionValue>("pipe.data", data_impl, nullptr, evt_tok);
    readable_state->data_listeners.push_back(ctx->data_handler);

    // ========================================================================
    // DRAIN HANDLER - Resume readable when writable is ready
    // ========================================================================

    auto drain_impl = [ctx](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!ctx->piping || !ctx->readable || !ctx->writable) {
            return std::monostate{};
        }

        if (ctx->readable->paused && !ctx->readable->ended && !ctx->readable->destroyed) {
            ctx->readable->resume();
        }

        return std::monostate{};
    };

    ctx->drain_handler = std::make_shared<FunctionValue>("pipe.drain", drain_impl, nullptr, evt_tok);
    writable_state->drain_listeners.push_back(ctx->drain_handler);

    // ========================================================================
    // END HANDLER - End writable when readable ends
    // ========================================================================

    auto end_impl = [ctx](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!ctx->piping) {
            return std::monostate{};
        }

        if (ctx->writable && ctx->end_on_finish && !ctx->writable->ended) {
            ctx->writable->ended = true;

            // If queue is empty, finish immediately
            if (ctx->writable->write_queue.empty() && !ctx->writable->writing) {
                ctx->writable->finished = true;
                emit_writable_event_sync(ctx->writable, ctx->writable->finish_listeners, {});

                if (ctx->writable->auto_destroy) {
                    ctx->writable->close_file();
                    emit_writable_event_sync(ctx->writable, ctx->writable->close_listeners, {});
                }
            } else if (!ctx->writable->writing && !ctx->writable->corked) {
                // Start draining remaining writes
                schedule_next_write(ctx->writable);
            }
        }

        cleanup_pipe(ctx);
        return std::monostate{};
    };

    ctx->end_handler = std::make_shared<FunctionValue>("pipe.end", end_impl, nullptr, evt_tok);
    readable_state->end_listeners.push_back(ctx->end_handler);

    // ========================================================================
    // ERROR HANDLERS - Propagate errors and cleanup
    // ========================================================================

    auto error_impl = [ctx](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (!ctx->piping) {
            return std::monostate{};
        }

        // Propagate error to writable
        if (ctx->writable && !args.empty()) {
            emit_writable_event_sync(ctx->writable, ctx->writable->error_listeners, args);
        }

        cleanup_pipe(ctx);

        // Destroy writable on readable error
        if (ctx->writable && !ctx->writable->destroyed) {
            ctx->writable->destroyed = true;
            ctx->writable->close_file();
        }

        return std::monostate{};
    };

    ctx->error_handler = std::make_shared<FunctionValue>("pipe.error", error_impl, nullptr, evt_tok);
    readable_state->error_listeners.push_back(ctx->error_handler);

    // ========================================================================
    // CLOSE HANDLER - Cleanup on close
    // ========================================================================

    auto close_impl = [ctx](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        cleanup_pipe(ctx);
        return std::monostate{};
    };

    ctx->close_handler = std::make_shared<FunctionValue>("pipe.close", close_impl, nullptr, evt_tok);
    readable_state->close_listeners.push_back(ctx->close_handler);

    // ========================================================================
    // START FLOWING if not already
    // ========================================================================

    if (!readable_state->flowing && !readable_state->ended && !readable_state->destroyed) {
        readable_state->flowing = true;

        // Defer the first read to next tick
        uv_timer_t* timer = new uv_timer_t;
        ReadableStreamState* raw_state = readable_state.get();
        timer->data = raw_state;
        uv_timer_init(scheduler_get_loop(), timer);

        uv_timer_start(timer, [](uv_timer_t* handle) {
            ReadableStreamState* raw = static_cast<ReadableStreamState*>(handle->data);
            ReadableStreamStatePtr st;
            {
                std::lock_guard<std::mutex> lock(g_readable_streams_mutex);
                auto it = g_readable_streams.find(raw->id);
                if (it != g_readable_streams.end()) {
                    st = it->second;
                }
            }
            
            if (st && st->flowing && !st->paused && !st->ended && !st->destroyed) {
                schedule_next_read(st);
            }
            
            uv_close((uv_handle_t*)handle, [](uv_handle_t* h) {
                delete (uv_timer_t*)h;
            }); }, 0, 0);
    }

    // Return the writable stream for chaining
    return Value{create_writable_stream_object(writable_state)};
}

// ============================================================================
// CREATE WRITABLE STREAM OBJECT
// ============================================================================

ObjectPtr create_writable_stream_object(WritableStreamStatePtr state) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    // ========================================================================
    // write(data, [encoding], [callback])
    // ========================================================================

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

        // Parse arguments: write(data) or write(data, callback) or write(data, encoding, callback)
        const Value& data = args[0];
        FunctionPtr callback = nullptr;
        std::string encoding = state->encoding;

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

        // Convert data to bytes
        std::vector<char> bytes = value_to_bytes(data, encoding);

        if (bytes.empty()) {
            // Empty write - just call callback if provided
            if (callback) {
                emit_writable_event_sync(state, {callback}, {});
            }
            return Value{true};
        }

        // Add to write queue
        WriteChunk chunk;
        chunk.data = std::move(bytes);
        chunk.callback = callback;

        state->buffered_size += chunk.data.size();
        state->write_queue.push_back(std::move(chunk));

        // Check if we should start draining
        bool needs_drain = (state->buffered_size >= state->high_water_mark);

        if (needs_drain) {
            state->draining = true;
        }

        // Start writing if not already writing and not corked
        if (!state->writing && !state->corked && !state->write_queue.empty()) {
            schedule_next_write(state);
        }

        // Return false if buffer is full (backpressure signal)
        return Value{!needs_drain};
    };
    obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("stream.write", write_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // end([finalChunk], [callback])
    // ========================================================================

    auto end_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->destroyed) {
            throw SwaziError("Error", "Cannot end destroyed stream", token.loc);
        }

        if (state->ended) {
            return std::monostate{};
        }

        state->ended = true;

        FunctionPtr callback = nullptr;

        // If final chunk provided, write it first
        if (!args.empty() && !std::holds_alternative<std::monostate>(args[0])) {
            if (std::holds_alternative<FunctionPtr>(args[0])) {
                callback = std::get<FunctionPtr>(args[0]);
            } else {
                // Write final chunk
                std::vector<char> bytes = value_to_bytes(args[0], state->encoding);
                if (!bytes.empty()) {
                    WriteChunk chunk;
                    chunk.data = std::move(bytes);
                    state->write_queue.push_back(std::move(chunk));
                }
            }
        }

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        // Add callback to finish listeners
        if (callback) {
            state->finish_listeners.push_back(callback);
        }

        // If queue is empty, finish immediately
        if (state->write_queue.empty() && !state->writing) {
            state->finished = true;
            emit_writable_event_sync(state, state->finish_listeners, {});

            if (state->auto_destroy) {
                state->close_file();
                emit_writable_event_sync(state, state->close_listeners, {});
            }
        } else if (!state->writing && !state->corked) {
            // Start draining
            schedule_next_write(state);
        }

        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("stream.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // destroy([error])
    // ========================================================================

    auto destroy_impl = [state](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (state->destroyed) {
            return std::monostate{};
        }

        state->destroyed = true;
        state->ended = true;

        // Clear write queue
        state->write_queue.clear();
        state->buffered_size = 0;

        // Close file
        state->close_file();

        // Emit error if provided
        if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
            emit_writable_event_sync(state, state->error_listeners, {args[0]});
        }

        // Emit close
        emit_writable_event_sync(state, state->close_listeners, {});

        // Remove from global map
        {
            std::lock_guard<std::mutex> lock(g_writable_streams_mutex);
            g_writable_streams.erase(state->id);
        }

        state->release_keepalive();
        return std::monostate{};
    };
    obj->properties["destroy"] = {
        Value{std::make_shared<FunctionValue>("stream.destroy", destroy_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // on(event, callback)
    // ========================================================================

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
        } else {
            throw SwaziError("TypeError", "Unknown event: " + event, token.loc);
        }

        return std::monostate{};
    };
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // cork()
    // ========================================================================

    auto cork_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        state->corked = true;
        state->cork_count++;
        return std::monostate{};
    };
    obj->properties["cork"] = {
        Value{std::make_shared<FunctionValue>("stream.cork", cork_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // uncork()
    // ========================================================================

    auto uncork_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (state->cork_count > 0) {
            state->cork_count--;
        }

        if (state->cork_count == 0) {
            state->corked = false;

            // Start draining if there are pending writes
            if (!state->write_queue.empty() && !state->writing) {
                schedule_next_write(state);
            }
        }

        return std::monostate{};
    };
    obj->properties["uncork"] = {
        Value{std::make_shared<FunctionValue>("stream.uncork", uncork_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // Property getters
    // ========================================================================

    auto is_ended_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->ended};
    };
    obj->properties["isEnded"] = {
        Value{std::make_shared<FunctionValue>("stream.isEnded", is_ended_impl, nullptr, tok)},
        false, true, true, tok};

    auto is_finished_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->finished};
    };
    obj->properties["isFinished"] = {
        Value{std::make_shared<FunctionValue>("stream.isFinished", is_finished_impl, nullptr, tok)},
        false, true, true, tok};

    auto bytes_written_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{static_cast<double>(state->bytes_written)};
    };
    obj->properties["bytesWritten"] = {
        Value{std::make_shared<FunctionValue>("stream.bytesWritten", bytes_written_impl, nullptr, tok)},
        false, true, true, tok};

    // Metadata properties
    obj->properties["highWaterMark"] = {Value{static_cast<double>(state->high_water_mark)}, false, false, true, tok};
    obj->properties["encoding"] = {Value{state->encoding}, false, false, true, tok};
    obj->properties["filePath"] = {Value{state->path}, false, false, true, tok};
    obj->properties["_fd"] = {Value{static_cast<double>(state->fd)}, false, false, true, tok};
    obj->properties["_id"] = {Value{static_cast<double>(state->id)}, false, false, true, tok};

    auto stream_events_arr = std::make_shared<ArrayValue>();
    stream_events_arr->elements.push_back(Value(std::string("drain")));
    stream_events_arr->elements.push_back(Value(std::string("finish")));
    stream_events_arr->elements.push_back(Value(std::string("error")));
    stream_events_arr->elements.push_back(Value(std::string("close")));
    obj->properties["_events"] = {stream_events_arr, false, false, true, tok};

    return obj;
}

// ============================================================================
// FACTORY FUNCTION
// ============================================================================

Value native_createWriteStream(const std::vector<Value>& args, EnvPtr env, Evaluator* evaluator, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "streams.createWritable requires path argument", token.loc);
    }

    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "path must be a string", token.loc);
    }

    std::string path = std::get<std::string>(args[0]);
    if (path.empty()) {
        throw SwaziError("TypeError", "path cannot be empty", token.loc);
    }

    WritableOptions opts;
    if (args.size() >= 2) {
        opts = parse_writable_options(args[1]);
    }

    int open_flags = flags_to_open_mode(opts.flags);
    int mode = 0644;  // rw-r--r--

    uv_fs_t open_req;
    int fd = uv_fs_open(scheduler_get_loop(), &open_req, path.c_str(), open_flags, mode, NULL);
    uv_fs_req_cleanup(&open_req);

    if (fd < 0) {
        throw SwaziError("IOError", "Failed to open file '" + path + "': " + uv_strerror(fd), token.loc);
    }

    auto state = std::make_shared<WritableStreamState>();
    state->id = g_next_writable_stream_id.fetch_add(1);
    state->fd = fd;
    state->path = path;
    state->high_water_mark = opts.high_water_mark;
    state->encoding = opts.encoding;
    state->auto_destroy = opts.auto_destroy;
    state->env = env;
    state->evaluator = evaluator;

    {
        std::lock_guard<std::mutex> lock(g_writable_streams_mutex);
        g_writable_streams[state->id] = state;
    }

    return Value{create_writable_stream_object(state)};
}
