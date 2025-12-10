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

std::mutex g_readable_streams_mutex;
// static std::atomic<long long> g_next_readable_stream_id{1};
std::unordered_map<long long, ReadableStreamStatePtr> g_readable_streams;

// ============================================================================
// STREAM OPTIONS
// ============================================================================
struct StreamOptions {
    size_t high_water_mark = 65536;  // 64KB default
    std::string encoding = "binary";
    bool auto_close = true;
    size_t start = 0;
    size_t end = 0;  // 0 = EOF
    double speed = 1.0;
};

static StreamOptions parse_stream_options(const Value& opts_val) {
    StreamOptions opts;
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

    // encoding
    auto enc_it = opts_obj->properties.find("encoding");
    if (enc_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(enc_it->second.value)) {
            std::string enc = std::get<std::string>(enc_it->second.value);
            if (enc == "utf8" || enc == "utf-8" || enc == "binary") {
                opts.encoding = enc;
            }
        } else if (std::holds_alternative<std::monostate>(enc_it->second.value)) {
            opts.encoding = "binary";
        }
    }

    // autoClose
    auto ac_it = opts_obj->properties.find("autoClose");
    if (ac_it != opts_obj->properties.end()) {
        if (std::holds_alternative<bool>(ac_it->second.value)) {
            opts.auto_close = std::get<bool>(ac_it->second.value);
        }
    }

    // start
    auto start_it = opts_obj->properties.find("start");
    if (start_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(start_it->second.value)) {
            double val = std::get<double>(start_it->second.value);
            if (val >= 0) {
                opts.start = static_cast<size_t>(val);
            }
        }
    }

    // end
    auto end_it = opts_obj->properties.find("end");
    if (end_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(end_it->second.value)) {
            double val = std::get<double>(end_it->second.value);
            if (val >= 0) {
                opts.end = static_cast<size_t>(val);
            }
        }
    }

    // speed
    auto speed_it = opts_obj->properties.find("speed");
    if (speed_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(speed_it->second.value)) {
            double val = std::get<double>(speed_it->second.value);
            if (val > 0) {
                opts.speed = val;
            }
        }
    }

    return opts;
}

// ============================================================================
// READABLE STREAM STATE
// ============================================================================

struct ReadContext {
    ReadableStreamState* state;
    char* buffer;
    size_t buffer_size;
};

// ============================================================================
// EVENT EMISSION - SYNCHRONOUS VERSION
// ============================================================================

static void emit_readable_event_sync(ReadableStreamStatePtr state,
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

    for (const auto& cb : listeners) {
        if (cb) {
            try {
                state->evaluator->invoke_function(cb, args, state->env, cb->token);
            } catch (const SwaziError& e) {
                std::cerr << "Unhandled error in stream listener: "
                          << e.what() << std::endl;

                // Destroy stream to prevent further reads
                state->destroyed = true;
                state->ended = true;
                state->paused = true;
                state->close_file();

                // Emit error event once (if not already in error listener)
                if (&listeners != &state->error_listeners && !state->error_listeners.empty()) {
                    try {
                        emit_readable_event_sync(state, state->error_listeners,
                            {Value{std::string(e.what())}});
                    } catch (...) {
                        // Error listener also threw - silently ignore
                    }
                }

                // Stop processing remaining callbacks
                return;

            } catch (const std::exception& e) {
                std::cerr << "Unhandled error in stream listener: "
                          << e.what() << std::endl;

                state->destroyed = true;
                state->ended = true;
                state->paused = true;
                state->close_file();
                return;

            } catch (...) {
                std::cerr << "Unknown error in stream listener" << std::endl;

                state->destroyed = true;
                state->ended = true;
                state->paused = true;
                state->close_file();
                return;
            }
        }
    }
}
// ============================================================================
// ASYNC READ OPERATIONS
// ============================================================================

static void on_read_complete(uv_fs_t* req) {
    g_active_stream_operations.fetch_sub(1);

    ReadContext* ctx = static_cast<ReadContext*>(req->data);
    if (!ctx) {
        uv_fs_req_cleanup(req);
        delete req;
        return;
    }

    ReadableStreamState* raw_state = ctx->state;
    char* buffer_data = ctx->buffer;
    ssize_t result = req->result;

    ReadableStreamStatePtr state;
    {
        std::lock_guard<std::mutex> lock(g_readable_streams_mutex);
        auto it = g_readable_streams.find(raw_state->id);
        if (it != g_readable_streams.end()) {
            state = it->second;
        }
    }

    uv_fs_req_cleanup(req);
    delete ctx;
    delete req;

    if (!state || state->destroyed) {
        if (buffer_data) free(buffer_data);
        return;
    }

    state->reading = false;

    if (result < 0) {
        if (buffer_data) free(buffer_data);
        std::string error_msg = std::string("Read error: ") + uv_strerror(static_cast<int>(result));
        emit_readable_event_sync(state, state->error_listeners, {Value{error_msg}});
        if (state->auto_close) {
            state->close_file();
        }
        state->ended = true;
        state->release_keepalive();
        return;
    }

    if (result == 0) {
        if (buffer_data) free(buffer_data);
        state->ended = true;
        emit_readable_event_sync(state, state->end_listeners, {});
        if (state->auto_close) {
            state->close_file();
            emit_readable_event_sync(state, state->close_listeners, {});
        }
        state->release_keepalive();
        return;
    }

    auto chunk = std::make_shared<BufferValue>();
    chunk->data.assign(buffer_data, buffer_data + result);
    chunk->encoding = state->encoding;
    free(buffer_data);

    state->current_position += result;
    bool reached_end = (state->current_position >= state->stream_end);
    Value encoded_chunk = encode_buffer_for_emission(chunk, state->encoding);

    // CRITICAL: Call callbacks SYNCHRONOUSLY
    // When this returns, if user called pause(), state->paused will be true
    emit_readable_event_sync(state, state->data_listeners, {encoded_chunk});

    if (reached_end) {
        state->ended = true;
        emit_readable_event_sync(state, state->end_listeners, {});
        if (state->auto_close) {
            state->close_file();
            emit_readable_event_sync(state, state->close_listeners, {});
        }
        state->release_keepalive();
        return;
    }

    // NOW check if user paused - this reflects their pause() call from callback
    if (state->paused || !state->flowing) {
        state->release_keepalive();
        return;
    }

    // User didn't pause, so continue reading (with speed throttling if set)
    if (state->speed > 1.0) {
        uv_timer_t* timer = new uv_timer_t;
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

            if (st && !st->paused && st->flowing && !st->ended && !st->destroyed) {
                schedule_next_read(st);
            } else if (st) {
                st->release_keepalive();
            }

            uv_close((uv_handle_t*)handle, [](uv_handle_t* h) {
                delete (uv_timer_t*)h;
            }); }, static_cast<uint64_t>(state->speed), 0);
    } else {
        // No speed delay - read next chunk immediately
        schedule_next_read(state);
    }
}
void schedule_next_read(ReadableStreamStatePtr state) {
    if (!state || state->destroyed || state->ended || state->paused || state->reading) {
        return;
    }

    if (state->fd < 0 || state->current_position >= state->stream_end) {
        state->ended = true;
        emit_readable_event_sync(state, state->end_listeners, {});  // ✅ FIXED
        if (state->auto_close) {
            state->close_file();
        }
        return;
    }

    state->reading = true;
    state->keep_alive();
    g_active_stream_operations.fetch_add(1);

    size_t remaining = state->stream_end - state->current_position;
    size_t to_read = std::min(state->high_water_mark, remaining);

    char* buffer = (char*)malloc(to_read);
    if (!buffer) {
        state->reading = false;
        g_active_stream_operations.fetch_sub(1);
        emit_readable_event_sync(state, state->error_listeners,  // ✅ FIXED
            {Value{std::string("Memory allocation failed")}});
        state->release_keepalive();
        return;
    }

    ReadContext* ctx = new ReadContext{state.get(), buffer, to_read};
    uv_buf_t buf = uv_buf_init(buffer, static_cast<unsigned int>(to_read));

    uv_fs_t* req = new uv_fs_t;
    req->data = ctx;

    int result = uv_fs_read(scheduler_get_loop(), req, state->fd, &buf, 1,
        state->current_position, on_read_complete);

    if (result < 0) {
        free(buffer);
        delete ctx;
        delete req;
        state->reading = false;
        g_active_stream_operations.fetch_sub(1);
        emit_readable_event_sync(state, state->error_listeners,  // ✅ FIXED
            {Value{std::string("Read failed: ") + uv_strerror(result)}});
        state->release_keepalive();
    }
}

// ============================================================================
// READABLE STREAM STATE METHODS
// ============================================================================

void ReadableStreamState::pause() {
    paused = true;
    flowing = false;
}

void ReadableStreamState::resume() {
    bool was_paused = paused;
    paused = false;
    flowing = true;

    if (was_paused && !ended && !destroyed && !reading) {
        schedule_next_read(shared_from_this());
    }
}

// ============================================================================
// STREAM OBJECT
// ============================================================================

ObjectPtr create_readable_stream_object(ReadableStreamStatePtr state) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

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

        if (event == "data") {
            bool is_first = state->data_listeners.empty();
            state->data_listeners.push_back(cb);
            if (is_first && !state->ended && !state->destroyed) {
                state->flowing = true;

                // CRITICAL: Defer the first read to next tick
                // This gives the user's code time to set up and potentially pause
                // before any data is emitted
                uv_timer_t* timer = new uv_timer_t;
                ReadableStreamState* raw_state = state.get();
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
                }); }, 0, 0);  // 0 delay = next tick
            }
        } else if (event == "end") {
            state->end_listeners.push_back(cb);
        } else if (event == "error") {
            state->error_listeners.push_back(cb);
        } else if (event == "close") {
            state->close_listeners.push_back(cb);
        }

        return std::monostate{};
    };
    obj->properties["on"] = {Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)}, false, false, true, tok};

    auto pause_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        state->paused = true;
        state->flowing = false;
        return std::monostate{};
    };
    obj->properties["pause"] = {Value{std::make_shared<FunctionValue>("stream.pause", pause_impl, nullptr, tok)}, false, false, true, tok};

    auto resume_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        bool was_paused = state->paused;
        state->paused = false;
        state->flowing = true;

        if (was_paused && !state->ended && !state->destroyed && !state->reading) {
            schedule_next_read(state);
        }
        return std::monostate{};
    };
    obj->properties["resume"] = {Value{std::make_shared<FunctionValue>("stream.resume", resume_impl, nullptr, tok)}, false, false, true, tok};

    auto destroy_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (state->destroyed) return std::monostate{};
        state->destroyed = true;
        state->ended = true;
        state->paused = true;
        state->close_file();
        emit_readable_event_sync(state, state->close_listeners, {});  // ✅ FIXED
        {
            std::lock_guard<std::mutex> lock(g_readable_streams_mutex);
            g_readable_streams.erase(state->id);
        }
        state->release_keepalive();
        return std::monostate{};
    };
    obj->properties["destroy"] = {Value{std::make_shared<FunctionValue>("stream.destroy", destroy_impl, nullptr, tok)}, false, false, true, tok};

    auto is_paused_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->paused};
    };
    obj->properties["isPaused"] = {Value{std::make_shared<FunctionValue>("stream.isPaused", is_paused_impl, nullptr, tok)}, false, true, true, tok};

    auto is_ended_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->ended};
    };
    obj->properties["isEnded"] = {Value{std::make_shared<FunctionValue>("stream.isEnded", is_ended_impl, nullptr, tok)}, false, true, true, tok};

    obj->properties["stream_start"] = {Value{(double)state->stream_start}, false, false, true, tok};
    obj->properties["stream_end"] = {Value{(double)state->stream_end}, false, false, true, tok};
    obj->properties["speed"] = {Value{(double)state->speed}, false, false, true, tok};
    obj->properties["encoding"] = {Value{state->encoding}, false, false, true, tok};
    obj->properties["chunkSize"] = {Value{(double)state->high_water_mark}, false, false, true, tok};
    obj->properties["filePath"] = {Value{state->path}, false, false, true, tok};
    obj->properties["fileSize"] = {Value{(double)state->file_size}, false, false, true, tok};
    obj->properties["_fd"] = {Value{(double)state->fd}, false, false, true, tok};
    obj->properties["_id"] = {Value{(double)state->id}, false, false, true, tok};

    auto stream_events_arr_val = std::make_shared<ArrayValue>();
    stream_events_arr_val->elements.push_back(Value(std::string("data")));
    stream_events_arr_val->elements.push_back(Value(std::string("end")));
    stream_events_arr_val->elements.push_back(Value(std::string("close")));
    stream_events_arr_val->elements.push_back(Value(std::string("error")));
    obj->properties["_events"] = {stream_events_arr_val, false, false, true, tok};

    auto pipe_impl = [state](const std::vector<Value>& args, EnvPtr env, const Token& tok) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "readable.pipe(writable) requires a writable stream as destination", tok.loc);
        }

        if (!std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "pipe destination must be a writable stream object", tok.loc);
        }

        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);

        // Extract destination stream ID
        auto id_it = dest_obj->properties.find("_id");
        if (id_it == dest_obj->properties.end() ||
            !std::holds_alternative<double>(id_it->second.value)) {
            throw SwaziError("TypeError", "Invalid stream object", tok.loc);
        }

        long long dest_id = static_cast<long long>(std::get<double>(id_it->second.value));

        // Try to find writable stream first
        WritableStreamStatePtr writable_state;
        {
            std::lock_guard<std::mutex> lock(g_writable_streams_mutex);
            auto it = g_writable_streams.find(dest_id);
            if (it != g_writable_streams.end()) {
                writable_state = it->second;
            }
        }

        // Parse options for end behavior
        bool end_on_finish = true;
        if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[1]);
            auto end_it = opts->properties.find("end");
            if (end_it != opts->properties.end() && std::holds_alternative<bool>(end_it->second.value)) {
                end_on_finish = std::get<bool>(end_it->second.value);
            }
        }

        // If not found in writable streams, assume it's a duplex and wire manually
        if (!writable_state) {
            // Wire up readable -> duplex manually using direct function calls
            Token evt_tok{};
            evt_tok.loc = TokenLocation("<pipe-to-duplex>", 0, 0, 0);

            // Data handler: calls duplex.write(chunk) when data arrives
            auto data_handler = [dest_obj, state](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                if (args.empty()) return std::monostate{};

                auto write_it = dest_obj->properties.find("write");
                if (write_it == dest_obj->properties.end()) return std::monostate{};

                if (!std::holds_alternative<FunctionPtr>(write_it->second.value)) return std::monostate{};

                FunctionPtr write_fn = std::get<FunctionPtr>(write_it->second.value);

                // Call the write function - it's a native function so call native_impl directly
                if (write_fn->is_native && write_fn->native_impl) {
                    try {
                        return write_fn->native_impl({args[0]}, env, token);
                    } catch (...) {
                        return Value{false};
                    }
                }

                // For non-native functions, would need evaluator->invoke_function
                // but duplex write() is always native, so this shouldn't happen
                return Value{false};
            };

            auto data_fn = std::make_shared<FunctionValue>("pipe.data", data_handler, nullptr, evt_tok);
            state->data_listeners.push_back(data_fn);

            // End handler: calls duplex.end() when readable ends (if end_on_finish is true)
            if (end_on_finish) {
                auto end_handler = [dest_obj](const std::vector<Value>&, EnvPtr env, const Token& token) -> Value {
                    auto end_it = dest_obj->properties.find("end");
                    if (end_it == dest_obj->properties.end()) return std::monostate{};

                    if (!std::holds_alternative<FunctionPtr>(end_it->second.value)) return std::monostate{};

                    FunctionPtr end_fn = std::get<FunctionPtr>(end_it->second.value);

                    // Call the end function - it's a native function
                    if (end_fn->is_native && end_fn->native_impl) {
                        try {
                            return end_fn->native_impl({}, env, token);
                        } catch (...) {
                            return std::monostate{};
                        }
                    }

                    return std::monostate{};
                };

                auto end_fn = std::make_shared<FunctionValue>("pipe.end", end_handler, nullptr, evt_tok);
                state->end_listeners.push_back(end_fn);
            }

            // Start flowing if not already
            if (!state->flowing && !state->ended && !state->destroyed) {
                state->flowing = true;

                uv_timer_t* timer = new uv_timer_t;
                ReadableStreamState* raw_state = state.get();
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

            // Return the duplex object for chaining
            return Value{dest_obj};
        }

        // Regular writable stream path - use implement_pipe
        return implement_pipe(state, writable_state, end_on_finish, tok);
    };
    obj->properties["pipe"] = {
        Value(std::make_shared<FunctionValue>("stream.pipe", pipe_impl, nullptr, tok)),
        false, false, true, tok};

    return obj;
}

// ============================================================================
// FACTORY
// ============================================================================

Value native_createReadStream(const std::vector<Value>& args, EnvPtr env, Evaluator* evaluator, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "streams.createReadable requires path argument", token.loc);
    }
    if (!std::holds_alternative<std::string>(args[0])) {
        throw SwaziError("TypeError", "path must be a string", token.loc);
    }

    std::string path = std::get<std::string>(args[0]);
    if (path.empty()) {
        throw SwaziError("TypeError", "path cannot be empty", token.loc);
    }

    StreamOptions opts;
    if (args.size() >= 2) {
        opts = parse_stream_options(args[1]);
    }

    uv_fs_t open_req;
    int fd = uv_fs_open(scheduler_get_loop(), &open_req, path.c_str(), O_RDONLY, 0, NULL);
    uv_fs_req_cleanup(&open_req);

    if (fd < 0) {
        throw SwaziError("IOError", "Failed to open file '" + path + "': " + uv_strerror(fd), token.loc);
    }

    uv_fs_t stat_req;
    int stat_result = uv_fs_fstat(scheduler_get_loop(), &stat_req, fd, NULL);

    if (stat_result < 0) {
        uv_fs_req_cleanup(&stat_req);
        uv_fs_t close_req;
        uv_fs_close(scheduler_get_loop(), &close_req, fd, NULL);
        uv_fs_req_cleanup(&close_req);
        throw SwaziError("IOError", "Failed to get file size: " + std::string(uv_strerror(stat_result)), token.loc);
    }

    size_t file_size = static_cast<size_t>(stat_req.statbuf.st_size);
    uv_fs_req_cleanup(&stat_req);

    if (opts.end == 0) {
        opts.end = file_size;
    }
    if (opts.end > file_size) {
        uv_fs_t close_req;
        uv_fs_close(scheduler_get_loop(), &close_req, fd, NULL);
        uv_fs_req_cleanup(&close_req);
        throw SwaziError("RangeError", "Stream end exceeds file size", token.loc);
    }
    if (opts.start > opts.end) {
        uv_fs_t close_req;
        uv_fs_close(scheduler_get_loop(), &close_req, fd, NULL);
        uv_fs_req_cleanup(&close_req);
        throw SwaziError("RangeError", "Stream start cannot exceed stream end", token.loc);
    }

    auto state = std::make_shared<ReadableStreamState>();
    state->id = g_next_stream_id.fetch_add(1);
    state->fd = fd;
    state->path = path;
    state->file_size = file_size;
    state->stream_start = opts.start;
    state->stream_end = opts.end;
    state->current_position = opts.start;
    state->high_water_mark = opts.high_water_mark;
    state->encoding = opts.encoding;
    state->auto_close = opts.auto_close;
    state->speed = opts.speed;
    state->env = env;
    state->evaluator = evaluator;

    {
        std::lock_guard<std::mutex> lock(g_readable_streams_mutex);
        g_readable_streams[state->id] = state;
    }

    return Value{create_readable_stream_object(state)};
}
