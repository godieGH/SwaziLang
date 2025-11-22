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

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// ============================================================================
// ACTIVE OPERATIONS TRACKING
// ============================================================================

static std::atomic<size_t> g_active_stream_operations{0};

bool streams_have_active_work() {
    return g_active_stream_operations.load() > 0;
}

// ============================================================================
// UTILITY HELPERS
// ============================================================================

static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) {
        return std::get<std::string>(v);
    }
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) {
        return std::get<bool>(v) ? "true" : "false";
    }
    return "";
}

static void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// ============================================================================
// ENCODING HELPERS
// ============================================================================

static Value encode_buffer_for_emission(const BufferPtr& buf, const std::string& encoding) {
    if (!buf) return std::monostate{};

    if (encoding == "utf8" || encoding == "utf-8") {
        return Value{std::string(buf->data.begin(), buf->data.end())};
    } else {
        // binary (default) - return raw buffer
        return Value{buf};
    }
}

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

struct ReadableStreamState : public std::enable_shared_from_this<ReadableStreamState> {
    long long id;
    uv_file fd = -1;
    std::string path;

    size_t current_position = 0;
    size_t stream_start = 0;
    size_t stream_end = 0;
    size_t file_size = 0;

    size_t high_water_mark = 65536;
    std::string encoding = "binary";
    bool auto_close = true;
    double speed = 1.0;

    bool paused = false;
    bool ended = false;
    bool reading = false;
    bool destroyed = false;
    bool flowing = false;
    
    EnvPtr env;
    Evaluator* evaluator = nullptr;  // ADD THIS

    std::vector<FunctionPtr> data_listeners;
    std::vector<FunctionPtr> end_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> close_listeners;

    std::vector<std::shared_ptr<ReadableStreamState>> self_references;

    void keep_alive() {
        self_references.push_back(shared_from_this());
    }

    void release_keepalive() {
        self_references.clear();
    }

    void close_file() {
        if (fd >= 0) {
            uv_fs_t close_req;
            uv_fs_close(scheduler_get_loop(), &close_req, fd, NULL);
            uv_fs_req_cleanup(&close_req);
            fd = -1;
        }
    }
};
struct ReadContext {
    ReadableStreamState* state;
    char* buffer;
    size_t buffer_size;
};

using ReadableStreamStatePtr = std::shared_ptr<ReadableStreamState>;

static std::mutex g_readable_streams_mutex;
static std::atomic<long long> g_next_readable_stream_id{1};
static std::unordered_map<long long, ReadableStreamStatePtr> g_readable_streams;

// ============================================================================
// EVENT EMISSION - SYNCHRONOUS VERSION
// ============================================================================

static void emit_readable_event_sync(ReadableStreamStatePtr state,
                                      const std::vector<FunctionPtr>& listeners,
                                      const std::vector<Value>& args) {
    if (!state || !state->env || !state->evaluator) {
        // Fallback to async if no env/evaluator
        for (const auto& cb : listeners) {
            if (cb) {
                schedule_listener_call(cb, args);
            }
        }
        return;
    }
    
    Token tok{};
    tok.loc = TokenLocation("<stream-event>", 0, 0, 0);
    
    // Call each listener SYNCHRONOUSLY using invoke_function
    for (const auto& cb : listeners) {
        if (cb) {
            try {
                // USE invoke_function to call the callback synchronously
                state->evaluator->invoke_function(cb, args, state->env, tok);
            } catch (const SwaziError& e) {
                // If callback throws, could emit error event
                // But do it async to avoid recursion
                schedule_listener_call(nullptr, {Value{std::string(e.what())}});
            } catch (...) {
                // Ignore other errors
            }
        }
    }
}

// ============================================================================
// ASYNC READ OPERATIONS
// ============================================================================

static void schedule_next_read(ReadableStreamStatePtr state);

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
            });
        }, static_cast<uint64_t>(state->speed), 0);
    } else {
        // No speed delay - read next chunk immediately
        schedule_next_read(state);
    }
}
static void schedule_next_read(ReadableStreamStatePtr state) {
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
// STREAM OBJECT
// ============================================================================

static ObjectPtr create_readable_stream_object(ReadableStreamStatePtr state) {
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
    obj->properties["on"] = {Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)}, false, false, false, tok};

    auto pause_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        state->paused = true;
        state->flowing = false;
        return std::monostate{};
    };
    obj->properties["pause"] = {Value{std::make_shared<FunctionValue>("stream.pause", pause_impl, nullptr, tok)}, false, false, false, tok};

    auto resume_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        bool was_paused = state->paused;
        state->paused = false;
        state->flowing = true;

        if (was_paused && !state->ended && !state->destroyed && !state->reading) {
            schedule_next_read(state);
        }
        return std::monostate{};
    };
    obj->properties["resume"] = {Value{std::make_shared<FunctionValue>("stream.resume", resume_impl, nullptr, tok)}, false, false, false, tok};

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
    obj->properties["destroy"] = {Value{std::make_shared<FunctionValue>("stream.destroy", destroy_impl, nullptr, tok)}, false, false, false, tok};

    auto is_paused_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->paused};
    };
    obj->properties["isPaused"] = {Value{std::make_shared<FunctionValue>("stream.isPaused", is_paused_impl, nullptr, tok)}, false, false, false, tok};

    auto is_ended_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->ended};
    };
    obj->properties["isEnded"] = {Value{std::make_shared<FunctionValue>("stream.isEnded", is_ended_impl, nullptr, tok)}, false, false, false, tok};

    return obj;
}

// ============================================================================
// FACTORY
// ============================================================================

static Value native_createReadStream(const std::vector<Value>& args, EnvPtr env, Evaluator* evaluator, const Token& token) {
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
    state->id = g_next_readable_stream_id.fetch_add(1);
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

// ============================================================================
// EXPORTS
// ============================================================================

std::shared_ptr<ObjectValue> make_streams_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    
    auto createReadable = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_createReadStream(args, env, evaluator, token);
    };

    obj->properties["readable"] = {
        Value{std::make_shared<FunctionValue>("streams.readable", createReadable, env, tok)},
        false, false, false, tok};

    return obj;
}

// ============================================================================
// NETWORK STREAM STUBS
// ============================================================================

ObjectPtr create_network_readable_stream_object(uv_tcp_t* socket) {
    auto obj = std::make_shared<ObjectValue>();
    return obj;
}

ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket) {
    auto obj = std::make_shared<ObjectValue>();
    return obj;
}