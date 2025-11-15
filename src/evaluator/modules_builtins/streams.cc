// streams.cc - Comprehensive streams module for Swazi language
// Provides readable/writable/duplex/transform stream abstractions

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
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
        // If in flowing mode, emit data immediately without buffering
        if (entry->state == StreamState::FLOWING) {
            std::vector<FunctionPtr> listeners;
            {
                std::lock_guard<std::mutex> llk(entry->listeners_mutex);
                listeners = entry->data_listeners;
            }
            emit_event(entry, listeners, {Value{data}});
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

// Write data to writable stream
static bool write_data(StreamEntryPtr entry, BufferPtr data) {
    if (!entry || !data) return false;
    if (entry->state == StreamState::DESTROYED) return false;

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

        BufferPtr data = read_data(entry, n);
        if (!data) return std::monostate{};
        return Value{data};
    };
    obj->properties["read"] = {
        Value{std::make_shared<FunctionValue>("stream.read", read_impl, nullptr, tok)},
        false, false, false, tok};

    // stream.write(chunk)
    auto write_impl = [entry](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stream.write requires data argument", token.loc);
        }

        BufferPtr buf;
        if (std::holds_alternative<BufferPtr>(args[0])) {
            buf = std::get<BufferPtr>(args[0]);
        } else if (std::holds_alternative<std::string>(args[0])) {
            buf = std::make_shared<BufferValue>();
            std::string s = std::get<std::string>(args[0]);
            buf->data.assign(s.begin(), s.end());
        } else {
            throw SwaziError("TypeError", "write expects Buffer or string", token.loc);
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

        // Emit any buffered data
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

        for (const auto& chunk : chunks) {
            emit_event(entry, listeners, {Value{chunk}});
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

        // Extract destination stream entry from object
        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);
        auto it = dest_obj->properties.find("__stream_id__");
        if (it == dest_obj->properties.end() || !std::holds_alternative<double>(it->second.value)) {
            throw SwaziError("TypeError", "destination is not a valid stream", token.loc);
        }

        long long dest_id = static_cast<long long>(std::get<double>(it->second.value));
        StreamEntryPtr dest;
        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            auto sit = g_streams.find(dest_id);
            if (sit != g_streams.end()) dest = sit->second;
        }

        if (!dest) {
            throw SwaziError("RuntimeError", "destination stream not found", token.loc);
        }

        pipe_streams(entry, dest);
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

// Factory: streams.read(path, options?)
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

// Factory: streams.write(path, options?)
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
    file->mode = "wb";
    file->is_binary = true;

#ifdef _WIN32
    file->handle = CreateFileA(path.c_str(), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file->handle == INVALID_HANDLE_VALUE) {
        throw SwaziError("IOError", "Failed to open file: " + path, token.loc);
    }
#else
    file->fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
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

// Factory: streams.create(source, mode)
static Value native_createStream(const std::vector<Value>& args, EnvPtr env, const Token& token) {
    if (args.size() < 2) {
        throw SwaziError("TypeError",
            "streams.create requires (source, mode) arguments",
            token.loc);
    }

    std::string mode = value_to_string_simple_local(args[1]);

    // Parse options if provided (3rd argument)
    StreamOptions opts;
    if (args.size() >= 3 && !std::holds_alternative<FunctionPtr>(args[2])) {
        opts = parse_stream_options(args[2]);
    }

    if (mode == "r" || mode == "read") {
        return native_createReadStream(args, env, token);
    } else if (mode == "w" || mode == "write") {
        return native_createWriteStream(args, env, token);
    } else if (mode == "d" || mode == "duplex") {
        // Create duplex stream (both readable and writable)
        auto entry = std::make_shared<StreamEntry>();
        entry->id = g_next_stream_id.fetch_add(1);
        entry->type = StreamType::DUPLEX;

        // Apply options
        entry->high_water_mark = opts.high_water_mark;
        entry->auto_close = opts.auto_close;
        entry->encoding = opts.encoding;

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams[entry->id] = entry;
        }

        return Value{create_stream_object(entry)};
    } else if (mode == "t" || mode == "transform") {
        // Create transform stream
        auto entry = std::make_shared<StreamEntry>();
        entry->id = g_next_stream_id.fetch_add(1);
        entry->type = StreamType::TRANSFORM;

        // Apply options
        entry->high_water_mark = opts.high_water_mark;
        entry->auto_close = opts.auto_close;
        entry->encoding = opts.encoding;

        // Store transform function if provided as last arg and it's a function
        if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2])) {
            entry->transform_fn = std::get<FunctionPtr>(args[2]);
        } else if (args.size() >= 4 && std::holds_alternative<FunctionPtr>(args[3])) {
            entry->transform_fn = std::get<FunctionPtr>(args[3]);
        }

        {
            std::lock_guard<std::mutex> lk(g_streams_mutex);
            g_streams[entry->id] = entry;
        }

        return Value{create_stream_object(entry)};
    } else {
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
std::shared_ptr<ObjectValue> make_streams_exports(EnvPtr env) {
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
        Value{std::make_shared<FunctionValue>("streams.create", native_createStream, env, tok)},
        false, false, false, tok};

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