// streams.cc - Comprehensive streams module for Swazi language
// Provides readable/writable/duplex/transform stream abstractions

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// Forward declarations
static std::string value_to_string_simple_local(const Value& v);

// Helper: Convert buffer to hex string
static std::string buffer_to_hex(const BufferPtr& buf) {
    if (!buf) return "";
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t byte : buf->data) {
        ss << std::setw(2) << static_cast<int>(byte);
    }
    return ss.str();
}

// Helper: Convert hex string to buffer
static BufferPtr hex_to_buffer(const std::string& hex) {
    auto buf = std::make_shared<BufferValue>();
    buf->encoding = "hex";

    for (size_t i = 0; i + 1 < hex.length(); i += 2) {
        std::string byte_str = hex.substr(i, 2);
        uint8_t byte = static_cast<uint8_t>(std::stoul(byte_str, nullptr, 16));
        buf->data.push_back(byte);
    }
    return buf;
}

// Helper: Convert buffer to base64
static std::string buffer_to_base64(const BufferPtr& buf) {
    if (!buf) return "";

    static const char* base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";

    std::string encoded;
    const auto& data = buf->data;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t octet_a = i < data.size() ? data[i] : 0;
        uint32_t octet_b = i + 1 < data.size() ? data[i + 1] : 0;
        uint32_t octet_c = i + 2 < data.size() ? data[i + 2] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        encoded += base64_chars[(triple >> 18) & 0x3F];
        encoded += base64_chars[(triple >> 12) & 0x3F];
        encoded += (i + 1 < data.size()) ? base64_chars[(triple >> 6) & 0x3F] : '=';
        encoded += (i + 2 < data.size()) ? base64_chars[triple & 0x3F] : '=';
    }

    return encoded;
}

// Helper: Convert base64 string to buffer
static BufferPtr base64_to_buffer(const std::string& b64) {
    auto buf = std::make_shared<BufferValue>();
    buf->encoding = "base64";

    // Build decode table
    static int decode_table[256];
    static bool table_initialized = false;
    if (!table_initialized) {
        for (int i = 0; i < 256; i++) decode_table[i] = -1;
        const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) {
            decode_table[static_cast<unsigned char>(chars[i])] = i;
        }
        decode_table['='] = 0;
        table_initialized = true;
    }

    uint32_t buffer = 0;
    int bits_collected = 0;

    for (char c : b64) {
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;

        int value = decode_table[static_cast<unsigned char>(c)];
        if (value == -1) continue;
        if (c == '=') break;

        buffer = (buffer << 6) | value;
        bits_collected += 6;

        if (bits_collected >= 8) {
            bits_collected -= 8;
            buf->data.push_back(static_cast<uint8_t>((buffer >> bits_collected) & 0xFF));
        }
    }

    return buf;
}

// Helper: Apply encoding conversion to buffer for emission
static Value encode_buffer_for_emission(const BufferPtr& buf, const std::string& encoding) {
    if (!buf) return std::monostate{};

    if (encoding == "utf8") {
        // Convert to UTF-8 string
        return Value{std::string(buf->data.begin(), buf->data.end())};
    } else if (encoding == "base64") {
        // Convert to base64 string
        return Value{buffer_to_base64(buf)};
    } else if (encoding == "hex") {
        // Convert to hex string
        return Value{buffer_to_hex(buf)};
    } else {
        // binary (default) - return raw buffer
        return Value{buf};
    }
}

// Helper: Convert input value to buffer based on encoding
static BufferPtr decode_value_to_buffer(const Value& val, const std::string& encoding) {
    if (std::holds_alternative<BufferPtr>(val)) {
        // Already a buffer
        return std::get<BufferPtr>(val);
    }

    if (!std::holds_alternative<std::string>(val)) {
        return nullptr;
    }

    std::string str = std::get<std::string>(val);

    if (encoding == "base64") {
        return base64_to_buffer(str);
    } else if (encoding == "hex") {
        return hex_to_buffer(str);
    } else {
        // utf8 or binary - just copy bytes
        auto buf = std::make_shared<BufferValue>();
        buf->data.assign(str.begin(), str.end());
        buf->encoding = encoding;
        return buf;
    }
}

// Stream types
enum class StreamType {
    READABLE,
    WRITABLE,
    DUPLEX,
    TRANSFORM
};

// Stream state machine
enum class StreamState {
    OPEN,
    PAUSED,
    FLOWING,
    CLOSED,
    DESTROYED,
    ERRORED
};

// Helper to parse options object
struct StreamOptions {
    size_t high_water_mark = 65536;  // 64KB default (matching SwaziLang)
    std::string encoding = "binary";
    bool auto_close = true;
    std::string flags = "w";
    size_t start = 0;        // Stream start position (for readable)
    size_t end = 0;          // Stream end position (0 = EOF)
    double speed = 1.0;      // Stream speed multiplier (for readable)
};

static StreamOptions parse_stream_options(const Value& opts_val) {
    StreamOptions opts;
    if (!std::holds_alternative<ObjectPtr>(opts_val)) {
        return opts;
    }

    ObjectPtr opts_obj = std::get<ObjectPtr>(opts_val);

    // Parse highWaterMark
    auto hwm_it = opts_obj->properties.find("highWaterMark");
    if (hwm_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(hwm_it->second.value)) {
            double val = std::get<double>(hwm_it->second.value);
            if (val > 0 && val <= 50e6) {  // Max 50MB
                opts.high_water_mark = static_cast<size_t>(val);
            }
        }
    }

    // Parse encoding
    auto enc_it = opts_obj->properties.find("encoding");
    if (enc_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(enc_it->second.value)) {
            opts.encoding = std::get<std::string>(enc_it->second.value);
        }
    }

    // Parse autoClose
    auto ac_it = opts_obj->properties.find("autoClose");
    if (ac_it != opts_obj->properties.end()) {
        if (std::holds_alternative<bool>(ac_it->second.value)) {
            opts.auto_close = std::get<bool>(ac_it->second.value);
        }
    }

    // Parse flags
    auto flags_it = opts_obj->properties.find("flags");
    if (flags_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(flags_it->second.value)) {
            opts.flags = std::get<std::string>(flags_it->second.value);
        }
    }

    // Parse start
    auto start_it = opts_obj->properties.find("start");
    if (start_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(start_it->second.value)) {
            double val = std::get<double>(start_it->second.value);
            if (val >= 0) {
                opts.start = static_cast<size_t>(val);
            }
        }
    }

    // Parse end
    auto end_it = opts_obj->properties.find("end");
    if (end_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(end_it->second.value)) {
            double val = std::get<double>(end_it->second.value);
            if (val >= 0) {
                opts.end = static_cast<size_t>(val);
            }
        }
    }

    // Parse speed
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

// Base stream entry that holds all stream data and event listeners
struct StreamEntry : public std::enable_shared_from_this<StreamEntry> {
    long long id;
    StreamType type;
    std::atomic<StreamState> state{StreamState::OPEN};
    std::atomic<bool> is_destroyed{false};
    std::atomic<bool> has_error{false};

    // Event listeners (protected by mutex)
    std::mutex listeners_mutex;
    std::vector<FunctionPtr> data_listeners;
    std::vector<FunctionPtr> end_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> close_listeners;
    std::vector<FunctionPtr> drain_listeners;
    std::vector<FunctionPtr> finish_listeners;
    std::vector<FunctionPtr> pipe_listeners;
    std::vector<FunctionPtr> unpipe_listeners;

    // Buffering
    std::mutex buffer_mutex;
    std::deque<BufferPtr> buffer_queue;
    size_t high_water_mark = 16384;  // 16KB default
    std::atomic<size_t> buffered_size{0};

    // File/source info
    std::string source_path;
    FilePtr file_handle;

    // NEW: Network socket support
    uv_tcp_t* tcp_handle = nullptr;
    bool is_network_stream = false;
    std::atomic<size_t> pending_writes{0};

    // NEW: Keep-alive mechanism for async operations
    std::vector<std::shared_ptr<StreamEntry>> self_references;

    void keep_alive() {
        self_references.push_back(shared_from_this());
    }

    void release_keepalive() {
        self_references.clear();
    }

    // Piping
    std::mutex pipe_mutex;
    std::vector<std::weak_ptr<StreamEntry>> piped_to;
    bool ended = false;
    bool destroyed = false;

    // Transform function
    FunctionPtr transform_fn;
    Evaluator* evaluator_ptr = nullptr;

    // Options
    bool auto_close = true;
    std::string encoding = "binary";
    std::atomic<bool> paused{false};
    size_t stream_start = 0;
    size_t stream_end = 0;
    double stream_speed = 1.0;
    std::atomic<bool> should_stop_reading{false};
};
using StreamEntryPtr = std::shared_ptr<StreamEntry>;


// Forward declarations
static std::string value_to_string_simple_local(const Value& v);
static ObjectPtr create_readable_stream_object(StreamEntryPtr entry);
static ObjectPtr create_writable_stream_object(StreamEntryPtr entry);
static ObjectPtr create_duplex_stream_object(StreamEntryPtr entry);
static ObjectPtr create_transform_stream_object(StreamEntryPtr entry);
static BufferPtr read_data(StreamEntryPtr entry, size_t n);
static Value read_data_encoded(StreamEntryPtr entry, size_t n);
static bool write_data(StreamEntryPtr entry, BufferPtr data);


// Global stream registry
static std::mutex g_streams_mutex;
static std::atomic<long long> g_next_stream_id{1};
static std::unordered_map<long long, StreamEntryPtr> g_streams;

// Helper to schedule listener call on main thread
static void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// Helper to emit event to all registered listeners
static void emit_event(StreamEntryPtr entry,
    const std::vector<FunctionPtr>& listeners,
    const std::vector<Value>& args) {
    for (const auto& cb : listeners) {
        if (cb) schedule_listener_call(cb, args);
    }
}


// Helper: Add common introspection methods to stream object
static void add_stream_introspection(ObjectPtr obj, StreamEntryPtr entry, const Token& tok) {
    // stream.isPaused()
    auto is_paused_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{entry->paused.load()};
    };
    obj->properties["isPaused"] = {
        Value{std::make_shared<FunctionValue>("stream.isPaused", is_paused_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.isEnded()
    auto is_ended_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{entry->ended};
    };
    obj->properties["isEnded"] = {
        Value{std::make_shared<FunctionValue>("stream.isEnded", is_ended_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.getBufferedSize()
    auto get_buffered_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{static_cast<double>(entry->buffered_size.load())};
    };
    obj->properties["getBufferedSize"] = {
        Value{std::make_shared<FunctionValue>("stream.getBufferedSize", get_buffered_impl, nullptr, tok)},
        false, false, false, tok};
}

// Create READABLE stream object (limited methods)
static ObjectPtr create_readable_stream_object(StreamEntryPtr entry) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    // stream.on(event, callback)
    auto on_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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

        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
        if (event == "data")
            entry->data_listeners.push_back(cb);
        else if (event == "end")
            entry->end_listeners.push_back(cb);
        else if (event == "error")
            entry->error_listeners.push_back(cb);
        else if (event == "close")
            entry->close_listeners.push_back(cb);

        return std::monostate{};
    };
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.pause()
    auto pause_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->paused = true;
        return std::monostate{};
    };
    obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("stream.pause", pause_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.resume()
    auto resume_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->paused = false;
        // Signal the reading loop to continue if it exists
        return std::monostate{};
    };
    obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("stream.resume", resume_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.read()
    auto read_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return read_data_encoded(entry, entry->high_water_mark);
    };
    obj->properties["read"] = {
        Value{std::make_shared<FunctionValue>("stream.read", read_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.pipe(dest)
    auto pipe_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "pipe requires destination stream", token.loc);
        }

        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);

        // Data forwarding
        auto data_forwarder = std::make_shared<FunctionValue>(
            "pipe_data_forwarder",
            [entry, dest_obj](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                if (!args.empty()) {
                    auto write_prop = dest_obj->properties.find("write");
                    if (write_prop != dest_obj->properties.end() &&
                        std::holds_alternative<FunctionPtr>(write_prop->second.value)) {
                        FunctionPtr write_fn = std::get<FunctionPtr>(write_prop->second.value);
                        Value result = write_fn->native_impl({args[0]}, env, token);
                        
                        // Handle backpressure
                        if (std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                            entry->paused = true;
                        }
                    }
                }
                return std::monostate{};
            },
            nullptr, Token{});

        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            entry->data_listeners.push_back(data_forwarder);
        }

        // End forwarding
        auto end_forwarder = std::make_shared<FunctionValue>(
            "pipe_end_forwarder",
            [dest_obj](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                auto end_prop = dest_obj->properties.find("end");
                if (end_prop != dest_obj->properties.end() &&
                    std::holds_alternative<FunctionPtr>(end_prop->second.value)) {
                    FunctionPtr end_fn = std::get<FunctionPtr>(end_prop->second.value);
                    end_fn->native_impl({}, nullptr, Token{});
                }
                return std::monostate{};
            },
            nullptr, Token{});

        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            entry->end_listeners.push_back(end_forwarder);
        }

        // Listen for drain to resume reading
        auto drain_listener = std::make_shared<FunctionValue>(
            "pipe_drain_listener",
            [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                entry->paused = false;
                return std::monostate{};
            },
            nullptr, Token{});

        auto drain_on_prop = dest_obj->properties.find("on");
        if (drain_on_prop != dest_obj->properties.end() &&
            std::holds_alternative<FunctionPtr>(drain_on_prop->second.value)) {
            FunctionPtr on_fn = std::get<FunctionPtr>(drain_on_prop->second.value);
            on_fn->native_impl({Value{std::string("drain")}, Value{drain_listener}}, nullptr, Token{});
        }

        return args[0];
    };
    obj->properties["pipe"] = {
        Value{std::make_shared<FunctionValue>("stream.pipe", pipe_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.end()
    auto end_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->should_stop_reading = true;
        entry->ended = true;
        
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->end_listeners;
        }
        emit_event(entry, listeners, {});
        
        if (entry->auto_close && entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }
        
        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("stream.end", end_impl, nullptr, tok)},
        false, false, false, tok};
    
    
    obj->properties["_type"] = {
        Value{std::string("readable")},  // or "writable", "duplex", "transform"
        true, false, true, tok};  // hidden, non-writable, non-configurable
        
      // Add introspection methods
    add_stream_introspection(obj, entry, tok);

    
    return obj;
}

// Create WRITABLE stream object (limited methods)
static ObjectPtr create_writable_stream_object(StreamEntryPtr entry) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    // stream.on(event, callback)
    auto on_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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

        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
        if (event == "drain")
            entry->drain_listeners.push_back(cb);
        else if (event == "error")
            entry->error_listeners.push_back(cb);
        else if (event == "finish")
            entry->finish_listeners.push_back(cb);
        else if (event == "close")
            entry->close_listeners.push_back(cb);

        return std::monostate{};
    };
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.write(chunk)
    auto write_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stream.write requires data argument", token.loc);
        }

        BufferPtr buf = decode_value_to_buffer(args[0], entry->encoding);

        if (!buf) {
            throw SwaziError("TypeError",
                "write expects Buffer or string (encoding: " + entry->encoding + ")",
                token.loc);
        }

        bool ok = write_data(entry, buf);
        return Value{ok};
    };
    obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("stream.write", write_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.end(chunk?)
    auto end_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (!args.empty()) {
            BufferPtr buf;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                buf = std::get<BufferPtr>(args[0]);
            } else if (std::holds_alternative<std::string>(args[0])) {
                buf = std::make_shared<BufferValue>();
                std::string s = std::get<std::string>(args[0]);
                buf->data.assign(s.begin(), s.end());
            }
            if (buf) write_data(entry, buf);
        }

        entry->ended = true;

        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->finish_listeners;
        }
        emit_event(entry, listeners, {});

        if (entry->auto_close && entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }

        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("stream.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.close()
    auto close_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }
        return std::monostate{};
    };
    obj->properties["close"] = {
        Value{std::make_shared<FunctionValue>("stream.close", close_impl, nullptr, tok)},
        false, false, false, tok};
        
    obj->properties["_type"] = {
        Value{std::string("writable")},  // or "writable", "duplex", "transform"
        true, false, true, tok};  // hidden, non-writable, non-configurable
    
    
      // Add introspection methods
    add_stream_introspection(obj, entry, tok);
  
    
    return obj;
}



// Push data into readable stream (internal API)
static bool push_data(StreamEntryPtr entry, BufferPtr data) {
    if (!entry || entry->is_destroyed) return false;

    std::lock_guard<std::mutex> lk(entry->buffer_mutex);

    if (data) {
        // If in flowing mode, emit data immediately with encoding applied
        if (entry->state == StreamState::FLOWING &&  !entry->paused) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> llk(entry->listeners_mutex);
                listeners = entry->data_listeners;
            }

            Value encoded_data = encode_buffer_for_emission(data, entry->encoding);
            
            // Call listeners synchronously (like SwaziLang)
            for (const auto& cb : listeners) {
                if (cb) {
                    try {
                        cb->native_impl({encoded_data}, nullptr, Token{});
                    } catch (const std::exception& e) {
                        std::cerr << "Error in data listener: " << e.what() << std::endl;
                    }
                }
            }
        } else {
            // Only buffer if we're paused or in some other non-flowing state
            entry->buffer_queue.push_back(data);
            entry->buffered_size += data->data.size();
        }

        return entry->buffered_size < entry->high_water_mark;
    } else {
        // null data = end of stream
        entry->ended = true;
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> llk(entry->listeners_mutex);
            listeners = entry->end_listeners;
        }
        emit_event(entry, listeners, {});
    }

    return true;
}

// Read data from stream buffer
static BufferPtr read_data(StreamEntryPtr entry, size_t n = 0) {
    if (!entry) return nullptr;

    std::lock_guard<std::mutex> lk(entry->buffer_mutex);

    if (entry->buffer_queue.empty()) return nullptr;

    if (n == 0) {
        // Read all available data
        auto result = std::make_shared<BufferValue>();
        while (!entry->buffer_queue.empty()) {
            auto chunk = entry->buffer_queue.front();
            entry->buffer_queue.pop_front();
            result->data.insert(result->data.end(),
                chunk->data.begin(),
                chunk->data.end());
            entry->buffered_size -= chunk->data.size();
        }
        return result;
    } else {
        // Read n bytes
        auto result = std::make_shared<BufferValue>();
        size_t read = 0;

        while (read < n && !entry->buffer_queue.empty()) {
            auto chunk = entry->buffer_queue.front();
            size_t needed = n - read;

            if (chunk->data.size() <= needed) {
                // Use entire chunk
                result->data.insert(result->data.end(),
                    chunk->data.begin(),
                    chunk->data.end());
                read += chunk->data.size();
                entry->buffered_size -= chunk->data.size();
                entry->buffer_queue.pop_front();
            } else {
                // Split chunk
                result->data.insert(result->data.end(),
                    chunk->data.begin(),
                    chunk->data.begin() + needed);
                chunk->data.erase(chunk->data.begin(),
                    chunk->data.begin() + needed);
                read += needed;
                entry->buffered_size -= needed;
            }
        }

        return result;
    }
}

static Value read_data_encoded(StreamEntryPtr entry, size_t n = 0) {
    BufferPtr buf = read_data(entry, n);
    if (!buf) return std::monostate{};

    // ✅ APPLY ENCODING HERE
    return encode_buffer_for_emission(buf, entry->encoding);
}

// Write data to writable stream
static bool write_data(StreamEntryPtr entry, BufferPtr data) {
    if (!entry || !data) return false;
    if (entry->is_destroyed) return false;

    // NEW: Transform stream execution path
    if (entry->type == StreamType::TRANSFORM) {
        // If no transform function, use identity (passthrough)
        if (!entry->transform_fn || !entry->evaluator_ptr) {
            push_data(entry, data);
            return entry->buffered_size < entry->high_water_mark;
        }
        try {
            Token dummy_token;
            dummy_token.loc = TokenLocation("<transform>", 0, 0, 0);

            // Use evaluator to call the function properly
            Value result = entry->evaluator_ptr->invoke_function(
                entry->transform_fn,
                {Value{data}},
                entry->transform_fn->closure,
                dummy_token);

            // Push transformed result to readable side
            if (std::holds_alternative<BufferPtr>(result)) {
                BufferPtr transformed = std::get<BufferPtr>(result);
                push_data(entry, transformed);
            } else if (std::holds_alternative<std::string>(result)) {
                auto buf = std::make_shared<BufferValue>();
                std::string s = std::get<std::string>(result);
                buf->data.assign(s.begin(), s.end());
                push_data(entry, buf);
            } else if (std::holds_alternative<std::monostate>(result)) {
                return true;
            }

            return entry->buffered_size < entry->high_water_mark;

        } catch (const std::exception& e) {
            std::cerr << "Transform error: " << e.what() << std::endl;
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                listeners = entry->error_listeners;
            }
            emit_event(entry, listeners,
                {Value{std::string("Transform error: ") + e.what()}});
            return false;
        }
    }

    // NEW: Network socket write path
    if (entry->is_network_stream && entry->tcp_handle) {
        // Allocate write request
        uv_write_t* req = new uv_write_t;

        // Allocate buffer copy for libuv
        char* buf_copy = (char*)malloc(data->data.size());
        memcpy(buf_copy, data->data.data(), data->data.size());

        uv_buf_t uvbuf = uv_buf_init(buf_copy,
            static_cast<unsigned int>(data->data.size()));

        // Track this write for backpressure
        entry->pending_writes++;
        size_t write_size = data->data.size();

        // Store context for callback (entry + buffer pointer)
        struct WriteContext {
            std::shared_ptr<StreamEntry> entry;
            void* buffer;
            size_t size;
        };

        auto* ctx = new WriteContext{entry, buf_copy, write_size};
        req->data = ctx;

        // Keep entry alive during async write
        entry->keep_alive();

        int r = uv_write(req, (uv_stream_t*)entry->tcp_handle, &uvbuf, 1,
            [](uv_write_t* req, int status) {
                auto* ctx = static_cast<WriteContext*>(req->data);
                auto entry = ctx->entry;

                entry->pending_writes--;
                entry->buffered_size -= ctx->size;

                if (status < 0) {
                    // Write failed - emit error
                    std::vector<FunctionPtr> listeners;
                    {
                        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                        listeners = entry->error_listeners;
                    }
                    emit_event(entry, listeners,
                        {Value{std::string("Write error: ") + uv_strerror(status)}});
                }

                // Check if we should emit drain
                if (entry->buffered_size < entry->high_water_mark) {
                    std::vector<FunctionPtr> listeners;
                    {
                        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                        listeners = entry->drain_listeners;
                    }
                    emit_event(entry, listeners, {});
                }

                entry->release_keepalive();
                free(ctx->buffer);
                delete ctx;
                delete req;
            });

        if (r != 0) {
            // Write failed immediately
            entry->pending_writes--;
            entry->release_keepalive();
            free(buf_copy);
            delete ctx;
            delete req;

            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                listeners = entry->error_listeners;
            }
            emit_event(entry, listeners,
                {Value{std::string("Write error: ") + uv_strerror(r)}});
            return false;
        }

        entry->buffered_size += write_size;
        return entry->buffered_size < entry->high_water_mark;
    }

    // EXISTING: File-backed writable stream code (unchanged)
    if (entry->file_handle && entry->file_handle->is_open) {
        // Write synchronously to file (could be made async with uv_fs_write)
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile((HANDLE)entry->file_handle->handle,
                data->data.data(),
                static_cast<DWORD>(data->data.size()),
                &written,
                nullptr)) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                listeners = entry->error_listeners;
            }
            emit_event(entry, listeners, {Value{std::string("Write error")}});
            return false;
        }
#else
        ssize_t written = ::write(entry->file_handle->fd,
            data->data.data(),
            data->data.size());
        if (written < 0) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                listeners = entry->error_listeners;
            }
            emit_event(entry, listeners,
                {Value{std::string("Write error: ") + std::strerror(errno)}});
            return false;
        }
#endif
    }

    // Check if we should emit drain
    bool should_drain = false;
    {
        std::lock_guard<std::mutex> lk(entry->buffer_mutex);
        entry->buffered_size += data->data.size();
        if (entry->buffered_size < entry->high_water_mark) {
            should_drain = true;
        }
    }

    if (should_drain) {
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->drain_listeners;
        }
        emit_event(entry, listeners, {});
    }

    return entry->buffered_size < entry->high_water_mark;
}


// Helper: Check if object is a writable stream
static bool is_writable_stream(const ObjectPtr& obj) {
    if (!obj) return false;
    
    auto type_it = obj->properties.find("_type");
    if (type_it == obj->properties.end()) return false;
    
    if (!std::holds_alternative<std::string>(type_it->second.value)) return false;
    
    std::string type = std::get<std::string>(type_it->second.value);
    return type == "writable" || type == "duplex" || type == "transform";
}


// Create DUPLEX stream object (both readable and writable methods)
static ObjectPtr create_duplex_stream_object(StreamEntryPtr entry) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    // ===== READABLE SIDE =====

    // stream.on(event, callback) - supports both readable and writable events
    auto on_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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

        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
        if (event == "data")
            entry->data_listeners.push_back(cb);
        else if (event == "end")
            entry->end_listeners.push_back(cb);
        else if (event == "error")
            entry->error_listeners.push_back(cb);
        else if (event == "close")
            entry->close_listeners.push_back(cb);
        else if (event == "drain")
            entry->drain_listeners.push_back(cb);
        else if (event == "finish")
            entry->finish_listeners.push_back(cb);

        return std::monostate{};
    };
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.pause()
    auto pause_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->paused = true;
        return std::monostate{};
    };
    obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("stream.pause", pause_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.resume()
    auto resume_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->paused = false;
        return std::monostate{};
    };
    obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("stream.resume", resume_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.read()
    auto read_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return read_data_encoded(entry, entry->high_water_mark);
    };
    obj->properties["read"] = {
        Value{std::make_shared<FunctionValue>("stream.read", read_impl, nullptr, tok)},
        false, false, false, tok};

    // ===== WRITABLE SIDE =====

    // stream.write(chunk)
    auto write_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stream.write requires data argument", token.loc);
        }

        BufferPtr buf = decode_value_to_buffer(args[0], entry->encoding);

        if (!buf) {
            throw SwaziError("TypeError",
                "write expects Buffer or string (encoding: " + entry->encoding + ")",
                token.loc);
        }

        bool ok = write_data(entry, buf);
        return Value{ok};
    };
    obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("stream.write", write_impl, nullptr, tok)},
        false, false, false, tok};

    // ===== COMMON METHODS =====

    // stream.pipe(dest)
    auto pipe_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "pipe requires destination stream", token.loc);
        }

        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);

        // Data forwarding
        auto data_forwarder = std::make_shared<FunctionValue>(
            "pipe_data_forwarder",
            [entry, dest_obj](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                if (!args.empty()) {
                    auto write_prop = dest_obj->properties.find("write");
                    if (write_prop != dest_obj->properties.end() &&
                        std::holds_alternative<FunctionPtr>(write_prop->second.value)) {
                        FunctionPtr write_fn = std::get<FunctionPtr>(write_prop->second.value);
                        Value result = write_fn->native_impl({args[0]}, env, token);
                        
                        if (std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                            entry->paused = true;
                        }
                    }
                }
                return std::monostate{};
            },
            nullptr, Token{});

        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            entry->data_listeners.push_back(data_forwarder);
        }

        // End forwarding
        auto end_forwarder = std::make_shared<FunctionValue>(
            "pipe_end_forwarder",
            [dest_obj](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                auto end_prop = dest_obj->properties.find("end");
                if (end_prop != dest_obj->properties.end() &&
                    std::holds_alternative<FunctionPtr>(end_prop->second.value)) {
                    FunctionPtr end_fn = std::get<FunctionPtr>(end_prop->second.value);
                    end_fn->native_impl({}, nullptr, Token{});
                }
                return std::monostate{};
            },
            nullptr, Token{});

        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            entry->end_listeners.push_back(end_forwarder);
        }

        // Drain listener
        auto drain_listener = std::make_shared<FunctionValue>(
            "pipe_drain_listener",
            [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                entry->paused = false;
                return std::monostate{};
            },
            nullptr, Token{});

        auto drain_on_prop = dest_obj->properties.find("on");
        if (drain_on_prop != dest_obj->properties.end() &&
            std::holds_alternative<FunctionPtr>(drain_on_prop->second.value)) {
            FunctionPtr on_fn = std::get<FunctionPtr>(drain_on_prop->second.value);
            on_fn->native_impl({Value{std::string("drain")}, Value{drain_listener}}, nullptr, Token{});
        }

        return args[0];
    };
    obj->properties["pipe"] = {
        Value{std::make_shared<FunctionValue>("stream.pipe", pipe_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.end(chunk?)
    auto end_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (!args.empty()) {
            BufferPtr buf = decode_value_to_buffer(args[0], entry->encoding);
            if (buf) write_data(entry, buf);
        }

        entry->ended = true;

        std::vector<FunctionPtr> finish_listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            finish_listeners = entry->finish_listeners;
        }
        emit_event(entry, finish_listeners, {});

        // Also emit end on readable side
        std::vector<FunctionPtr> end_listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            end_listeners = entry->end_listeners;
        }
        emit_event(entry, end_listeners, {});

        if (entry->auto_close && entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }

        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("stream.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.destroy()
    auto destroy_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->is_destroyed = true;
        entry->destroyed = true;

        if (entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }

        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->close_listeners;
        }
        emit_event(entry, listeners, {});

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams.erase(entry->id);
        }

        return std::monostate{};
    };
    obj->properties["destroy"] = {
        Value{std::make_shared<FunctionValue>("stream.destroy", destroy_impl, nullptr, tok)},
        false, false, false, tok};
        
    obj->properties["_type"] = {
        Value{std::string("duplex")},  // or "writable", "duplex", "transform"
        true, false, true, tok};  // hidden, non-writable, non-configurable
    
      // Add introspection methods
    add_stream_introspection(obj, entry, tok);

    return obj;
}

// Create TRANSFORM stream object (duplex with transform function)
static ObjectPtr create_transform_stream_object(StreamEntryPtr entry) {
    // Start with duplex base
    auto obj = create_duplex_stream_object(entry);

    // Override write() to apply transformation
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    auto transform_write_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stream.write requires data argument", token.loc);
        }

        BufferPtr buf = decode_value_to_buffer(args[0], entry->encoding);

        if (!buf) {
            throw SwaziError("TypeError",
                "write expects Buffer or string (encoding: " + entry->encoding + ")",
                token.loc);
        }

        // If no transform function, pass through (identity transform)
        if (!entry->transform_fn || !entry->evaluator_ptr) {
            push_data(entry, buf);
            return Value{entry->buffered_size < entry->high_water_mark};
        }

        // Apply transform function
        try {
            Token dummy_token;
            dummy_token.loc = TokenLocation("<transform>", 0, 0, 0);

            Value result = entry->evaluator_ptr->invoke_function(
                entry->transform_fn,
                {Value{buf}},
                entry->transform_fn->closure,
                dummy_token);

            // Handle different return types
            if (std::holds_alternative<BufferPtr>(result)) {
                BufferPtr transformed = std::get<BufferPtr>(result);
                push_data(entry, transformed);
            } else if (std::holds_alternative<std::string>(result)) {
                auto transformed_buf = std::make_shared<BufferValue>();
                std::string s = std::get<std::string>(result);
                transformed_buf->data.assign(s.begin(), s.end());
                push_data(entry, transformed_buf);
            } else if (std::holds_alternative<std::monostate>(result)) {
                // Transform returned nothing (filtering)
                return Value{true};
            } else {
                throw SwaziError("TypeError",
                    "Transform function must return Buffer, string, or null",
                    token.loc);
            }

            return Value{entry->buffered_size < entry->high_water_mark};

        } catch (const std::exception& e) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                listeners = entry->error_listeners;
            }
            emit_event(entry, listeners,
                {Value{std::string("Transform error: ") + e.what()}});
            return Value{false};
        }
    };

    obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("stream.write", transform_write_impl, nullptr, tok)},
        false, false, false, tok};
        
    obj->properties["_type"] = {
        Value{std::string("transform")},  // or "writable", "duplex", "transform"
        true, false, true, tok};  // hidden, non-writable, non-configurable
      // Add introspection methods
    add_stream_introspection(obj, entry, tok);

    return obj;
}


// Factory: streams.readable(path, options?)
static Value native_createReadStream(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "streams.readable requires path argument", token.loc);
    }

    std::string path = value_to_string_simple_local(args[0]);

    StreamOptions opts;
    if (args.size() >= 2) {
        opts = parse_stream_options(args[1]);
    }

    auto entry = std::make_shared<StreamEntry>();
    entry->id = g_next_stream_id.fetch_add(1);
    entry->type = StreamType::READABLE;
    entry->state = StreamState::FLOWING;
    entry->high_water_mark = opts.high_water_mark;
    entry->auto_close = opts.auto_close;
    entry->encoding = opts.encoding;
    entry->stream_start = opts.start;
    entry->stream_end = opts.end;
    entry->stream_speed = opts.speed;
    entry->paused = false;

    // Open file
    auto file = std::make_shared<FileValue>();
    file->path = path;
    file->mode = "rb";
    file->is_binary = true;

#ifdef _WIN32
    file->handle = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) {
        throw SwaziError("IOError", "Failed to open file: " + path, token.loc);
    }
    
    // Get file size
    LARGE_INTEGER file_size;
    if (!GetFileSizeEx((HANDLE)file->handle, &file_size)) {
        CloseHandle((HANDLE)file->handle);
        throw SwaziError("IOError", "Failed to get file size: " + path, token.loc);
    }
    size_t total_size = static_cast<size_t>(file_size.QuadPart);
#else
    file->fd = ::open(path.c_str(), O_RDONLY);
    if (file->fd < 0) {
        throw SwaziError("IOError",
            "Failed to open file: " + path + " (" + std::strerror(errno) + ")",
            token.loc);
    }
    
    // Get file size
    struct stat st;
    if (fstat(file->fd, &st) != 0) {
        ::close(file->fd);
        throw SwaziError("IOError", "Failed to get file size: " + path, token.loc);
    }
    size_t total_size = static_cast<size_t>(st.st_size);
#endif

    file->is_open = true;
    entry->file_handle = file;

    // Validate and set stream boundaries
    if (entry->stream_end == 0) {
        entry->stream_end = total_size;
    } else if (entry->stream_end > total_size) {
        file->close_internal();
        throw SwaziError("RangeError", "Stream end exceeds file size", token.loc);
    }

    if (entry->stream_start > entry->stream_end) {
        file->close_internal();
        throw SwaziError("RangeError", "Stream start cannot exceed stream end", token.loc);
    }

    // Seek to start position
    if (entry->stream_start > 0) {
#ifdef _WIN32
        LARGE_INTEGER pos;
        pos.QuadPart = entry->stream_start;
        if (!SetFilePointerEx((HANDLE)file->handle, pos, nullptr, FILE_BEGIN)) {
            file->close_internal();
            throw SwaziError("IOError", "Failed to seek to start position", token.loc);
        }
#else
        if (lseek(file->fd, entry->stream_start, SEEK_SET) < 0) {
            file->close_internal();
            throw SwaziError("IOError", "Failed to seek to start position", token.loc);
        }
#endif
    }

    // Register stream
    {
        std::lock_guard<std::mutex> lk(g_streams_mutex);
        g_streams[entry->id] = entry;
    }

    // Schedule async reading loop (SwaziLang-style)
    scheduler_run_on_loop([entry]() {
        size_t current_pos = entry->stream_start;
        const size_t HWM = entry->high_water_mark;
        std::vector<uint8_t> buffer(HWM);

        while (!entry->should_stop_reading && !entry->ended) {
            // Pause check (like SwaziLang's handleDataCallback)
            while (entry->paused && !entry->should_stop_reading) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (entry->should_stop_reading) break;

            // Check if we've reached the end
            if (current_pos >= entry->stream_end) {
                entry->ended = true;
                push_data(entry, nullptr);  // Signal EOF
                break;
            }

            // Calculate chunk size
            size_t to_read = std::min(HWM, entry->stream_end - current_pos);

            ssize_t bytes_read = 0;
#ifdef _WIN32
            DWORD read_count = 0;
            if (!ReadFile((HANDLE)entry->file_handle->handle,
                    buffer.data(),
                    static_cast<DWORD>(to_read),
                    &read_count,
                    nullptr)) {
                // Error
                std::vector<FunctionPtr> listeners;
                {
                    std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                    listeners = entry->error_listeners;
                }
                emit_event(entry, listeners, {Value{std::string("Read error")}});
                break;
            }
            bytes_read = read_count;
#else
            bytes_read = ::read(entry->file_handle->fd, buffer.data(), to_read);
            if (bytes_read < 0) {
                // Error
                std::vector<FunctionPtr> listeners;
                {
                    std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                    listeners = entry->error_listeners;
                }
                emit_event(entry, listeners,
                    {Value{std::string("Read error: ") + std::strerror(errno)}});
                break;
            }
#endif

            if (bytes_read == 0) {
                // Unexpected EOF
                entry->ended = true;
                push_data(entry, nullptr);
                break;
            }

            current_pos += bytes_read;

            // Create chunk and push
            auto chunk = std::make_shared<BufferValue>();
            chunk->data.assign(buffer.begin(), buffer.begin() + bytes_read);
            push_data(entry, chunk);

            // Apply speed throttling (like SwaziLang)
            if (entry->stream_speed > 0) {
                int delay_ms = static_cast<int>(entry->stream_speed);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }

        // Auto-close if enabled
        if (entry->auto_close && entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }
    });

    return Value{create_readable_stream_object(entry)};
}

// Factory: streams.writable(path, options?)
static Value native_createWriteStream(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "streams.writable requires path argument", token.loc);
    }

    std::string path = value_to_string_simple_local(args[0]);

    // Parse options if provided
    StreamOptions opts;
    if (args.size() >= 2) {
        opts = parse_stream_options(args[1]);
    }

    auto entry = std::make_shared<StreamEntry>();
    entry->id = g_next_stream_id.fetch_add(1);
    entry->type = StreamType::WRITABLE;
    entry->source_path = path;

    // Apply options
    entry->high_water_mark = opts.high_water_mark;
    entry->auto_close = opts.auto_close;
    entry->encoding = opts.encoding;

    // Open file for writing
    auto file = std::make_shared<FileValue>();
    file->path = path;
    file->is_binary = true;

#ifdef _WIN32
    // Determine creation mode based on flags
    DWORD creation_mode = CREATE_ALWAYS;  // Default: truncate/overwrite

    if (opts.flags == "a" || opts.flags == "a+") {
        creation_mode = OPEN_ALWAYS;  // Append: open existing or create new
        file->mode = "ab";
    } else {
        file->mode = "wb";
    }

    file->handle = CreateFileA(path.c_str(), GENERIC_WRITE, 0,
        nullptr, creation_mode, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file->handle == INVALID_HANDLE_VALUE) {
        throw SwaziError("IOError", "Failed to open file: " + path, token.loc);
    }

    // If append mode, seek to end of file
    if (opts.flags == "a" || opts.flags == "a+") {
        SetFilePointer((HANDLE)file->handle, 0, nullptr, FILE_END);
    }
#else
    // Determine open flags based on options
    int open_flags = O_WRONLY | O_CREAT;

    if (opts.flags == "a" || opts.flags == "a+") {
        open_flags |= O_APPEND;  // Append mode
        file->mode = "ab";
    } else {
        open_flags |= O_TRUNC;  // Truncate mode (default)
        file->mode = "wb";
    }

    file->fd = ::open(path.c_str(), open_flags, 0644);

    if (file->fd < 0) {
        throw SwaziError("IOError",
            "Failed to open file: " + path + " (" + std::strerror(errno) + ")",
            token.loc);
    }
#endif

    file->is_open = true;
    entry->file_handle = file;

    // Register stream
    {
        std::lock_guard<std::mutex> lk(g_streams_mutex);
        g_streams[entry->id] = entry;
    }

    return Value{create_writable_stream_object(entry)};
}

// Helper: Normalize and validate stream mode
static std::string normalize_stream_mode(const std::string& mode, const Token& token) {
    if (mode == "r" || mode == "read" || mode == "readable") {
        return "readable";
    } else if (mode == "w" || mode == "write" || mode == "writable") {
        return "writable";
    } else if (mode == "d" || mode == "duplex") {
        return "duplex";
    } else if (mode == "t" || mode == "transform") {
        return "transform";
    } else {
        throw SwaziError("ValueError",
            "Invalid stream mode '" + mode + "'. Use 'r'/'read'/'readable', "
            "'w'/'write'/'writable', 'd'/'duplex', or 't'/'transform'",
            token.loc);
    }
}
// Factory: streams.create(source, mode, options?) OR streams.create(source, mode, transform_fn, options?)
static Value native_createStream(const std::vector<Value>& args, EnvPtr env, const Token& token, Evaluator* evaluator) {
    if (args.size() < 2) {
        throw SwaziError("TypeError",
            "streams.create requires at least (source, mode) arguments",
            token.loc);
    }

    std::string raw_mode = value_to_string_simple_local(args[1]);
    std::string mode = normalize_stream_mode(raw_mode, token);

    // ============ READABLE MODE ============
    if (mode == "readable") {
        std::vector<Value> read_args;
        read_args.push_back(args[0]);
        if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
            read_args.push_back(args[2]);
        }
        return native_createReadStream(read_args, env, token);
    }

    // ============ WRITABLE MODE ============
    else if (mode == "writable") {
        std::vector<Value> write_args;
        write_args.push_back(args[0]);
        if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
            write_args.push_back(args[2]);
        }
        return native_createWriteStream(write_args, env, token);
    }

    // ============ DUPLEX MODE ============
    else if (mode == "duplex") {
        StreamOptions opts;
        if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
            opts = parse_stream_options(args[2]);
        }

        auto entry = std::make_shared<StreamEntry>();
        entry->id = g_next_stream_id.fetch_add(1);
        entry->type = StreamType::DUPLEX;
        entry->high_water_mark = opts.high_water_mark;
        entry->auto_close = opts.auto_close;
        entry->encoding = opts.encoding;
        entry->paused = false;

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams[entry->id] = entry;
        }

        return Value{create_duplex_stream_object(entry)};
    }

    // ============ TRANSFORM MODE ============
    else if (mode == "transform") {
        FunctionPtr transform_fn = nullptr;
        StreamOptions opts;
        
        if (args.size() >= 3) {
            if (std::holds_alternative<FunctionPtr>(args[2])) {
                transform_fn = std::get<FunctionPtr>(args[2]);
                if (args.size() >= 4 && std::holds_alternative<ObjectPtr>(args[3])) {
                    opts = parse_stream_options(args[3]);
                }
            } else if (std::holds_alternative<ObjectPtr>(args[2])) {
                opts = parse_stream_options(args[2]);
            }
        }

        auto entry = std::make_shared<StreamEntry>();
        entry->id = g_next_stream_id.fetch_add(1);
        entry->type = StreamType::TRANSFORM;
        entry->evaluator_ptr = evaluator;
        entry->transform_fn = transform_fn;
        entry->high_water_mark = opts.high_water_mark;
        entry->auto_close = opts.auto_close;
        entry->encoding = opts.encoding;
        entry->paused = false;

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams[entry->id] = entry;
        }

        return Value{create_transform_stream_object(entry)};
    }

    // Should never reach here due to normalize_stream_mode validation
    throw SwaziError("ValueError", "Invalid stream mode", token.loc);
}

// Helper implementation
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

// Export factory
std::shared_ptr<ObjectValue> make_streams_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    // streams.readable(path, options?)
    obj->properties["readable"] = {
        Value{std::make_shared<FunctionValue>("streams.read", native_createReadStream, env, tok)},
        false, false, false, tok};

    // streams.writable(path, options?)
    obj->properties["writable"] = {
        Value{std::make_shared<FunctionValue>("streams.write", native_createWriteStream, env, tok)},
        false, false, false, tok};

    // streams.create(source, mode, transform?)
    obj->properties["create"] = {
        Value{std::make_shared<FunctionValue>("streams.create", [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value { return native_createStream(args, env, token, evaluator); }, env, tok)}, false, false, false, tok};

    return obj;
}

// NEW: Create a readable stream from a TCP socket (internal helper)
static StreamEntryPtr create_readable_network_stream(uv_tcp_t* socket) {
    auto entry = std::make_shared<StreamEntry>();
    entry->id = g_next_stream_id.fetch_add(1);
    entry->type = StreamType::READABLE;
    entry->state = StreamState::FLOWING;
    entry->tcp_handle = socket;
    entry->is_network_stream = true;

    // Register stream
    {
        std::lock_guard<std::mutex> lk(g_streams_mutex);
        g_streams[entry->id] = entry;
    }

    // Store back-pointer for libuv callbacks
    socket->data = entry.get();

    // Keep stream alive during read operations
    entry->keep_alive();

    // Start reading from socket
    uv_read_start((uv_stream_t*)socket,
        // Allocation callback
        [](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
            buf->base = (char*)malloc(suggested);
            buf->len = static_cast<unsigned int>(suggested);
        },
        // Read callback
        [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
            StreamEntry* raw_entry = static_cast<StreamEntry*>(stream->data);
            if (!raw_entry) {
                if (buf->base) free(buf->base);
                return;
            }

            // Get shared_ptr from raw pointer
            StreamEntryPtr entry;
            {
                std::lock_guard<std::mutex> lk(g_streams_mutex);
                auto it = g_streams.find(raw_entry->id);
                if (it != g_streams.end()) {
                    entry = it->second;
                }
            }

            if (!entry) {
                if (buf->base) free(buf->base);
                return;
            }

            if (nread > 0) {
                // Create buffer and push to stream
                auto chunk = std::make_shared<BufferValue>();
                chunk->data.assign(buf->base, buf->base + nread);

                // Push data (handles flowing mode and backpressure)
                push_data(entry, chunk);
            } else if (nread < 0) {
                if (nread == UV_EOF) {
                    // End of stream
                    push_data(entry, nullptr);
                } else {
                    // Error
                    std::vector<FunctionPtr> listeners;
                    {
                        std::lock_guard<std::mutex> lk(entry->listeners_mutex);
                        listeners = entry->error_listeners;
                    }
                    emit_event(entry, listeners,
                        {Value{std::string("Read error: ") + uv_strerror(nread)}});
                }

                // Stop reading and clean up
                uv_read_stop(stream);
                entry->release_keepalive();
            }

            if (buf->base) free(buf->base);
        });

    return entry;
}

// NEW: Create a writable stream from a TCP socket (internal helper)
static StreamEntryPtr create_writable_network_stream(uv_tcp_t* socket) {
    auto entry = std::make_shared<StreamEntry>();
    entry->id = g_next_stream_id.fetch_add(1);
    entry->type = StreamType::WRITABLE;
    entry->state = StreamState::OPEN;
    entry->tcp_handle = socket;
    entry->is_network_stream = true;

    // Register stream
    {
        std::lock_guard<std::mutex> lk(g_streams_mutex);
        g_streams[entry->id] = entry;
    }

    return entry;
}

ObjectPtr create_network_readable_stream_object(uv_tcp_t* socket) {
    auto entry = create_readable_network_stream(socket);
    return create_readable_stream_object(entry);  // ✅ CORRECT
}

ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket) {
    auto entry = create_writable_network_stream(socket);
    return create_writable_stream_object(entry);  // ✅ CORRECT
}