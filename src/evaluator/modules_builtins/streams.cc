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
    size_t high_water_mark = 16384;  // 16KB default
    std::string encoding = "binary";
    bool auto_close = true;
    size_t chunk_size = 4096;  // Read chunk size for readable streams
    std::string flags = "w";
};

static StreamOptions parse_stream_options(const Value& opts_val) {
    StreamOptions opts;

    if (!std::holds_alternative<ObjectPtr>(opts_val)) {
        return opts;  // Return defaults if not an object
    }

    ObjectPtr opts_obj = std::get<ObjectPtr>(opts_val);

    // Parse highWaterMark
    auto hwm_it = opts_obj->properties.find("highWaterMark");
    if (hwm_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(hwm_it->second.value)) {
            double val = std::get<double>(hwm_it->second.value);
            if (val > 0) {
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

    // Parse chunkSize (for readable streams)
    auto cs_it = opts_obj->properties.find("chunkSize");
    if (cs_it != opts_obj->properties.end()) {
        if (std::holds_alternative<double>(cs_it->second.value)) {
            double val = std::get<double>(cs_it->second.value);
            if (val > 0) {
                opts.chunk_size = static_cast<size_t>(val);
            }
        }
    }

    auto flags_it = opts_obj->properties.find("flags");
    if (flags_it != opts_obj->properties.end()) {
        if (std::holds_alternative<std::string>(flags_it->second.value)) {
            opts.flags = std::get<std::string>(flags_it->second.value);
        }
    }

    return opts;
}

// Base stream entry that holds all stream data and event listeners
struct StreamEntry : public std::enable_shared_from_this<StreamEntry> {
    long long id;
    StreamType type;
    std::atomic<StreamState> state{StreamState::OPEN};

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
    size_t chunk_size = 4096;
    std::string encoding = "binary";
};
using StreamEntryPtr = std::shared_ptr<StreamEntry>;

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

// Create stream wrapper object with all methods
static ObjectPtr create_stream_object(StreamEntryPtr entry);

// Push data into readable stream (internal API)
static bool push_data(StreamEntryPtr entry, BufferPtr data) {
    if (!entry || entry->state == StreamState::DESTROYED) return false;

    std::lock_guard<std::mutex> lk(entry->buffer_mutex);

    if (data) {
        // If in flowing mode, emit data immediately with encoding applied
        if (entry->state == StreamState::FLOWING) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> llk(entry->listeners_mutex);
                listeners = entry->data_listeners;
            }

            // ✅ APPLY ENCODING HERE
            Value encoded_data = encode_buffer_for_emission(data, entry->encoding);
            emit_event(entry, listeners, {encoded_data});
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
    if (entry->state == StreamState::DESTROYED) return false;

    // NEW: Transform stream execution path
    if (entry->type == StreamType::TRANSFORM && entry->transform_fn && entry->evaluator_ptr) {
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

// Pipe implementation
static void pipe_streams(StreamEntryPtr src, StreamEntryPtr dest) {
    if (!src || !dest) return;

    // Register pipe relationship
    {
        std::lock_guard<std::mutex> lk(src->pipe_mutex);
        src->piped_to.push_back(dest);
    }

    // Emit pipe event
    {
        std::lock_guard<std::mutex> lk(src->listeners_mutex);
        emit_event(src, src->pipe_listeners, {});
    }

    // Set up data forwarding
    auto data_forwarder = std::make_shared<FunctionValue>(
        "pipe_data_forwarder",
        [src, dest](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            if (!args.empty() && std::holds_alternative<BufferPtr>(args[0])) {
                BufferPtr chunk = std::get<BufferPtr>(args[0]);
                write_data(dest, chunk);
            }
            return std::monostate{};
        },
        nullptr, Token{});

    {
        std::lock_guard<std::mutex> lk(src->listeners_mutex);
        src->data_listeners.push_back(data_forwarder);
    }

    // Set up end forwarding
    auto end_forwarder = std::make_shared<FunctionValue>(
        "pipe_end_forwarder",
        [dest](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            dest->ended = true;
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> lk(dest->listeners_mutex);
                listeners = dest->end_listeners;
            }
            emit_event(dest, listeners, {});
            return std::monostate{};
        },
        nullptr, Token{});

    {
        std::lock_guard<std::mutex> lk(src->listeners_mutex);
        src->end_listeners.push_back(end_forwarder);
    }
}

// Create stream object with all methods
static ObjectPtr create_stream_object(StreamEntryPtr entry) {
    auto obj = std::make_shared<ObjectValue>();

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
        else if (event == "drain")
            entry->drain_listeners.push_back(cb);
        else if (event == "finish")
            entry->finish_listeners.push_back(cb);
        else if (event == "pipe")
            entry->pipe_listeners.push_back(cb);
        else if (event == "unpipe")
            entry->unpipe_listeners.push_back(cb);

        return std::monostate{};
    };
    Token tok = Token{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stream.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.read(n?)
    auto read_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        size_t n = 0;
        if (!args.empty() && std::holds_alternative<double>(args[0])) {
            n = static_cast<size_t>(std::get<double>(args[0]));
        }

        // ✅ USE ENCODED READ
        return read_data_encoded(entry, n);
    };
    obj->properties["read"] = {
        Value{std::make_shared<FunctionValue>("stream.read", read_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.write(chunk)
    auto write_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stream.write requires data argument", token.loc);
        }

        // ✅ DECODE INPUT BASED ON ENCODING
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

    // stream.pause()
    auto pause_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->state = StreamState::PAUSED;
        return std::monostate{};
    };
    obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("stream.pause", pause_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.resume()
    auto resume_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->state = StreamState::FLOWING;

        // Emit any buffered data WITH ENCODING APPLIED
        std::vector<BufferPtr> chunks;
        {
            std::lock_guard<std::mutex> lk(entry->buffer_mutex);
            chunks = std::vector<BufferPtr>(entry->buffer_queue.begin(),
                entry->buffer_queue.end());
            entry->buffer_queue.clear();
        }

        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->data_listeners;
        }

        // ✅ APPLY ENCODING TO EACH BUFFERED CHUNK
        for (const auto& chunk : chunks) {
            Value encoded_data = encode_buffer_for_emission(chunk, entry->encoding);
            emit_event(entry, listeners, {encoded_data});
        }

        return std::monostate{};
    };
    obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("stream.resume", resume_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.pipe(dest)
    auto pipe_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "pipe requires destination stream", token.loc);
        }

        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);

        // Set up data forwarding through object methods (respects wrappers!)
        auto data_forwarder = std::make_shared<FunctionValue>(
            "pipe_data_forwarder",
            [dest_obj](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                if (!args.empty() && std::holds_alternative<BufferPtr>(args[0])) {
                    auto write_prop = dest_obj->properties.find("write");
                    if (write_prop != dest_obj->properties.end() &&
                        std::holds_alternative<FunctionPtr>(write_prop->second.value)) {
                        FunctionPtr write_fn = std::get<FunctionPtr>(write_prop->second.value);
                        write_fn->native_impl(args, env, token);
                    }
                }
                return std::monostate{};
            },
            nullptr, Token{});

        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            entry->data_listeners.push_back(data_forwarder);
        }

        // Set up end forwarding through object methods
        auto end_forwarder = std::make_shared<FunctionValue>(
            "pipe_end_forwarder",
            [dest_obj](const std::vector<Value>&, EnvPtr env, const Token& token) -> Value {
                auto end_prop = dest_obj->properties.find("end");
                if (end_prop != dest_obj->properties.end() &&
                    std::holds_alternative<FunctionPtr>(end_prop->second.value)) {
                    FunctionPtr end_fn = std::get<FunctionPtr>(end_prop->second.value);
                    end_fn->native_impl({}, env, token);
                }
                return std::monostate{};
            },
            nullptr, Token{});

        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            entry->end_listeners.push_back(end_forwarder);
        }

        // Emit pipe event
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            emit_event(entry, entry->pipe_listeners, {});
        }

        return args[0];  // Return dest for chaining
    };
    obj->properties["pipe"] = {
        Value{std::make_shared<FunctionValue>("stream.pipe", pipe_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.destroy()
    auto destroy_impl = [entry](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        entry->state = StreamState::DESTROYED;
        entry->destroyed = true;

        // Close file if exists
        if (entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }

        // Emit close event
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->close_listeners;
        }
        emit_event(entry, listeners, {});

        // Remove from registry
        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams.erase(entry->id);
        }

        return std::monostate{};
    };
    obj->properties["destroy"] = {
        Value{std::make_shared<FunctionValue>("stream.destroy", destroy_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.end(chunk?)
    auto end_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        // Write final chunk if provided
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

        // Emit finish event
        std::vector<FunctionPtr> listeners;
        {
            std::lock_guard<std::mutex> lk(entry->listeners_mutex);
            listeners = entry->finish_listeners;
        }
        emit_event(entry, listeners, {});

        return std::monostate{};
    };
    obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("stream.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // Store stream ID as hidden property for pipe() to find it
    obj->properties["__stream_id__"] = {
        Value{static_cast<double>(entry->id)},
        true, false, true, tok};

    return obj;
}

// Factory: streams.readable(path, options?)
static Value native_createReadStream(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError", "streams.readable requires path argument", token.loc);
    }

    std::string path = value_to_string_simple_local(args[0]);

    // Parse options if provided
    StreamOptions opts;
    if (args.size() >= 2) {
        opts = parse_stream_options(args[1]);
    }

    auto entry = std::make_shared<StreamEntry>();
    entry->id = g_next_stream_id.fetch_add(1);
    entry->type = StreamType::READABLE;
    entry->source_path = path;
    entry->state = StreamState::FLOWING;

    // Apply options
    entry->high_water_mark = opts.high_water_mark;
    entry->auto_close = opts.auto_close;
    entry->chunk_size = opts.chunk_size;
    entry->encoding = opts.encoding;

    // Open file for reading
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
#else
    file->fd = ::open(path.c_str(), O_RDONLY);
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

    // Schedule async reading with configurable chunk size
    scheduler_run_on_loop([entry]() {
        const size_t chunk_size = entry->chunk_size;  // Use configured size
        std::vector<uint8_t> buffer(chunk_size);

        while (entry->state != StreamState::DESTROYED && !entry->ended) {
            // Pause if requested
            while (entry->state == StreamState::PAUSED) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            // Respect high water mark - pause reading if buffer is full
            while (entry->buffered_size >= entry->high_water_mark &&
                entry->state == StreamState::FLOWING) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            ssize_t bytes_read = 0;
#ifdef _WIN32
            DWORD read_count = 0;
            if (!ReadFile((HANDLE)entry->file_handle->handle,
                    buffer.data(),
                    static_cast<DWORD>(chunk_size),
                    &read_count,
                    nullptr)) {
                break;
            }
            bytes_read = read_count;
#else
            bytes_read = ::read(entry->file_handle->fd, buffer.data(), chunk_size);
#endif

            if (bytes_read <= 0) {
                // EOF or error
                push_data(entry, nullptr);  // Signal end
                break;
            }

            auto chunk = std::make_shared<BufferValue>();
            chunk->data.assign(buffer.begin(), buffer.begin() + bytes_read);
            push_data(entry, chunk);
        }

        // Close file if autoClose is enabled
        if (entry->auto_close && entry->file_handle && entry->file_handle->is_open) {
            entry->file_handle->close_internal();
        }
    });

    return Value{create_stream_object(entry)};
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

    return Value{create_stream_object(entry)};
}

// Factory: streams.create(source, mode, options?/function)
static Value native_createStream(const std::vector<Value>& args, EnvPtr env, const Token& token, Evaluator* evaluator) {
    if (args.size() < 2) {
        throw SwaziError("TypeError",
            "streams.create requires (source, mode, ...) arguments",
            token.loc);
    }

    std::string mode = value_to_string_simple_local(args[1]);

    // ============ READABLE MODE ============
    if (mode == "r" || mode == "read") {
        // Signature: create(path, "r", options?)
        std::vector<Value> read_args;
        read_args.push_back(args[0]);  // path

        // If 3rd arg exists and is NOT a function, treat as options
        if (args.size() >= 3 && !std::holds_alternative<FunctionPtr>(args[2])) {
            read_args.push_back(args[2]);  // options object
        }

        return native_createReadStream(read_args, env, token);
    }

    // ============ WRITABLE MODE ============
    else if (mode == "w" || mode == "write") {
        // Signature: create(path, "w", options?)
        std::vector<Value> write_args;
        write_args.push_back(args[0]);  // path

        // If 3rd arg exists and is NOT a function, treat as options
        if (args.size() >= 3 && !std::holds_alternative<FunctionPtr>(args[2])) {
            write_args.push_back(args[2]);  // options object
        }

        return native_createWriteStream(write_args, env, token);
    }

    // ============ DUPLEX MODE ============
    else if (mode == "d" || mode == "duplex") {
        // Signature: create(source, "d", options?)
        StreamOptions opts;
        if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
            opts = parse_stream_options(args[2]);
        }

        auto entry = std::make_shared<StreamEntry>();
        entry->id = g_next_stream_id.fetch_add(1);
        entry->type = StreamType::DUPLEX;
        entry->state = StreamState::FLOWING;  // ✅ ADD THIS - duplex should also flow

        // Apply options
        entry->high_water_mark = opts.high_water_mark;
        entry->auto_close = opts.auto_close;
        entry->encoding = opts.encoding;

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams[entry->id] = entry;
        }

        return Value{create_stream_object(entry)};
    }

    // ============ TRANSFORM MODE ============
    else if (mode == "t" || mode == "transform") {
        auto entry = std::make_shared<StreamEntry>();
        entry->id = g_next_stream_id.fetch_add(1);
        entry->type = StreamType::TRANSFORM;
        entry->state = StreamState::FLOWING;  // ✅ Already correct - start in flowing mode
        entry->evaluator_ptr = evaluator;

        // Extract transform function (3rd arg MUST be a function)
        if (args.size() < 3 || !std::holds_alternative<FunctionPtr>(args[2])) {
            throw SwaziError("TypeError",
                "Transform mode requires a transform function as 3rd argument. "
                "Usage: create(source, 't', transformFn, options?)",
                token.loc);
        }

        entry->transform_fn = std::get<FunctionPtr>(args[2]);

        // Parse options if 4th arg exists and is an object
        StreamOptions opts;
        if (args.size() >= 4 && std::holds_alternative<ObjectPtr>(args[3])) {
            opts = parse_stream_options(args[3]);
        }

        // Apply options
        entry->high_water_mark = opts.high_water_mark;
        entry->auto_close = opts.auto_close;
        entry->encoding = opts.encoding;

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams[entry->id] = entry;
        }

        return Value{create_stream_object(entry)};
    }

    else {
        throw SwaziError("ValueError",
            "Invalid stream mode. Use 'r', 'w', 'd', or 't'",
            token.loc);
    }
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
    return create_stream_object(entry);
}

ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket) {
    auto entry = create_writable_network_stream(socket);
    return create_stream_object(entry);
}