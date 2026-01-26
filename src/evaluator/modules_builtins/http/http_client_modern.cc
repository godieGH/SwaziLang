// http_client_modern.cc
// Modern event-driven HTTP/HTTPS client with streaming support
// Supports: GET, POST, PUT, DELETE, PATCH, upload/download streaming, pause/resume, HTTPS

#include <llhttp.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// OpenSSL includes for HTTPS
#ifdef HAVE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

// ============================================================================
// CASE-INSENSITIVE HEADER MAP (shared with server)
// ============================================================================

struct CaseInsensitiveCompare {
    bool operator()(const std::string& a, const std::string& b) const {
        return std::lexicographical_compare(
            a.begin(), a.end(),
            b.begin(), b.end(),
            [](unsigned char ca, unsigned char cb) {
                return std::tolower(ca) < std::tolower(cb);
            });
    }
};

class HeaderMap {
   private:
    std::map<std::string, std::string, CaseInsensitiveCompare> headers_;
    std::map<std::string, std::string, CaseInsensitiveCompare> original_case_;

   public:
    void set(const std::string& name, const std::string& value) {
        headers_[name] = value;
        if (original_case_.find(name) == original_case_.end()) {
            original_case_[name] = name;
        }
    }

    std::optional<std::string> get(const std::string& name) const {
        auto it = headers_.find(name);
        if (it == headers_.end()) return std::nullopt;
        return it->second;
    }

    bool has(const std::string& name) const {
        return headers_.find(name) != headers_.end();
    }

    void remove(const std::string& name) {
        headers_.erase(name);
        original_case_.erase(name);
    }

    void clear() {
        headers_.clear();
        original_case_.clear();
    }

    class iterator {
       private:
        std::map<std::string, std::string, CaseInsensitiveCompare>::const_iterator it_;
        const std::map<std::string, std::string, CaseInsensitiveCompare>* original_case_;

       public:
        iterator(
            std::map<std::string, std::string, CaseInsensitiveCompare>::const_iterator it,
            const std::map<std::string, std::string, CaseInsensitiveCompare>* original_case)
            : it_(it), original_case_(original_case) {}

        iterator& operator++() {
            ++it_;
            return *this;
        }
        bool operator!=(const iterator& other) const { return it_ != other.it_; }

        std::pair<std::string, std::string> operator*() const {
            auto orig_it = original_case_->find(it_->first);
            std::string original_name = (orig_it != original_case_->end())
                ? orig_it->second
                : it_->first;
            return {original_name, it_->second};
        }
    };

    iterator begin() const {
        return iterator(headers_.begin(), &original_case_);
    }

    iterator end() const {
        return iterator(headers_.end(), &original_case_);
    }

    size_t size() const {
        return headers_.size();
    }

    bool empty() const {
        return headers_.empty();
    }
};

// ============================================================================
// STRUCTURES
// ============================================================================

struct StreamEventHandlers {
    FunctionPtr on_response;
    FunctionPtr on_data;
    FunctionPtr on_end;
    FunctionPtr on_error;
    FunctionPtr on_progress;
    FunctionPtr on_connect;
    FunctionPtr on_close;
    FunctionPtr on_drain;
    FunctionPtr on_upload_progress;
    EnvPtr env;
    Evaluator* evaluator;
    std::mutex mutex;
};

struct WriteRequest {
    std::vector<uint8_t> data;
    FunctionPtr callback;
    size_t body_bytes = 0;
};

struct HttpClientRequest {
    uv_tcp_t socket;
    uv_connect_t connect_req;

    llhttp_t parser;
    llhttp_settings_t settings;

    std::string host;
    int port = 80;
    std::string method;
    std::string path;
    HeaderMap request_headers;
    std::shared_ptr<HeaderMap> response_headers;
    long status_code = 0;
    std::shared_ptr<std::string> status_text;

    std::string url;
    std::string current_header_field;

    std::atomic<bool> paused{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> closed{false};
    std::atomic<bool> headers_sent{false};
    std::atomic<bool> request_complete{false};
    std::atomic<bool> response_headers_received{false};

    size_t total_bytes_received = 0;
    size_t content_length = 0;
    size_t total_bytes_sent = 0;
    size_t body_bytes_sent = 0;
    size_t upload_size = 0;

    std::shared_ptr<StreamEventHandlers> handlers;
    Evaluator* evaluator = nullptr;

    // Upload queue
    std::deque<WriteRequest> write_queue;
    std::atomic<bool> writing{false};
    std::mutex write_mutex;

    bool is_chunked = false;           // True if we are in "client.write" mode
    bool has_fixed_body = false;       // True if 'body' option was provided
    std::vector<uint8_t> body_buffer;  // For string/buffer body

    // File streaming support
    FilePtr file_source = nullptr;  // For file body
    uint64_t file_size = 0;
    uint64_t file_bytes_sent = 0;
    std::vector<uint8_t> file_read_buffer;

    // HTTPS support
    bool use_ssl = false;
#ifdef HAVE_OPENSSL
    SSL* ssl = nullptr;
    SSL_CTX* ssl_ctx = nullptr;
    BIO* bio_read = nullptr;
    BIO* bio_write = nullptr;
#endif
};

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

static void emit_event(std::shared_ptr<StreamEventHandlers> handlers,
    const std::string& event,
    const Value& data = std::monostate{});

static void queue_write(HttpClientRequest* req, const std::vector<uint8_t>& data,
    FunctionPtr callback = nullptr, size_t body_bytes = 0);

static void stream_next_file_chunk(HttpClientRequest* req);

static void close_connection(HttpClientRequest* req, bool emit_close_event);

static void cleanup_ssl(HttpClientRequest* req);

// ============================================================================
// GLOBAL STATE
// ============================================================================

static std::atomic<int> g_active_http_requests{0};

bool http_has_active_work() {
    return g_active_http_requests.load() > 0;
}

// ============================================================================
// HELPER: String conversions
// ============================================================================

static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::string();
}

static void close_connection_internal(HttpClientRequest* req, bool emit_close_event) {
    // This MUST be called on the event loop thread

    if (req->closed.exchange(true)) {
        // Already closed, do nothing
        return;
    }

    // Decrement active requests counter ONCE
    g_active_http_requests.fetch_sub(1);

    // Capture handlers in shared_ptr to keep it alive
    auto handlers = req->handlers;

    // Emit close event if requested
    if (emit_close_event && handlers) {
        emit_event(handlers, "close");
    }

#ifdef HAVE_OPENSSL
    if (req->use_ssl) {
        cleanup_ssl(req);
    }
#endif

    // Close the socket handle
    // The deletion happens in the uv_close callback
    if (!uv_is_closing((uv_handle_t*)&req->socket)) {
        uv_close((uv_handle_t*)&req->socket, [](uv_handle_t* handle) {
            auto* req = static_cast<HttpClientRequest*>(handle->data);
            delete req;
        });
    } else {
        // Socket is already closing, just delete
        // This shouldn't normally happen with proper coordination
        // delete req;
    }
}

// ============================================================================
// EVENT EMISSION
// ============================================================================

static void emit_event(std::shared_ptr<StreamEventHandlers> handlers,
    const std::string& event,
    const Value& data) {
    if (!handlers) return;

    FunctionPtr fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(handlers->mutex);
        if (event == "response")
            fn = handlers->on_response;
        else if (event == "data")
            fn = handlers->on_data;
        else if (event == "end")
            fn = handlers->on_end;
        else if (event == "error")
            fn = handlers->on_error;
        else if (event == "progress")
            fn = handlers->on_progress;
        else if (event == "connect")
            fn = handlers->on_connect;
        else if (event == "close")
            fn = handlers->on_close;
        else if (event == "drain")
            fn = handlers->on_drain;
        else if (event == "uploadProgress")
            fn = handlers->on_upload_progress;
    }

    if (!fn) return;

    std::vector<Value> args;
    if (!std::holds_alternative<std::monostate>(data)) {
        args.push_back(data);
    }

    scheduler_run_on_loop([fn, args, handlers]() {
        try {
            CallbackPayload* payload = new CallbackPayload(fn, args);
            enqueue_callback_global(static_cast<void*>(payload));
        } catch (...) {}
    });
}

// ============================================================================
// SSL/TLS SUPPORT
// ============================================================================

#ifdef HAVE_OPENSSL
static void init_openssl() {
    static bool initialized = false;
    if (!initialized) {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        initialized = true;
    }
}

static bool setup_ssl(HttpClientRequest* req) {
    init_openssl();

    req->ssl_ctx = SSL_CTX_new(TLS_client_method());
    if (!req->ssl_ctx) {
        return false;
    }

    SSL_CTX_set_verify(req->ssl_ctx, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_options(req->ssl_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    req->ssl = SSL_new(req->ssl_ctx);
    if (!req->ssl) {
        SSL_CTX_free(req->ssl_ctx);
        return false;
    }

    req->bio_read = BIO_new(BIO_s_mem());
    req->bio_write = BIO_new(BIO_s_mem());

    SSL_set_bio(req->ssl, req->bio_read, req->bio_write);
    SSL_set_connect_state(req->ssl);
    SSL_set_tlsext_host_name(req->ssl, req->host.c_str());

    return true;
}

static void cleanup_ssl(HttpClientRequest* req) {
    if (req->ssl) {
        SSL_free(req->ssl);
        req->ssl = nullptr;
    }
    if (req->ssl_ctx) {
        SSL_CTX_free(req->ssl_ctx);
        req->ssl_ctx = nullptr;
    }
}

static int do_ssl_handshake(HttpClientRequest* req) {
    int ret = SSL_do_handshake(req->ssl);
    if (ret == 1) {
        return 1;  // Success
    }

    int err = SSL_get_error(req->ssl, ret);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return 0;  // Need more data
    }

    return -1;  // Error
}
#endif

// ============================================================================
// LLHTTP CALLBACKS
// ============================================================================

static int on_status(llhttp_t* parser, const char* at, size_t length) {
    auto* req = static_cast<HttpClientRequest*>(parser->data);
    req->status_code = parser->status_code;
    req->status_text->assign(at, length);
    return 0;
}

static int on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* req = static_cast<HttpClientRequest*>(parser->data);
    req->current_header_field.assign(at, length);
    for (auto& c : req->current_header_field) c = std::tolower(c);
    return 0;
}

static int on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* req = static_cast<HttpClientRequest*>(parser->data);
    std::string value(at, length);

    req->response_headers->set(req->current_header_field, value);

    if (req->current_header_field == "content-length") {
        try {
            req->content_length = std::stoull(value);
        } catch (...) {
            req->content_length = 0;
        }
    }

    return 0;
}

static int on_headers_complete(llhttp_t* parser) {
    auto* req = static_cast<HttpClientRequest*>(parser->data);
    req->response_headers_received.store(true);

    // Build ResponseMeta object
    auto meta = std::make_shared<ObjectValue>();
    meta->properties["status"] = PropertyDescriptor{
        Value{static_cast<double>(req->status_code)}, false, false, true, Token{}};
    meta->properties["statusText"] = PropertyDescriptor{
        Value{*req->status_text}, false, false, true, Token{}};
    meta->properties["url"] = PropertyDescriptor{
        Value{req->url}, false, false, true, Token{}};

    auto headers_obj = std::make_shared<ObjectValue>();
    for (auto it = req->response_headers->begin(); it != req->response_headers->end(); ++it) {
        auto kv = *it;
        headers_obj->properties[kv.first] = PropertyDescriptor{
            Value{kv.second}, false, false, true, Token{}};
    }
    meta->properties["headers"] = PropertyDescriptor{
        Value{headers_obj}, false, false, true, Token{}};

    emit_event(req->handlers, "response", Value{meta});

    return 0;
}

static int on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* req = static_cast<HttpClientRequest*>(parser->data);

    req->total_bytes_received += length;

    // Emit data chunk
    auto chunk = std::make_shared<BufferValue>();
    chunk->data.assign(at, at + length);
    chunk->encoding = "binary";

    emit_event(req->handlers, "data", Value{chunk});

    // Emit progress if we know content length
    if (req->content_length > 0) {
        auto progress = std::make_shared<ObjectValue>();
        progress->properties["loaded"] = PropertyDescriptor{
            Value{static_cast<double>(req->total_bytes_received)}, false, false, true, Token{}};
        progress->properties["total"] = PropertyDescriptor{
            Value{static_cast<double>(req->content_length)}, false, false, true, Token{}};
        progress->properties["percentage"] = PropertyDescriptor{
            Value{(static_cast<double>(req->total_bytes_received) / req->content_length) * 100.0},
            false, false, true, Token{}};

        emit_event(req->handlers, "progress", Value{progress});
    }

    return 0;
}

static int on_message_complete(llhttp_t* parser) {
    auto* req = static_cast<HttpClientRequest*>(parser->data);

    if (req->closed.load()) {
        return 0;
    }

    auto handlers = req->handlers;
    emit_event(handlers, "end");

    close_connection(req, true);

    return 0;
}

// ============================================================================
// WRITE OPERATIONS
// ============================================================================

static void process_write_queue(HttpClientRequest* req);

static void on_write_complete(uv_write_t* write_req, int status) {
    auto* req = static_cast<HttpClientRequest*>(write_req->data);
    auto handlers = req->handlers;

    if (write_req->bufs) {
        if (write_req->bufs->base) free(write_req->bufs->base);
        delete write_req->bufs;
    }
    delete write_req;

    if (status < 0) {
        std::string error = std::string("Write error: ") + uv_strerror(status);
        emit_event(handlers, "error", Value{error});
        close_connection(req, true);
        return;
    }

    req->writing.store(false);
    process_write_queue(req);
}

static void process_write_queue(HttpClientRequest* req) {
    if (req->closed.load() || req->writing.load()) return;

    WriteRequest wr;
    {
        std::lock_guard<std::mutex> lock(req->write_mutex);
        if (req->write_queue.empty()) {
            emit_event(req->handlers, "drain");
            return;
        }
        wr = std::move(req->write_queue.front());
        req->write_queue.pop_front();
    }

    req->writing.store(true);

    // wire bytes (diagnostic)
    req->total_bytes_sent += wr.data.size();

    // body bytes (logical payload) — only increment by declared amount
    if (wr.body_bytes > 0) {
        req->body_bytes_sent += wr.body_bytes;
        // Avoid overflow past upload_size
        if (req->upload_size > 0 && req->body_bytes_sent > req->upload_size) {
            req->body_bytes_sent = req->upload_size;
        }
    }

    // Emit uploadProgress based on logical body bytes (backwards-compatible shape)
    // Also provide wireBytes (diagnostic) so future consumers can inspect physical bytes.
    if (req->upload_size > 0) {
        auto progress = std::make_shared<ObjectValue>();
        progress->properties["loaded"] = PropertyDescriptor{
            Value{static_cast<double>(req->body_bytes_sent)}, false, false, true, Token{}};
        progress->properties["total"] = PropertyDescriptor{
            Value{static_cast<double>(req->upload_size)}, false, false, true, Token{}};

        // extra, non-breaking field for diagnostics
        progress->properties["wireBytes"] = PropertyDescriptor{
            Value{static_cast<double>(req->total_bytes_sent)}, false, false, true, Token{}};

        // optional percentage (keeps semantics)
        progress->properties["percentage"] = PropertyDescriptor{
            Value{(req->upload_size > 0) ? (static_cast<double>(req->body_bytes_sent) / req->upload_size) * 100.0 : 0.0},
            false, false, true, Token{}};

        emit_event(req->handlers, "uploadProgress", Value{progress});
    }

#ifdef HAVE_OPENSSL
    if (req->use_ssl && req->ssl) {
        // Write to SSL
        int written = SSL_write(req->ssl, wr.data.data(), wr.data.size());

        // Get encrypted data from BIO
        char buf[16384];
        int pending = BIO_pending(req->bio_write);
        if (pending > 0) {
            int read = BIO_read(req->bio_write, buf, std::min(pending, (int)sizeof(buf)));
            if (read > 0) {
                char* send_buf = (char*)malloc(read);
                memcpy(send_buf, buf, read);

                uv_write_t* write_req = new uv_write_t;
                write_req->data = req;

                uv_buf_t* uvbuf = new uv_buf_t;
                *uvbuf = uv_buf_init(send_buf, read);
                write_req->bufs = uvbuf;

                uv_write(write_req, (uv_stream_t*)&req->socket, uvbuf, 1, on_write_complete);
                return;
            }
        }

        req->writing.store(false);
        process_write_queue(req);
        return;
    }
#endif

    // Plain TCP write
    char* send_buf = (char*)malloc(wr.data.size());
    memcpy(send_buf, wr.data.data(), wr.data.size());

    uv_write_t* write_req = new uv_write_t;
    write_req->data = req;

    uv_buf_t* uvbuf = new uv_buf_t;
    *uvbuf = uv_buf_init(send_buf, (unsigned int)wr.data.size());
    write_req->bufs = uvbuf;

    int result = uv_write(write_req, (uv_stream_t*)&req->socket, uvbuf, 1, on_write_complete);

    if (result < 0) {
        free(send_buf);
        delete uvbuf;
        delete write_req;
        req->writing.store(false);

        std::string error = std::string("Write failed: ") + uv_strerror(result);
        emit_event(req->handlers, "error", Value{error});
    }

    if (wr.callback) {
        emit_event(req->handlers, "drain");
    }
}

static void queue_write(HttpClientRequest* req, const std::vector<uint8_t>& data, FunctionPtr callback, size_t body_bytes) {
    WriteRequest wr;
    wr.data = data;
    wr.callback = callback;
    wr.body_bytes = body_bytes;

    {
        std::lock_guard<std::mutex> lock(req->write_mutex);
        req->write_queue.push_back(std::move(wr));
    }

    if (!req->writing.load()) {
        scheduler_run_on_loop([req]() {
            process_write_queue(req);
        });
    }
}

// ============================================================================
// READ OPERATIONS
// ============================================================================

static void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = suggested_size;
}

static void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* req = static_cast<HttpClientRequest*>(stream->data);
    auto handlers = req->handlers;

    // Early exit if already closed
    if (req->closed.load()) {
        if (buf->base) free(buf->base);
        return;
    }

    if (nread > 0) {
#ifdef HAVE_OPENSSL
        if (req->use_ssl && req->ssl) {
            BIO_write(req->bio_read, buf->base, nread);

            if (!SSL_is_init_finished(req->ssl)) {
                int hs_result = do_ssl_handshake(req);

                if (hs_result < 0) {
                    emit_event(handlers, "error", Value{std::string("SSL handshake failed")});
                    if (buf->base) free(buf->base);
                    close_connection(req, true);
                    return;
                }

                char out_buf[16384];
                int pending = BIO_pending(req->bio_write);
                if (pending > 0) {
                    int read = BIO_read(req->bio_write, out_buf, std::min(pending, (int)sizeof(out_buf)));
                    if (read > 0) {
                        std::vector<uint8_t> data(out_buf, out_buf + read);
                        queue_write(req, data, nullptr, 0);
                    }
                }

                if (hs_result == 1) {
                    std::ostringstream request_stream;
                    request_stream << req->method << " " << req->path << " HTTP/1.1\r\n";
                    request_stream << "Host: " << req->host << "\r\n";

                    for (auto it = req->request_headers.begin(); it != req->request_headers.end(); ++it) {
                        auto kv = *it;
                        request_stream << kv.first << ": " << kv.second << "\r\n";
                    }

                    request_stream << "\r\n";

                    std::string req_str = request_stream.str();
                    std::vector<uint8_t> req_data(req_str.begin(), req_str.end());
                    queue_write(req, req_data);
                    req->headers_sent.store(true);

                    emit_event(handlers, "connect");
                }

                if (buf->base) free(buf->base);
                return;
            }

            char decrypt_buf[16384];
            int bytes_read;

            while ((bytes_read = SSL_read(req->ssl, decrypt_buf, sizeof(decrypt_buf))) > 0) {
                llhttp_errno_t err = llhttp_execute(&req->parser, decrypt_buf, bytes_read);

                if (err != HPE_OK) {
                    std::string error = std::string("HTTP parse error: ") + llhttp_errno_name(err);
                    emit_event(handlers, "error", Value{error});
                    if (buf->base) free(buf->base);
                    close_connection(req, true);
                    return;
                }
            }

            if (buf->base) free(buf->base);
            return;
        }
#endif

        // Plain HTTP parsing
        llhttp_errno_t err = llhttp_execute(&req->parser, buf->base, nread);

        if (err != HPE_OK) {
            std::string error = std::string("HTTP parse error: ") + llhttp_errno_name(err);
            emit_event(handlers, "error", Value{error});
            if (buf->base) free(buf->base);
            close_connection(req, true);
            return;
        }

    } else if (nread < 0) {
        // Connection closed or error
        if (nread != UV_EOF) {
            std::string error = std::string("Read error: ") + uv_strerror(nread);
            emit_event(handlers, "error", Value{error});
        }

        if (buf->base) free(buf->base);
        close_connection(req, true);
        return;
    }

    if (buf->base) free(buf->base);
}

// ============================================================================
// CONNECTION
// ============================================================================

static void send_initial_request(HttpClientRequest* req) {
    if (req->use_ssl) {
#ifdef HAVE_OPENSSL
        char buf[16384];
        int pending = BIO_pending(req->bio_write);
        if (pending > 0) {
            int read = BIO_read(req->bio_write, buf, std::min(pending, (int)sizeof(buf)));
            if (read > 0) {
                std::vector<uint8_t> data(buf, buf + read);
                queue_write(req, data);
            }
        }
#endif
        return;
    }

    std::ostringstream request_stream;
    request_stream << req->method << " " << req->path << " HTTP/1.1\r\n";
    request_stream << "Host: " << req->host << "\r\n";
    request_stream << "Connection: close\r\n";  // Always close connection

    for (auto it = req->request_headers.begin(); it != req->request_headers.end(); ++it) {
        auto kv = *it;
        request_stream << kv.first << ": " << kv.second << "\r\n";
    }

    request_stream << "\r\n";

    std::string req_str = request_stream.str();
    std::vector<uint8_t> req_data(req_str.begin(), req_str.end());

    // Send Headers
    queue_write(req, req_data, nullptr, 0);
    req->headers_sent.store(true);
    emit_event(req->handlers, "connect");

    // ROUTE BASED ON STRATEGY
    if (req->has_fixed_body) {
        if (req->file_source) {
            // Strategy: File Stream
            scheduler_run_on_loop([req]() {
                stream_next_file_chunk(req);
            });
        } else {
            // Strategy: Memory Buffer
            if (!req->body_buffer.empty()) {
                queue_write(req, req->body_buffer, nullptr, req->body_buffer.size());
            }
            req->request_complete.store(true);
        }
    }
}

static void on_connect(uv_connect_t* connect_req, int status) {
    auto* req = static_cast<HttpClientRequest*>(connect_req->data);
    auto handlers = req->handlers;

    if (status < 0) {
        std::string error = std::string("Connection failed: ") + uv_strerror(status);
        emit_event(handlers, "error", Value{error});
        close_connection(req, true);
        return;
    }

    req->connected.store(true);
    uv_read_start((uv_stream_t*)&req->socket, alloc_buffer, on_read);
    send_initial_request(req);
}

static void stream_next_file_chunk(HttpClientRequest* req) {
    if (req->closed.load()) return;

    const size_t CHUNK_SIZE = 64 * 1024;  // 64KB chunks
    std::vector<uint8_t> buffer(CHUNK_SIZE);
    size_t bytes_read = 0;

    // Platform specific read
#ifdef _WIN32
    DWORD read_bytes;
    if (ReadFile((HANDLE)req->file_source->handle, buffer.data(), CHUNK_SIZE, &read_bytes, NULL)) {
        bytes_read = read_bytes;
    }
#else
    ssize_t r = read(req->file_source->fd, buffer.data(), CHUNK_SIZE);
    if (r > 0) bytes_read = r;
#endif

    if (bytes_read > 0) {
        buffer.resize(bytes_read);

        auto callback_impl = [req](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            scheduler_run_on_loop([req]() {
                stream_next_file_chunk(req);
            });
            return std::monostate{};
        };

        auto callback_fn = std::make_shared<FunctionValue>(
            "file_chunk_callback",
            callback_impl,
            nullptr,
            Token{});

        queue_write(req, buffer, callback_fn, buffer.size());
    } else {
        // EOF or Error
        req->request_complete.store(true);
        emit_event(req->handlers, "drain");
    }
}

static void close_connection(HttpClientRequest* req, bool emit_close_event = true) {
    // Always schedule on the loop to ensure thread safety
    auto handlers = req->handlers;  // Keep handlers alive

    scheduler_run_on_loop([req, emit_close_event, handlers]() {
        close_connection_internal(req, emit_close_event);
    });
}

// ============================================================================
// PUBLIC API: http.open(url, options)
// ============================================================================

Value native_http_open(const std::vector<Value>& args, EnvPtr callEnv, const Token& token, Evaluator* evaluator) {
    if (args.empty()) {
        throw SwaziError("TypeError", "http.open requires url", token.loc);
    }

    std::string url = value_to_string_simple(args[0]);

    // Parse URL
    std::regex url_regex(R"(^(https?)://([^/:]+)(?::(\d+))?(/.*)?$)");
    std::smatch matches;

    if (!std::regex_match(url, matches, url_regex)) {
        throw SwaziError("TypeError", "Invalid URL format", token.loc);
    }

    std::string protocol = matches[1];
    std::string host = matches[2];
    int port = matches[3].length() > 0 ? std::stoi(matches[3]) : (protocol == "https" ? 443 : 80);
    std::string path = matches[4].length() > 0 ? matches[4].str() : "/";

    bool use_ssl = (protocol == "https");

#ifndef HAVE_OPENSSL
    if (use_ssl) {
        throw SwaziError("NotImplementedError",
            "HTTPS not available - rebuild with OpenSSL support", token.loc);
    }
#endif

    // Parse options
    std::string method = "GET";
    HeaderMap headers;

    // Create request object
    auto* req = new HttpClientRequest();
    req->host = host;
    req->port = port;
    req->method = method;
    req->path = path;
    req->url = url;
    req->use_ssl = use_ssl;

    if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
        ObjectPtr opts = std::get<ObjectPtr>(args[1]);

        auto method_it = opts->properties.find("method");
        if (method_it != opts->properties.end()) {
            req->method = value_to_string_simple(method_it->second.value);
            method = req->method;
        }

        auto headers_it = opts->properties.find("headers");
        if (headers_it != opts->properties.end() &&  // ✅ Compare to opts->properties.end()
            std::holds_alternative<ObjectPtr>(headers_it->second.value)) {
            ObjectPtr hdrs = std::get<ObjectPtr>(headers_it->second.value);
            for (const auto& kv : hdrs->properties) {
                headers.set(kv.first, value_to_string_simple(kv.second.value));
            }
        }
        auto it_body = opts->properties.find("body");
        if (it_body != opts->properties.end()) {
            const Value& body_val = it_body->second.value;

            if (std::holds_alternative<std::string>(body_val)) {
                std::string str = std::get<std::string>(body_val);
                req->body_buffer.assign(str.begin(), str.end());
                req->has_fixed_body = true;
            } else if (std::holds_alternative<BufferPtr>(body_val)) {
                BufferPtr buf = std::get<BufferPtr>(body_val);
                req->body_buffer = buf->data;
                req->has_fixed_body = true;
            } else if (std::holds_alternative<FilePtr>(body_val)) {
                req->file_source = std::get<FilePtr>(body_val);
                if (!req->file_source->is_open) {
                    delete req;
                    throw SwaziError("IOError", "File must be open for upload", token.loc);
                }
                req->has_fixed_body = true;

                // Get File Size
#ifdef _WIN32
                LARGE_INTEGER filesize_li;
                if (GetFileSizeEx((HANDLE)req->file_source->handle, &filesize_li)) {
                    req->file_size = static_cast<uint64_t>(filesize_li.QuadPart);
                }
#else
                struct stat st;
                if (fstat(req->file_source->fd, &st) == 0) {
                    req->file_size = static_cast<uint64_t>(st.st_size);
                }
#endif
            }
        }
    }

    req->request_headers = headers;
    req->evaluator = evaluator;

    req->response_headers = std::make_shared<HeaderMap>();
    req->status_text = std::make_shared<std::string>();

    req->handlers = std::make_shared<StreamEventHandlers>();
    req->handlers->env = callEnv;
    req->handlers->evaluator = evaluator;

    // Initialize llhttp
    llhttp_settings_init(&req->settings);
    req->settings.on_status = on_status;
    req->settings.on_header_field = on_header_field;
    req->settings.on_header_value = on_header_value;
    req->settings.on_headers_complete = on_headers_complete;
    req->settings.on_body = on_body;
    req->settings.on_message_complete = on_message_complete;

    llhttp_init(&req->parser, HTTP_RESPONSE, &req->settings);
    req->parser.data = req;

    if (req->has_fixed_body) {
        uint64_t len = req->file_source ? req->file_size : req->body_buffer.size();
        req->request_headers.set("Content-Length", std::to_string(len));  // ✅
        req->upload_size = len;
    } else if (req->method != "GET" && req->method != "HEAD") {
        // ✅ Use has() instead of find()
        if (!req->request_headers.has("Content-Length")) {
            req->request_headers.set("Transfer-Encoding", "chunked");  // ✅
            req->is_chunked = true;
        }
    }

#ifdef HAVE_OPENSSL
    if (use_ssl) {
        if (!setup_ssl(req)) {
            delete req;
            throw SwaziError("SSLError", "Failed to initialize SSL", token.loc);
        }
    }
#endif

    // Build RequestStream object
    auto stream_obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<http>", 0, 0, 0);

    // on(event, callback)
    auto on_impl = [req](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2 || !std::holds_alternative<std::string>(args[0]) ||
            !std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "on(event, callback) requires event name and function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr callback = std::get<FunctionPtr>(args[1]);

        std::lock_guard<std::mutex> lock(req->handlers->mutex);
        if (event == "response")
            req->handlers->on_response = callback;
        else if (event == "data")
            req->handlers->on_data = callback;
        else if (event == "end")
            req->handlers->on_end = callback;
        else if (event == "error")
            req->handlers->on_error = callback;
        else if (event == "progress")
            req->handlers->on_progress = callback;
        else if (event == "connect")
            req->handlers->on_connect = callback;
        else if (event == "close")
            req->handlers->on_close = callback;
        else if (event == "drain")
            req->handlers->on_drain = callback;
        else if (event == "uploadProgress")
            req->handlers->on_upload_progress = callback;

        return std::monostate{};
    };
    stream_obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("request.on", on_impl, nullptr, tok)},
        false, false, false, tok};

    // write(data, [callback])
    auto write_impl = [req](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "write() requires data argument", token.loc);
        }

        if (!req->headers_sent.load()) {
            throw SwaziError("Error", "Cannot write before connection established", token.loc);
        }

        FunctionPtr callback = nullptr;
        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        std::vector<uint8_t> data;

        if (std::holds_alternative<BufferPtr>(args[0])) {
            BufferPtr buf = std::get<BufferPtr>(args[0]);
            data = buf->data;
        } else if (std::holds_alternative<std::string>(args[0])) {
            std::string str = std::get<std::string>(args[0]);
            data.assign(str.begin(), str.end());
        } else {
            std::string str = value_to_string_simple(args[0]);
            data.assign(str.begin(), str.end());
        }

        if (!data.empty()) {
            if (req->is_chunked) {
                std::ostringstream chunk_header;
                chunk_header << std::hex << data.size() << "\r\n";
                std::string header_str = chunk_header.str();

                std::vector<uint8_t> chunked_packet;
                chunked_packet.reserve(header_str.size() + data.size() + 2);

                chunked_packet.insert(chunked_packet.end(), header_str.begin(), header_str.end());
                chunked_packet.insert(chunked_packet.end(), data.begin(), data.end());
                chunked_packet.push_back('\r');
                chunked_packet.push_back('\n');

                queue_write(req, chunked_packet, callback, data.size());  // data is the payload length
            } else {
                queue_write(req, data, callback, data.size());
            }
        }

        return Value{true};
    };
    stream_obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("request.write", write_impl, nullptr, tok)},
        false, false, false, tok};

    // end([data], [callback])
    auto end_impl = [req](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (req->request_complete.load()) {
            return std::monostate{};
        }

        FunctionPtr callback = nullptr;
        std::vector<uint8_t> final_data;
        bool has_data = false;

        if (!args.empty() && !std::holds_alternative<std::monostate>(args[0])) {
            if (std::holds_alternative<FunctionPtr>(args[0])) {
                callback = std::get<FunctionPtr>(args[0]);
            } else {
                has_data = true;
                if (std::holds_alternative<BufferPtr>(args[0])) {
                    final_data = std::get<BufferPtr>(args[0])->data;
                } else if (std::holds_alternative<std::string>(args[0])) {
                    std::string str = std::get<std::string>(args[0]);
                    final_data.assign(str.begin(), str.end());
                } else {
                    std::string str = value_to_string_simple(args[0]);
                    final_data.assign(str.begin(), str.end());
                }
            }
        }

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        if (has_data && !final_data.empty()) {
            if (req->has_fixed_body) {
                throw SwaziError("Error", "Cannot call end(data) when 'body' option is used", token.loc);
            }

            if (req->is_chunked) {
                std::ostringstream ss;
                ss << std::hex << final_data.size() << "\r\n";
                std::string head = ss.str();

                std::vector<uint8_t> chunked;
                chunked.reserve(head.size() + final_data.size() + 2);
                chunked.insert(chunked.end(), head.begin(), head.end());
                chunked.insert(chunked.end(), final_data.begin(), final_data.end());
                chunked.push_back('\r');
                chunked.push_back('\n');

                queue_write(req, chunked, nullptr, final_data.size());
            } else {
                queue_write(req, final_data, nullptr, final_data.size());
            }
        }

        if (req->is_chunked) {
            std::string term = "0\r\n\r\n";
            std::vector<uint8_t> term_data(term.begin(), term.end());
            queue_write(req, term_data, nullptr, 0);  // not body
        }

        req->request_complete.store(true);

        if (callback) {
            emit_event(req->handlers, "drain");
        }

        return std::monostate{};
    };
    stream_obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("request.end", end_impl, nullptr, tok)},
        false, false, false, tok};

    // pause()
    auto pause_impl = [req](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        scheduler_run_on_loop([req]() {
            if (!req->paused.exchange(true)) {
                uv_read_stop((uv_stream_t*)&req->socket);
            }
        });
        return std::monostate{};
    };
    stream_obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("request.pause", pause_impl, nullptr, tok)},
        false, false, false, tok};

    // resume()
    auto resume_impl = [req](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        scheduler_run_on_loop([req]() {
            if (req->paused.exchange(false)) {
                uv_read_start((uv_stream_t*)&req->socket, alloc_buffer, on_read);
            }
        });
        return std::monostate{};
    };
    stream_obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("request.resume", resume_impl, nullptr, tok)},
        false, false, false, tok};

    // abort([reason])
    auto abort_impl = [req](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        std::string reason = "aborted";
        if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
            reason = std::get<std::string>(args[0]);
        }

        auto handlers = req->handlers;

        // Emit error before closing
        emit_event(handlers, "error", Value{reason});

        // Close will emit "close" event
        close_connection(req, true);

        return std::monostate{};
    };
    stream_obj->properties["abort"] = {
        Value{std::make_shared<FunctionValue>("request.abort", abort_impl, nullptr, tok)},
        false, false, false, tok};

    // setHeader(name, value)
    auto setHeader_impl = [req](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "setHeader(name, value) requires both arguments", token.loc);
        }

        if (req->headers_sent.load()) {
            throw SwaziError("Error", "Cannot set headers after they have been sent", token.loc);
        }

        std::string name = value_to_string_simple(args[0]);
        std::string value = value_to_string_simple(args[1]);

        req->request_headers.set(name, value);  // ✅ Use HeaderMap

        return std::monostate{};
    };
    stream_obj->properties["setHeader"] = {
        Value{std::make_shared<FunctionValue>("request.setHeader", setHeader_impl, nullptr, tok)},
        false, false, false, tok};

    // getHeader(name)
    auto getHeader_impl = [req](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "getHeader(name) requires name argument", token.loc);
        }

        std::string name = value_to_string_simple(args[0]);
        auto val = req->request_headers.get(name);  // ✅ Use HeaderMap
        if (!val.has_value()) return Value{std::monostate{}};

        return Value{val.value()};
    };
    stream_obj->properties["getHeader"] = {
        Value{std::make_shared<FunctionValue>("request.getHeader", getHeader_impl, nullptr, tok)},
        false, false, false, tok};

    // removeHeader(name)
    auto removeHeader_impl = [req](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "removeHeader(name) requires name argument", token.loc);
        }

        if (req->headers_sent.load()) {
            throw SwaziError("Error", "Cannot remove headers after they have been sent", token.loc);
        }

        std::string name = value_to_string_simple(args[0]);
        req->request_headers.remove(name);  // ✅ Use HeaderMap

        return std::monostate{};
    };
    stream_obj->properties["removeHeader"] = {
        Value{std::make_shared<FunctionValue>("request.removeHeader", removeHeader_impl, nullptr, tok)},
        false, false, false, tok};

    // Properties
    stream_obj->properties["url"] = {Value{url}, false, false, true, tok};
    stream_obj->properties["method"] = {Value{method}, false, false, true, tok};
    stream_obj->properties["host"] = {Value{host}, false, false, true, tok};
    stream_obj->properties["port"] = {Value{static_cast<double>(port)}, false, false, true, tok};
    stream_obj->properties["path"] = {Value{path}, false, false, true, tok};
    stream_obj->properties["protocol"] = {Value{protocol}, false, false, true, tok};

    // Start connection on loop thread
    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) {
        delete req;
        throw SwaziError("RuntimeError", "No event loop available", token.loc);
    }

    g_active_http_requests.fetch_add(1);

    scheduler_run_on_loop([req, loop, host, port]() {
        uv_tcp_init(loop, &req->socket);
        req->socket.data = req;
        req->connect_req.data = req;

        uv_getaddrinfo_t* addrinfo_req = new uv_getaddrinfo_t;
        addrinfo_req->data = req;

        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        int r = uv_getaddrinfo(
            loop,
            addrinfo_req,
            [](uv_getaddrinfo_t* addrinfo_req, int status, struct addrinfo* res) {
                HttpClientRequest* req = static_cast<HttpClientRequest*>(addrinfo_req->data);

                if (status != 0 || !res) {
                    std::string error = std::string("DNS lookup failed: ") + uv_strerror(status);
                    emit_event(req->handlers, "error", Value{error});

                    if (res) uv_freeaddrinfo(res);
                    delete addrinfo_req;

                    close_connection(req, true);
                    return;
                }

                struct sockaddr_in addr;
                memcpy(&addr, res->ai_addr, sizeof(addr));
                addr.sin_port = htons(req->port);

                uv_tcp_connect(&req->connect_req, &req->socket,
                    (const struct sockaddr*)&addr, on_connect);

                uv_freeaddrinfo(res);
                delete addrinfo_req;
            },
            host.c_str(),
            nullptr,
            &hints);

        if (r != 0) {
            // uv_getaddrinfo couldn't be started; centralize cleanup
            std::string error = std::string("Failed to start DNS lookup: ") + uv_strerror(r);
            emit_event(req->handlers, "error", Value{error});

            // Use centralized close (will decrement active count and delete on loop)
            close_connection(req, true);
            delete addrinfo_req;
        }
    });

    return Value{stream_obj};
}

// ============================================================================
// CONVENIENCE WRAPPERS
// ============================================================================

// http.get(url, options?) -> RequestStream
Value native_http_get(const std::vector<Value>& args, EnvPtr env, const Token& token, Evaluator* evaluator) {
    std::vector<Value> modified_args = args;

    if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
        // Options already provided
    } else {
        // Create options object with GET method
        auto opts = std::make_shared<ObjectValue>();
        opts->properties["method"] = PropertyDescriptor{Value{std::string("GET")}, false, false, false, Token{}};
        modified_args.push_back(Value{opts});
    }

    return native_http_open(modified_args, env, token, evaluator);
}

// http.post(url, data, options?) -> RequestStream
Value native_http_post(const std::vector<Value>& args, EnvPtr env, const Token& token, Evaluator* evaluator) {
    if (args.size() < 2) {
        throw SwaziError("TypeError", "http.post requires url and data", token.loc);
    }

    auto opts = std::make_shared<ObjectValue>();
    opts->properties["method"] = PropertyDescriptor{Value{std::string("POST")}, false, false, false, Token{}};

    // Merge user options if provided
    if (args.size() >= 3 && std::holds_alternative<ObjectPtr>(args[2])) {
        ObjectPtr user_opts = std::get<ObjectPtr>(args[2]);
        for (const auto& kv : user_opts->properties) {
            opts->properties[kv.first] = kv.second;
        }
    }

    // Create request
    std::vector<Value> req_args = {args[0], Value{opts}};
    Value req_stream = native_http_open(req_args, env, token, evaluator);

    if (!std::holds_alternative<ObjectPtr>(req_stream)) {
        return req_stream;
    }

    ObjectPtr stream_obj = std::get<ObjectPtr>(req_stream);

    // Auto-send data when connected
    auto on_prop = stream_obj->properties.find("on");
    auto write_prop = stream_obj->properties.find("write");
    auto end_prop = stream_obj->properties.find("end");

    if (on_prop != stream_obj->properties.end() &&
        write_prop != stream_obj->properties.end() &&
        end_prop != stream_obj->properties.end() &&
        std::holds_alternative<FunctionPtr>(on_prop->second.value) &&
        std::holds_alternative<FunctionPtr>(write_prop->second.value) &&
        std::holds_alternative<FunctionPtr>(end_prop->second.value)) {
        FunctionPtr on_fn = std::get<FunctionPtr>(on_prop->second.value);
        FunctionPtr write_fn = std::get<FunctionPtr>(write_prop->second.value);
        FunctionPtr end_fn = std::get<FunctionPtr>(end_prop->second.value);

        // Create connect handler
        auto connect_handler = [write_fn, end_fn, data = args[1]](
                                   const std::vector<Value>&, EnvPtr env, const Token& token) -> Value {
            // Write data
            if (write_fn->is_native && write_fn->native_impl) {
                write_fn->native_impl({data}, env, token);
            }
            // End request
            if (end_fn->is_native && end_fn->native_impl) {
                end_fn->native_impl({}, env, token);
            }
            return std::monostate{};
        };

        auto handler_fn = std::make_shared<FunctionValue>(
            "post_connect_handler", connect_handler, nullptr, Token{});

        // Register connect handler
        if (on_fn->is_native && on_fn->native_impl) {
            on_fn->native_impl({Value{std::string("connect")}, Value{handler_fn}}, env, token);
        }
    }

    return req_stream;
}

// ============================================================================
// EXPORTS REGISTRATION
// ============================================================================

void native_http_exetended(const ObjectPtr& http_module, Evaluator* evaluator, EnvPtr env) {
    Token tok{};
    tok.loc = TokenLocation("<http>", 0, 0, 0);

    // http.open(url, options?)
    auto open_fn = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_http_open(args, env, token, evaluator);
    };
    http_module->properties["open"] = {
        Value{std::make_shared<FunctionValue>("http.open", open_fn, env, tok)},
        false, false, false, tok};

    // http.get(url, options?)
    auto get_fn = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_http_get(args, env, token, evaluator);
    };
    http_module->properties["get"] = {
        Value{std::make_shared<FunctionValue>("http.get", get_fn, env, tok)},
        false, false, false, tok};

    // http.post(url, data, options?)
    auto post_fn = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_http_post(args, env, token, evaluator);
    };
    http_module->properties["post"] = {
        Value{std::make_shared<FunctionValue>("http.post", post_fn, env, tok)},
        false, false, false, tok};
}
