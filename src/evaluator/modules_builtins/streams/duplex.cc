#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
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

struct WriteRequest {
    std::vector<char> data;
    FunctionPtr callback;
};

struct DuplexStreamState : public std::enable_shared_from_this<DuplexStreamState> {
    long long id;

    // Internal buffer for readable side (when not backed by file)
    std::deque<std::vector<char>> read_buffer;
    size_t read_buffer_size = 0;

    // Internal buffer for writable side (when not backed by file)
    std::deque<std::unique_ptr<WriteRequest>> write_buffer;
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
    bool emitting = false;

    EnvPtr env;
    Evaluator* evaluator = nullptr;
    ObjectPtr recv;

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

        auto end_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            emit_duplex_event_sync(state, state->end_listeners, {});

            // If not allowHalfOpen and writable is also ended, close
            if (!state->allow_half_open && state->writable_ended) {
                emit_duplex_event_sync(state, state->close_listeners, {});
                {
                    std::lock_guard<std::mutex> lock(g_duplex_streams_mutex);
                    g_duplex_streams.erase(state->id);
                }
                state->release_keepalive();
            }
            return Value{};
        };

        schedule_listener_call(std::make_shared<FunctionValue>("end_worker", end_fn, nullptr, Token()), {});

        return false;
    }

    state->read_buffer.push_back(data);
    state->read_buffer_size += data.size();

    if (!state->emitting && state->readable_flowing && !state->readable_paused) {
        state->emitting = true;

        // If flowing, emit data immediately
        Token token{};
        token.loc = TokenLocation("<duplex-push-worker>", 0, 0, 0);
        auto push_fn = [state](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            if (state->readable_flowing && !state->readable_paused) {
                bool already_refill = false;
                while (!state->read_buffer.empty() && state->readable_flowing && !state->readable_paused) {
                    auto chunk_data = state->read_buffer.front();
                    state->read_buffer.pop_front();
                    state->read_buffer_size -= chunk_data.size();

                    auto chunk = std::make_shared<BufferValue>();
                    chunk->data.assign(chunk_data.begin(), chunk_data.end());
                    chunk->encoding = state->read_encoding;

                    Value encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
                    emit_duplex_event_sync(state, state->data_listeners, {encoded_chunk});

                    // refill the buffer
                    if ((state->read_buffer_size / state->read_high_water_mark) < 0.25 && !already_refill) {
                        if (state->read_impl && !state->reading) {
                            schedule_listener_call(std::make_shared<FunctionValue>("rifill_worker", [state](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                                state->reading = true;
                                try {
                                    state->evaluator->call_function_with_receiver_public(state->read_impl, state->recv, {}, state->env, token);
                                } catch (const SwaziError& e) {
                                  if (state->error_listeners.empty()) {
                                    std::cerr << "Unhandled error in stream listener: " << e.what() << std::endl;
                                  } else {
                                    Value error{std::string(e.what())};
                                    emit_duplex_event_sync(state, state->error_listeners, {error});
                                  }
                                } catch (...) {}
                              state->reading = false;
                              return Value{}; }, nullptr, token), {});
                        }
                        already_refill = true;
                    }
                }
                state->emitting = false;
            }
            return Value{};
        };
        schedule_listener_call(std::make_shared<FunctionValue>("push_worker", push_fn, nullptr, token), {});
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

static void process_write_queue(DuplexStreamStatePtr state) {
    // 1. If the buffer is empty, we are done for now
    if (state->write_buffer.empty() || state->destroyed) {
        state->writing = false;

        if (state->destroyed) return;

        // If the stream was told to end, and we just finished the last chunk...
        if (state->writable_ended) {
            state->writable_finished = true;
            emit_duplex_event_sync(state, state->finish_listeners, {});

            // Handle closing logic here once both sides are done
            if (state->readable_ended || !state->allow_half_open) {
                emit_duplex_event_sync(state, state->close_listeners, {});

                // Both sides are done. If we don't clean up now,
                // this object stays in memory forever.
                {
                    std::lock_guard<std::mutex> lock(g_duplex_streams_mutex);
                    g_duplex_streams.erase(state->id);
                }
                state->release_keepalive();
            }
        } else {
            // Otherwise, we just drained and can take more data
            emit_duplex_event_sync(state, state->drain_listeners, {});
        }
        return;
    }

    // Mark as writing
    state->writing = true;

    // Grab the current chunk
    auto req = std::move(state->write_buffer.front());
    state->write_buffer.pop_front();  // should add this line
    auto chunk_buf = std::make_shared<BufferValue>();
    chunk_buf->data.assign(req->data.begin(), req->data.end());
    chunk_buf->encoding = state->write_encoding;

    FunctionPtr callback = req->callback;
    size_t bytes_size = req->data.size();

    Token tok{};
    tok.loc = TokenLocation("<duplex-write-worker>", 0, 0, 0);

    // Create the task
    auto task_fn = [state, chunk_buf, callback, bytes_size](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
        if (state->destroyed) return std::monostate{};

        // 1. Run the implementation
        if (state->write_impl) {
            try {
                state->evaluator->call_function_with_receiver_public(state->write_impl, state->recv, {Value{chunk_buf}}, state->env, token);
            } catch (const SwaziError& e) {
                if (state->error_listeners.empty()) {
                    std::cerr << "Unhandled error in stream listener: " << e.what() << std::endl;
                } else {
                    Value error{std::string(e.what())};
                    emit_duplex_event_sync(state, state->error_listeners, {error});
                }
            } catch (...) {}
        }

        if (state->write_buffer_size >= bytes_size) {
            state->write_buffer_size -= bytes_size;
        } else {
            state->write_buffer_size = 0;
        }

        // 3. Fire callback
        if (callback) {
            emit_duplex_event_sync(state, {callback}, {});
        }

        // 4. TRIGGER NEXT TICK
        // Do NOT call process_write_queue(state) directly here.
        // Schedule it so the current task finishes and clears its stack first.
        state->writing = false;  // Reset so the next call can proceed
        schedule_listener_call(std::make_shared<FunctionValue>("next_write", [state](const std::vector<Value>&, EnvPtr, const Token&) {
            process_write_queue(state);
            return std::monostate{}; }, nullptr, token), {});

        return std::monostate{};
    };
    schedule_listener_call(std::make_shared<FunctionValue>("write_worker", task_fn, nullptr, tok), {});
}

// ============================================================================
// HELPER: Add a data listener and start flowing (unify on("data") and pipe())
// ============================================================================
static void add_duplex_data_listener(DuplexStreamStatePtr state, FunctionPtr listener, const Token& evt_tok) {
    bool is_first = state->data_listeners.empty();
    state->data_listeners.push_back(listener);

    if (is_first && !state->readable_ended && !state->destroyed) {
        // Start flowing and schedule the same worker that on_impl used.
        state->readable_flowing = true;

        auto on_data_fn = [state](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
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

            // Call user's _read implementation if provided (kick the producer)
            if (state->read_impl && !state->reading) {
                state->reading = true;
                try {
                    state->evaluator->call_function_with_receiver_public(state->read_impl, state->recv, {}, state->env, token);
                } catch (const SwaziError& e) {
                    if (state->error_listeners.empty()) {
                        std::cerr << "Unhandled error in stream listener: " << e.what() << std::endl;
                    } else {
                        Value error{std::string(e.what())};
                        emit_duplex_event_sync(state, state->error_listeners, {error});
                    }
                } catch (...) {}
                state->reading = false;
            }

            return Value{};
        };

        schedule_listener_call(std::make_shared<FunctionValue>("on_data_worker", on_data_fn, nullptr, evt_tok), {});
    }
}

static ObjectPtr create_duplex_stream_object(DuplexStreamStatePtr state) {
    auto obj = std::make_shared<ObjectValue>();
    state->recv = obj;
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
            add_duplex_data_listener(state, cb, token);
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
            auto resume_fn = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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
                        state->evaluator->call_function_with_receiver_public(state->read_impl, state->recv, {}, state->env, token);
                    } catch (const SwaziError& e) {
                        if (state->error_listeners.empty()) {
                            std::cerr << "Unhandled error in stream listener: " << e.what() << std::endl;
                        } else {
                            Value error{std::string(e.what())};
                            emit_duplex_event_sync(state, state->error_listeners, {error});
                        }
                    } catch (...) {}
                    state->reading = false;
                }

                return Value{};
            };
            schedule_listener_call(std::make_shared<FunctionValue>("worker_resume", resume_fn, nullptr, token), {});
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

        auto req = std::make_unique<WriteRequest>(WriteRequest{std::move(bytes), callback});
        size_t size = req->data.size();

        // Add to write buffer
        state->write_buffer.push_back(std::move(req));
        state->write_buffer_size += size;

        if (!state->writing) {
            state->writing = true;
            process_write_queue(state);
        }

        bool is_under_limit = state->write_buffer_size < state->write_high_water_mark;
        return Value{is_under_limit};
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
    // read() - Pull data from the readable buffer
    // ========================================================================
    auto read_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->readable_ended || state->destroyed) return Value{};

        if (state->read_impl && !state->reading && state->read_buffer.empty()) {
            state->reading = true;
            try {
                state->evaluator->call_function_with_receiver_public(state->read_impl, state->recv, {}, state->env, token);
            } catch (const SwaziError& e) {
                if (state->error_listeners.empty()) {
                    std::cerr << "Unhandled error in stream listener: " << e.what() << std::endl;
                } else {
                    Value error{std::string(e.what())};
                    emit_duplex_event_sync(state, state->error_listeners, {error});
                }
            } catch (...) {}
            state->reading = false;
        }

        auto chunk = std::make_shared<BufferValue>();
        Value encoded_chunk;

        if (!state->read_buffer.empty() && !state->readable_flowing) {
            auto chunk_data = state->read_buffer.front();
            state->read_buffer.pop_front();
            state->read_buffer_size -= chunk_data.size();

            chunk->data.assign(chunk_data.begin(), chunk_data.end());
            chunk->encoding = state->read_encoding;

            encoded_chunk = encode_buffer_for_emission(chunk, state->read_encoding);
        }
        return Value{encoded_chunk};
    };
    obj->properties["read"] = {
        Value{std::make_shared<FunctionValue>("duplex.read", read_impl, nullptr, tok)},
        false, false, false, tok};

    // ========================================================================
    // end([finalChunk], [callback])
    // ========================================================================

    auto end_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->destroyed || state->writable_ended) return std::monostate{};

        state->writable_ended = true;

        FunctionPtr callback = nullptr;
        Value chunk = std::monostate{};

        // 1. Identify what the arguments are
        if (args.size() >= 1) {
            if (std::holds_alternative<FunctionPtr>(args[0])) {
                callback = std::get<FunctionPtr>(args[0]);
            } else {
                chunk = args[0];
            }
        }
        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        // 2. Handle the final chunk (if any)
        bool has_chunk = !std::holds_alternative<std::monostate>(chunk);
        if (has_chunk) {
            // Convert to bytes logic (same as write_impl)
            std::vector<char> bytes;
            if (std::holds_alternative<BufferPtr>(chunk)) {
                auto buf = std::get<BufferPtr>(chunk);
                bytes = std::vector<char>(buf->data.begin(), buf->data.end());
            } else if (std::holds_alternative<std::string>(chunk)) {
                std::string str = std::get<std::string>(chunk);
                bytes = std::vector<char>(str.begin(), str.end());
            }

            auto req = std::make_unique<WriteRequest>(WriteRequest{std::move(bytes), callback});
            size_t size = req->data.size();

            state->write_buffer.push_back(std::move(req));
            state->write_buffer_size += size;
        } else if (callback) {
            // No chunk, but we have a callback.
            // We should trigger it when the stream actually finishes.
            state->finish_listeners.push_back(callback);
        }

        // 3. Start processing if not already
        if (!state->writing) {
            state->writing = true;
            process_write_queue(state);
        }

        // 4. Handle readable side if needed
        if (!state->allow_half_open && !state->readable_ended) {
            duplex_push(state, {});
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

        // Helper: attach "on" (works for native and userland)
        auto attach_on = [&](ObjectPtr target, FunctionPtr on_fn, EnvPtr env, const Token& evt_tok, const std::string& eventName, Value listener) {
            if (!on_fn) return;
            try {
                if (on_fn->is_native && on_fn->native_impl) {
                    on_fn->native_impl({Value{eventName}, listener}, env, evt_tok);
                } else if (state && state->evaluator) {
                    // call with target as receiver
                    state->evaluator->call_function_with_receiver_public(on_fn, target, {Value{eventName}, listener}, env, evt_tok);
                }
            } catch (...) {
                // ignore attach errors
            }
        };

        // If not found in writable streams, assume it's another duplex and wire manually
        if (!writable_state) {
            Token evt_tok{};
            evt_tok.loc = TokenLocation("<duplex-pipe>", 0, 0, 0);

            // Data handler: call dest.write(...) — use public API for both native and userland
            auto data_handler = [dest_obj, state](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                if (args.empty()) return std::monostate{};

                auto write_it = dest_obj->properties.find("write");
                if (write_it == dest_obj->properties.end()) return std::monostate{};
                if (!std::holds_alternative<FunctionPtr>(write_it->second.value)) return std::monostate{};

                FunctionPtr write_fn = std::get<FunctionPtr>(write_it->second.value);
                Value result = std::monostate{};

                try {
                    if (write_fn->is_native && write_fn->native_impl) {
                        result = write_fn->native_impl({args[0]}, env, token);
                    } else if (state && state->evaluator) {
                        // call with dest_obj as receiver so userland write sees the right this
                        result = state->evaluator->call_function_with_receiver_public(write_fn, dest_obj, {args[0]}, env, token);
                    }
                } catch (const SwaziError& e) {
                    // forward to error listeners on source stream
                    if (!state->error_listeners.empty()) {
                        Value err{std::string(e.what())};
                        emit_duplex_event_sync(state, state->error_listeners, {err});
                    } else {
                        std::cerr << "Unhandled error in pipe write: " << e.what() << std::endl;
                    }
                    return Value{false};
                } catch (...) {
                    return Value{false};
                }

                // Treat non-bool (monostate / Value{}) as "true" (no backpressure)
                if (std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                    state->readable_paused = true;
                    state->readable_flowing = false;
                    return result;
                }
                return Value{true};
            };
            auto data_fn = std::make_shared<FunctionValue>("duplex-pipe.data", data_handler, nullptr, evt_tok);
            add_duplex_data_listener(state, data_fn, evt_tok);

            // Drain handler: when destination drains, resume this duplex
            auto drain_handler = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (state->readable_paused && !state->readable_ended) {
                    state->readable_paused = false;
                    state->readable_flowing = true;
                }
                return std::monostate{};
            };
            auto drain_fn = std::make_shared<FunctionValue>("duplex-pipe.drain", drain_handler, nullptr, evt_tok);

            // Attach drain listener via dest.on("drain", drain_fn) (native or userland)
            auto drain_it = dest_obj->properties.find("on");
            if (drain_it != dest_obj->properties.end() && std::holds_alternative<FunctionPtr>(drain_it->second.value)) {
                FunctionPtr on_fn = std::get<FunctionPtr>(drain_it->second.value);
                attach_on(dest_obj, on_fn, env, evt_tok, "drain", Value{drain_fn});
            }

            // End handler: when source finishes, call dest.end()
            if (end_on_finish) {
                auto end_handler = [dest_obj](const std::vector<Value>&, EnvPtr env, const Token& token) -> Value {
                    auto end_it = dest_obj->properties.find("end");
                    if (end_it == dest_obj->properties.end()) return std::monostate{};
                    if (!std::holds_alternative<FunctionPtr>(end_it->second.value)) return std::monostate{};
                    FunctionPtr end_fn = std::get<FunctionPtr>(end_it->second.value);

                    try {
                        if (end_fn->is_native && end_fn->native_impl) {
                            return end_fn->native_impl({}, env, token);
                        } else if (dest_obj && dest_obj->properties.count("end")) {
                            // If userland, call with dest_obj as receiver through its evaluator if available.
                            // We don't have evaluator here, but end() is typically native — if not, best-effort:
                            // fallthrough to monostate (no-op) to avoid throwing inside event emission.
                            return std::monostate{};
                        }
                    } catch (...) {}
                    return std::monostate{};
                };

                auto end_fn = std::make_shared<FunctionValue>("duplex-pipe.end", end_handler, nullptr, evt_tok);
                state->end_listeners.push_back(end_fn);
            }

            // Return the destination object for chaining
            return Value{dest_obj};
        }

        // If destination is a writable stream (we have writable_state),
        // wire into its public API where possible. Keep existing writable_state path
        // but also attach a drain listener to resume the source.
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
        add_duplex_data_listener(state, data_fn, evt_tok);

        // Drain handler (resume source when writable drains)
        auto drain_handler = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (state->readable_paused && !state->readable_ended) {
                state->readable_paused = false;
                state->readable_flowing = true;
            }
            return std::monostate{};
        };
        auto drain_fn = std::make_shared<FunctionValue>("duplex-to-writable.drain", drain_handler, nullptr, evt_tok);
        writable_state->drain_listeners.push_back(drain_fn);

        // End handler for writable_state (when source ends)
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

    obj->properties["isDestroyed"] = {
        Value{std::make_shared<FunctionValue>("duplex.isDestroyed", [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{state->destroyed}; }, nullptr, tok)}, false, true, true, tok};
    obj->properties["isEnded"] = {
        Value{std::make_shared<FunctionValue>("duplex.isEnded", [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{state->readable_ended}; }, nullptr, tok)}, false, true, true, tok};
    obj->properties["isFinished"] = {
        Value{std::make_shared<FunctionValue>("duplex.isFinished", [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{state->writable_finished}; }, nullptr, tok)}, false, true, true, tok};

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

        auto read_it = impl_obj->properties.find("_read");
        if (read_it != impl_obj->properties.end() && std::holds_alternative<FunctionPtr>(read_it->second.value)) {
            state->read_impl = std::get<FunctionPtr>(read_it->second.value);
        }

        auto write_it = impl_obj->properties.find("_write");
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
