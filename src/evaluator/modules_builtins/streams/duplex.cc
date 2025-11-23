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
// DUPLEX STREAM STATE
// ============================================================================

struct DuplexStreamState : public std::enable_shared_from_this<DuplexStreamState> {
    long long id;

    // Internal buffer for readable side (when not backed by file)
    std::deque<std::vector<char>> read_buffer;
    size_t read_buffer_size = 0;

    // Internal buffer for writable side (when not backed by file)
    std::deque<std::vector<char>> write_buffer;
    size_t write_buffer_size = 0;

    size_t read_high_water_mark = 65536;
    size_t write_high_water_mark = 65536;
    std::string read_encoding = "binary";
    std::string write_encoding = "utf8";

    // State flags
    bool allow_half_open = true;
    bool readable_ended = false;
    bool writable_ended = false;
    bool readable_flowing = false;
    bool readable_paused = false;
    bool writable_finished = false;
    bool destroyed = false;
    bool reading = false;
    bool writing = false;

    EnvPtr env;
    Evaluator* evaluator = nullptr;

    // Event listeners
    std::vector<FunctionPtr> data_listeners;
    std::vector<FunctionPtr> end_listeners;
    std::vector<FunctionPtr> drain_listeners;
    std::vector<FunctionPtr> finish_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> close_listeners;

    // User-provided read/write implementations
    FunctionPtr read_impl;   // Called when readable needs data
    FunctionPtr write_impl;  // Called when data needs to be written

    std::vector<std::shared_ptr<DuplexStreamState>> self_references;

    void keep_alive() {
        self_references.push_back(shared_from_this());
    }

    void release_keepalive() {
        self_references.clear();
    }
};

using DuplexStreamStatePtr = std::shared_ptr<DuplexStreamState>;

static std::mutex g_duplex_streams_mutex;
// static std::atomic<long long> g_next_duplex_stream_id{1};
static std::unordered_map<long long, DuplexStreamStatePtr> g_duplex_streams;

// ============================================================================
// HELPER: Emit events synchronously
// ============================================================================

static void emit_duplex_event_sync(DuplexStreamStatePtr state,
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
    tok.loc = TokenLocation("<duplex-event>", 0, 0, 0);

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
// HELPER: Push data to readable buffer
// ============================================================================

static bool duplex_push(DuplexStreamStatePtr state, const std::vector<char>& data) {
    if (state->readable_ended || state->destroyed) {
        return false;
    }

    if (data.empty()) {
        // Push null = end readable
        state->readable_ended = true;
        emit_duplex_event_sync(state, state->end_listeners, {});

        // If not allowHalfOpen and writable is also ended, close
        if (!state->allow_half_open && state->writable_ended) {
            emit_duplex_event_sync(state, state->close_listeners, {});
        }

        return false;
    }

    state->read_buffer.push_back(data);
    state->read_buffer_size += data.size();

    // If flowing, emit data immediately
    if (state->readable_flowing && !state->readable_paused) {
        while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
            auto chunk_data = state->read_buffer.front();
            state->read_buffer.pop_front();
            state->read_buffer_size -= chunk_data.size();

            auto chunk = std::make_shared<BufferValue>();
            chunk->data.assign(chunk_data.begin(), chunk_data.end());
            chunk->encoding = state->read_encoding;

            Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
            emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});
        }
    }

    return state->read_buffer_size < state->read_high_water_mark;
}

// ============================================================================
// DUPLEX OPTIONS
// ============================================================================

struct DuplexOptions {
    size_t read_high_water_mark = 65536;
    size_t write_high_water_mark = 65536;
    std::string read_encoding = "binary";
    std::string write_encoding = "utf8";
    bool allow_half_open = true;
};

static DuplexOptions parse_duplex_options(const Value& opts_val) {
    DuplexOptions opts;
    if (!std::holds_alternative<ObjectPtr>(opts_val)) {
        return opts;
    }

    ObjectPtr opts_obj = std::get<ObjectPtr>(opts_val);

    auto rhwm_it = opts_obj->properties.find("readHighWaterMark");
    if (rhwm_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(rhwm_it->second.value)) {
            double val = std::get<double>(rhwm_it->second.value);
            if (val > 0 && val <= 50e6) {
                opts.read_high_water_mark = static_cast<size_t>(val);
            }
        }
    }

    auto whwm_it = opts_obj->properties.find("writeHighWaterMark");
    if (whwm_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(whwm_it->second.value)) {
            double val = std::get<double>(whwm_it->second.value);
            if (val > 0 && val <= 50e6) {
                opts.write_high_water_mark = static_cast<size_t>(val);
            }
        }
    }

    auto renc_it = opts_obj->properties.find("readEncoding");
    if (renc_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(renc_it->second.value)) {
            std::string enc = std::get<std::string>(renc_it->second.value);
            if (enc == "utf8" || enc == "utf-8" || enc == "binary") {
                opts.read_encoding = enc;
            }
        }
    }

    auto wenc_it = opts_obj->properties.find("writeEncoding");
    if (wenc_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(wenc_it->second.value)) {
            std::string enc = std::get<std::string>(wenc_it->second.value);
            if (enc == "utf8" || enc == "utf-8" || enc == "binary") {
                opts.write_encoding = enc;
            }
        }
    }

    auto aho_it = opts_obj->properties.find("allowHalfOpen");
    if (aho_it != opts_obj->properties.end()) {
        if (std::holds_alternative<bool>(aho_it->second.value)) {
            opts.allow_half_open = std::get<bool>(aho_it->second.value);
        }
    }

    return opts;
}

// ============================================================================
// CREATE DUPLEX STREAM OBJECT
// ============================================================================

static ObjectPtr create_duplex_stream_object(DuplexStreamStatePtr state) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<duplex>", 0, 0, 0);

    // ========================================================================
    // on(event, callback)
    // ========================================================================

    auto on_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "duplex.on requires (event, callback)", token.loc);
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

            if (is_first && !state->readable_ended && !state->destroyed) {
                state->readable_flowing = true;

                // Emit any buffered data
                while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
                    auto chunk_data = state->read_buffer.front();
                    state->read_buffer.pop_front();
                    state->read_buffer_size -= chunk_data.size();

                    auto chunk = std::make_shared<BufferValue>();
                    chunk->data.assign(chunk_data.begin(), chunk_data.end());
                    chunk->encoding = state->read_encoding;

                    Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
                    emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});
                }

                // Call user's read implementation if provided
                if (state->read_impl && !state->reading) {
                    state->reading = true;
                    try {
                        state->evaluator->invoke_function(state->read_impl, {}, state->env, token);
                    } catch (...) {
                        // Ignore errors in user read implementation
                    }
                    state->reading = false;
                }
            }
        } else if (event == "end") {
            state->end_listeners.push_back(cb);
        } else if (event == "drain") {
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
        Value{std::make_shared<FunctionValue>("duplex.on", on_impl, nullptr, tok)},
        false, false, true, tok};

    // ========================================================================
    // pause() - Pause readable side
    // ========================================================================

    auto pause_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        state->readable_paused = true;
        state->readable_flowing = false;
        return std::monostate{};
    };
    obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("duplex.pause", pause_impl, nullptr, tok)},
        false, false, true, tok};

    // ========================================================================
    // resume() - Resume readable side
    // ========================================================================

    auto resume_impl = [state](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
        bool was_paused = state->readable_paused;
        state->readable_paused = false;
        state->readable_flowing = true;

        if (was_paused) {
            // Emit buffered data
            while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
                auto chunk_data = state->read_buffer.front();
                state->read_buffer.pop_front();
                state->read_buffer_size -= chunk_data.size();

                auto chunk = std::make_shared<BufferValue>();
                chunk->data.assign(chunk_data.begin(), chunk_data.end());
                chunk->encoding = state->read_encoding;

                Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
                emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});
            }

            // Call user read if needed
            if (state->read_impl && !state->reading && state->read_buffer.empty()) {
                state->reading = true;
                try {
                    state->evaluator->invoke_function(state->read_impl, {}, state->env, token);
                } catch (...) {}
                state->reading = false;
            }
        }

        return std::monostate{};
    };
    obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("duplex.resume", resume_impl, nullptr, tok)},
        false, false, true, tok};

    // ========================================================================
    // write(data, [encoding], [callback])
    // ========================================================================

    auto write_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->destroyed) {
            throw SwaziError("Error", "Cannot write to destroyed stream", token.loc);
        }
        if (state->writable_ended) {
            throw SwaziError("Error", "Cannot write after end", token.loc);
        }
        if (args.empty()) {
            throw SwaziError("TypeError", "write() requires data argument", token.loc);
        }

        const Value& data = args[0];
        FunctionPtr callback = nullptr;
        std::string encoding = state->write_encoding;

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

        // Convert to bytes
        std::vector<char> bytes;
        if (std::holds_alternative<BufferPtr>(data)) {
            auto buf = std::get<BufferPtr>(data);
            bytes = std::vector<char>(buf->data.begin(), buf->data.end());
        } else if (std::holds_alternative<std::string>(data)) {
            std::string str = std::get<std::string>(data);
            bytes = std::vector<char>(str.begin(), str.end());
        }

        if (bytes.empty()) {
            if (callback) {
                emit_duplex_event_sync(state, {callback}, {});
            }
            return Value{true};
        }

        // Add to write buffer
        state->write_buffer.push_back(bytes);
        state->write_buffer_size += bytes.size();

        // Call user write implementation if provided
        if (state->write_impl && !state->writing) {
            state->writing = true;

            // Create buffer value for user
            auto chunk_buf = std::make_shared<BufferValue>();
            chunk_buf->data.assign(bytes.begin(), bytes.end());
            chunk_buf->encoding = encoding;

            try {
                state->evaluator->invoke_function(state->write_impl, {Value{chunk_buf}}, state->env, token);
            } catch (...) {
                // Handle write error
            }

            state->writing = false;

            // Pop from buffer since it's been written
            state->write_buffer.pop_front();
            state->write_buffer_size -= bytes.size();
        }

        if (callback) {
            emit_duplex_event_sync(state, {callback}, {});
        }

        bool needs_drain = (state->write_buffer_size >= state->write_high_water_mark);
        return Value{!needs_drain};
    };
    obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("duplex.write", write_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // push(data) - Push data to readable buffer (for internal use)
    // ========================================================================

    auto push_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            // push() with no args = push(null) = end readable
            return Value{duplex_push(state, {})};
        }

        const Value& data = args[0];

        // Handle null/undefined
        if (std::holds_alternative<std::monostate>(data)) {
            return Value{duplex_push(state, {})};
        }

        // Convert to bytes
        std::vector<char> bytes;
        if (std::holds_alternative<BufferPtr>(data)) {
            auto buf = std::get<BufferPtr>(data);
            bytes = std::vector<char>(buf->data.begin(), buf->data.end());
        } else if (std::holds_alternative<std::string>(data)) {
            std::string str = std::get<std::string>(data);
            bytes = std::vector<char>(str.begin(), str.end());
        }

        return Value{duplex_push(state, bytes)};
    };
    obj->properties["push"] = {
        Value{std::make_shared<FunctionValue>("duplex.push", push_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // end([finalChunk], [callback])
    // ========================================================================

    auto end_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->destroyed) {
            throw SwaziError("Error", "Cannot end destroyed stream", token.loc);
        }
        if (state->writable_ended) {
            return std::monostate{};
        }

        state->writable_ended = true;

        FunctionPtr callback = nullptr;

        if (!args.empty() && !std::holds_alternative<std::monostate>(args[0])) {
            if (std::holds_alternative<FunctionPtr>(args[0])) {
                callback = std::get<FunctionPtr>(args[0]);
            } else {
                // Write final chunk first
                std::vector<char> bytes;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    auto buf = std::get<BufferPtr>(args[0]);
                    bytes = std::vector<char>(buf->data.begin(), buf->data.end());
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    bytes = std::vector<char>(str.begin(), str.end());
                }

                if (!bytes.empty() && state->write_impl) {
                    auto chunk_buf = std::make_shared<BufferValue>();
                    chunk_buf->data.assign(bytes.begin(), bytes.end());
                    try {
                        state->evaluator->invoke_function(state->write_impl, {Value{chunk_buf}}, state->env, token);
                    } catch (...) {}
                }
            }
        }

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        // Mark as finished
        state->writable_finished = true;
        emit_duplex_event_sync(state, state->finish_listeners, {});

        if (callback) {
            emit_duplex_event_sync(state, {callback}, {});
        }

        // If not allowHalfOpen, end readable too
        if (!state->allow_half_open && !state->readable_ended) {
            duplex_push(state, {});  // Push null to end
        }

        // If both sides ended, close
        if (state->readable_ended && state->writable_ended) {
            emit_duplex_event_sync(state, state->close_listeners, {});
        }

        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("duplex.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // destroy([error])
    // ========================================================================

    auto destroy_impl = [state](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (state->destroyed) {
            return std::monostate{};
        }

        state->destroyed = true;
        state->readable_ended = true;
        state->writable_ended = true;

        state->read_buffer.clear();
        state->read_buffer_size = 0;
        state->write_buffer.clear();
        state->write_buffer_size = 0;

        if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
            emit_duplex_event_sync(state, state->error_listeners, {args[0]});
        }

        emit_duplex_event_sync(state, state->close_listeners, {});

        {
            std::lock_guard<std::mutex> lock(g_duplex_streams_mutex);
            g_duplex_streams.erase(state->id);
        }

        state->release_keepalive();
        return std::monostate{};
    };
    obj->properties["destroy"] = {
        Value{std::make_shared<FunctionValue>("duplex.destroy", destroy_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // pipe(destination, [options]) - Pipe duplex's readable side to destination
    // ========================================================================

    auto pipe_impl = [state](const std::vector<Value>& args, EnvPtr env, const Token& tok) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "duplex.pipe(writable) requires a writable stream as destination", tok.loc);
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

        // Try to find writable stream
        WritableStreamStatePtr writable_state;
        {
            std::lock_guard<std::mutex> lock(g_writable_streams_mutex);
            auto it = g_writable_streams.find(dest_id);
            if (it != g_writable_streams.end()) {
                writable_state = it->second;
            }
        }

        // Parse options
        bool end_on_finish = true;
        if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[1]);
            auto end_it = opts->properties.find("end");
            if (end_it != opts->properties.end() && std::holds_alternative<bool>(end_it->second.value)) {
                end_on_finish = std::get<bool>(end_it->second.value);
            }
        }

        // If not found in writable streams, assume it's another duplex and wire manually
        if (!writable_state) {
            Token evt_tok{};
            evt_tok.loc = TokenLocation("<duplex-pipe>", 0, 0, 0);

            // Data handler: when duplex emits data, write to destination
            auto data_handler = [dest_obj, state](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                if (args.empty()) return std::monostate{};

                auto write_it = dest_obj->properties.find("write");
                if (write_it == dest_obj->properties.end()) return std::monostate{};

                if (!std::holds_alternative<FunctionPtr>(write_it->second.value)) return std::monostate{};

                FunctionPtr write_fn = std::get<FunctionPtr>(write_it->second.value);

                // Call write function
                if (write_fn->is_native && write_fn->native_impl) {
                    try {
                        Value result = write_fn->native_impl({args[0]}, env, token);

                        // Handle backpressure - if write returns false, pause this duplex
                        if (std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                            state->readable_paused = true;
                            state->readable_flowing = false;
                        }

                        return result;
                    } catch (...) {
                        return Value{false};
                    }
                }

                return Value{false};
            };

            auto data_fn = std::make_shared<FunctionValue>("duplex-pipe.data", data_handler, nullptr, evt_tok);
            state->data_listeners.push_back(data_fn);

            // Drain handler: when destination drains, resume this duplex
            auto drain_handler = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (state->readable_paused && !state->readable_ended) {
                    state->readable_paused = false;
                    state->readable_flowing = true;

                    // Emit any buffered data
                    while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
                        auto chunk_data = state->read_buffer.front();
                        state->read_buffer.pop_front();
                        state->read_buffer_size -= chunk_data.size();

                        auto chunk = std::make_shared<BufferValue>();
                        chunk->data.assign(chunk_data.begin(), chunk_data.end());
                        chunk->encoding = state->read_encoding;

                        Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
                        emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});
                    }
                }
                return std::monostate{};
            };

            auto drain_fn = std::make_shared<FunctionValue>("duplex-pipe.drain", drain_handler, nullptr, evt_tok);

            // Attach drain listener to destination
            auto drain_it = dest_obj->properties.find("on");
            if (drain_it != dest_obj->properties.end() && std::holds_alternative<FunctionPtr>(drain_it->second.value)) {
                FunctionPtr on_fn = std::get<FunctionPtr>(drain_it->second.value);
                if (on_fn->is_native && on_fn->native_impl) {
                    try {
                        on_fn->native_impl({Value{std::string("drain")}, Value{drain_fn}}, env, evt_tok);
                    } catch (...) {}
                }
            }

            // End handler: end destination when this duplex ends
            if (end_on_finish) {
                auto end_handler = [dest_obj](const std::vector<Value>&, EnvPtr env, const Token& token) -> Value {
                    auto end_it = dest_obj->properties.find("end");
                    if (end_it == dest_obj->properties.end()) return std::monostate{};

                    if (!std::holds_alternative<FunctionPtr>(end_it->second.value)) return std::monostate{};

                    FunctionPtr end_fn = std::get<FunctionPtr>(end_it->second.value);

                    if (end_fn->is_native && end_fn->native_impl) {
                        try {
                            return end_fn->native_impl({}, env, token);
                        } catch (...) {
                            return std::monostate{};
                        }
                    }

                    return std::monostate{};
                };

                auto end_fn = std::make_shared<FunctionValue>("duplex-pipe.end", end_handler, nullptr, evt_tok);
                state->end_listeners.push_back(end_fn);
            }

            // Start flowing if not already
            if (!state->readable_flowing && !state->readable_ended && !state->destroyed) {
                state->readable_flowing = true;

                // Emit any buffered data
                while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
                    auto chunk_data = state->read_buffer.front();
                    state->read_buffer.pop_front();
                    state->read_buffer_size -= chunk_data.size();

                    auto chunk = std::make_shared<BufferValue>();
                    chunk->data.assign(chunk_data.begin(), chunk_data.end());
                    chunk->encoding = state->read_encoding;

                    Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
                    emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});
                }
            }

            // Return the destination object for chaining
            return Value{dest_obj};
        }

        // If destination is a writable stream, we can't use implement_pipe directly
        // since that expects ReadableStreamState, not DuplexStreamState
        // So we wire it up manually similar to above
        Token evt_tok{};
        evt_tok.loc = TokenLocation("<duplex-to-writable-pipe>", 0, 0, 0);

        auto data_handler = [writable_state, state](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            if (args.empty() || !writable_state || writable_state->destroyed || writable_state->ended) {
                return std::monostate{};
            }

            const Value& chunk = args[0];
            std::vector<char> bytes;

            if (std::holds_alternative<BufferPtr>(chunk)) {
                auto buf = std::get<BufferPtr>(chunk);
                bytes = std::vector<char>(buf->data.begin(), buf->data.end());
            } else if (std::holds_alternative<std::string>(chunk)) {
                std::string str = std::get<std::string>(chunk);
                bytes = std::vector<char>(str.begin(), str.end());
            }

            if (bytes.empty()) return std::monostate{};

            WriteChunk write_chunk;
            write_chunk.data = std::move(bytes);
            write_chunk.callback = nullptr;

            writable_state->buffered_size += write_chunk.data.size();
            writable_state->write_queue.push_back(std::move(write_chunk));

            bool needs_drain = (writable_state->buffered_size >= writable_state->high_water_mark);

            if (!writable_state->writing && !writable_state->corked) {
                schedule_next_write(writable_state);
            }

            if (needs_drain) {
                writable_state->draining = true;
                state->readable_paused = true;
                state->readable_flowing = false;
            }

            return Value{!needs_drain};
        };

        auto data_fn = std::make_shared<FunctionValue>("duplex-to-writable.data", data_handler, nullptr, evt_tok);
        state->data_listeners.push_back(data_fn);

        // Drain handler
        auto drain_handler = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (state->readable_paused && !state->readable_ended) {
                state->readable_paused = false;
                state->readable_flowing = true;
            }
            return std::monostate{};
        };

        auto drain_fn = std::make_shared<FunctionValue>("duplex-to-writable.drain", drain_handler, nullptr, evt_tok);
        writable_state->drain_listeners.push_back(drain_fn);

        // End handler
        if (end_on_finish) {
            auto end_handler = [writable_state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (writable_state && !writable_state->ended) {
                    writable_state->ended = true;

                    if (writable_state->write_queue.empty() && !writable_state->writing) {
                        writable_state->finished = true;
                        emit_writable_event_sync(writable_state, writable_state->finish_listeners, {});

                        if (writable_state->auto_destroy) {
                            writable_state->close_file();
                            emit_writable_event_sync(writable_state, writable_state->close_listeners, {});
                        }
                    } else if (!writable_state->writing && !writable_state->corked) {
                        schedule_next_write(writable_state);
                    }
                }
                return std::monostate{};
            };

            auto end_fn = std::make_shared<FunctionValue>("duplex-to-writable.end", end_handler, nullptr, evt_tok);
            state->end_listeners.push_back(end_fn);
        }

        // Start flowing
        if (!state->readable_flowing && !state->readable_ended && !state->destroyed) {
            state->readable_flowing = true;

            // Emit buffered data
            while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
                auto chunk_data = state->read_buffer.front();
                state->read_buffer.pop_front();
                state->read_buffer_size -= chunk_data.size();

                auto chunk = std::make_shared<BufferValue>();
                chunk->data.assign(chunk_data.begin(), chunk_data.end());
                chunk->encoding = state->read_encoding;

                Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
                emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});
            }
        }

        // Return writable stream object for chaining
        return Value{create_writable_stream_object(writable_state)};
    };

    obj->properties["pipe"] = {
        Value{std::make_shared<FunctionValue>("duplex.pipe", pipe_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // Properties
    // ========================================================================

    obj->properties["isPaused"] = {
        Value{std::make_shared<FunctionValue>("duplex.isPaused", [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{state->readable_paused}; }, nullptr, tok)}, false, true, true, tok};

    obj->properties["isEnded"] = {
        Value{std::make_shared<FunctionValue>("duplex.isEnded", [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{state->readable_ended && state->writable_ended}; }, nullptr, tok)}, false, true, true, tok};

    obj->properties["readHighWaterMark"] = {Value{static_cast<double>(state->read_high_water_mark)}, false, false, true, tok};
    obj->properties["writeHighWaterMark"] = {Value{static_cast<double>(state->write_high_water_mark)}, false, false, true, tok};
    obj->properties["readEncoding"] = {Value{state->read_encoding}, false, false, true, tok};
    obj->properties["writeEncoding"] = {Value{state->write_encoding}, false, false, true, tok};
    obj->properties["allowHalfOpen"] = {Value{state->allow_half_open}, false, false, true, tok};
    obj->properties["_id"] = {Value{static_cast<double>(state->id)}, false, false, true, tok};

    auto events_arr = std::make_shared<ArrayValue>();
    events_arr->elements.push_back(Value(std::string("data")));
    events_arr->elements.push_back(Value(std::string("end")));
    events_arr->elements.push_back(Value(std::string("drain")));
    events_arr->elements.push_back(Value(std::string("finish")));
    events_arr->elements.push_back(Value(std::string("error")));
    events_arr->elements.push_back(Value(std::string("close")));
    obj->properties["_events"] = {events_arr, false, false, true, tok};

    return obj;
}

// ============================================================================
// FACTORY FUNCTION
// ============================================================================

Value native_createDuplexStream(const std::vector<Value>& args, EnvPtr env, Evaluator* evaluator, const Token& token) {
    DuplexOptions opts;
    if (!args.empty()) {
        opts = parse_duplex_options(args[0]);
    }

    auto state = std::make_shared<DuplexStreamState>();
    state->id = g_next_stream_id.fetch_add(1);
    state->read_high_water_mark = opts.read_high_water_mark;
    state->write_high_water_mark = opts.write_high_water_mark;
    state->read_encoding = opts.read_encoding;
    state->write_encoding = opts.write_encoding;
    state->allow_half_open = opts.allow_half_open;
    state->env = env;
    state->evaluator = evaluator;

    // Extract optional read/write implementations
    if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
        ObjectPtr impl_obj = std::get<ObjectPtr>(args[1]);

        auto read_it = impl_obj->properties.find("read");
        if (read_it != impl_obj->properties.end() && std::holds_alternative<FunctionPtr>(read_it->second.value)) {
            state->read_impl = std::get<FunctionPtr>(read_it->second.value);
        }

        auto write_it = impl_obj->properties.find("write");
        if (write_it != impl_obj->properties.end() && std::holds_alternative<FunctionPtr>(write_it->second.value)) {
            state->write_impl = std::get<FunctionPtr>(write_it->second.value);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_duplex_streams_mutex);
        g_duplex_streams[state->id] = state;
    }

    return Value{create_duplex_stream_object(state)};
}