// http_server.cc
#include <llhttp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <uv.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

// Forward declarations to allow proper close callback implementations
struct HttpRequestState;
static void close_client_and_state(uv_handle_t* h);

// ============================================================================
// CASE-INSENSITIVE HEADER MAP
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

// Wrapper class for headers that preserves original case
class HeaderMap {
   private:
    std::map<std::string, std::string, CaseInsensitiveCompare> headers_;
    std::map<std::string, std::string, CaseInsensitiveCompare> original_case_;

   public:
    void set(const std::string& name, const std::string& value) {
        headers_[name] = value;
        // Preserve the FIRST casing used (Node.js behavior)
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

    // Iterator support for sending headers with original casing
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

    // Get all headers as object (for JavaScript)
    std::map<std::string, std::string> to_map() const {
        std::map<std::string, std::string> result;
        for (auto it = begin(); it != end(); ++it) {
            auto kv = *it;
            result[kv.first] = kv.second;
        }
        return result;
    }
};

static void alloc_buffer(uv_handle_t*, size_t suggested_size, uv_buf_t* buf);
static void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf);

// ============================================================================
// HELPER: String conversions
// ============================================================================

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

// ============================================================================
// HTTP RESPONSE
// ============================================================================

struct HttpResponse : public std::enable_shared_from_this<HttpResponse> {
    int status_code = 200;
    std::string reason;
    HeaderMap headers;
    uv_stream_t* client = nullptr;
    bool chunked_mode = false;
    bool finished = false;

    bool sendfile_active = false;
    FunctionPtr sendfile_callback = nullptr;

    std::atomic<int> pending_writes{0};
    static const int MAX_PENDING_WRITES = 16;  // Configurable limit

    EnvPtr env = nullptr;
    Evaluator* evaluator = nullptr;
    std::vector<FunctionPtr> drain_listeners;

    // NEW: queued writes when we've reached the MAX_PENDING_WRITES
    std::deque<std::vector<uint8_t>> write_queue;
    bool write_queue_backpressure = false;

    // Request-close semantics: when a parse error or EOF occurs we'll request a close
    // and only perform the TCP shutdown/close once outstanding writes & queue are drained.
    std::atomic<bool> close_requested{false};

    static std::string reason_for_code(int code) {
        switch (code) {
            case 200:
                return "OK";
            case 201:
                return "Created";
            case 204:
                return "No Content";
            case 301:
                return "Moved Permanently";
            case 302:
                return "Found";
            case 304:
                return "Not Modified";
            case 400:
                return "Bad Request";
            case 401:
                return "Unauthorized";
            case 403:
                return "Forbidden";
            case 404:
                return "Not Found";
            case 500:
                return "Internal Server Error";
            default:
                return "";
        }
    }

    void emit_drain() {
        if (drain_listeners.empty()) return;
        for (auto& cb : drain_listeners) {
            if (!cb) continue;
            FunctionPtr callback = cb;
            scheduler_run_on_loop([callback]() {
                try {
                    CallbackPayload* payload = new CallbackPayload(callback, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                } catch (...) {}
            });
        }
    }

    // Process queued writes when capacity becomes available.
    void process_queued_writes() {
        while (!write_queue.empty() && pending_writes.load() < MAX_PENDING_WRITES && client && !finished) {
            auto data = std::move(write_queue.front());
            write_queue.pop_front();

            if (!headers_flushed) {
                if (!headers.has("Content-Length")) {
                    chunked_mode = true;
                    headers.set("Transfer-Encoding", "chunked");
                }
                flush_headers();
            }

            if (data.empty()) {
                continue;
            }

            if (!chunked_mode) {
                // Send raw data without chunk framing
                char* buf = static_cast<char*>(malloc(data.size()));
                memcpy(buf, data.data(), data.size());
                uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(data.size()));
                uv_write_t* req = new uv_write_t;

                struct WriteContext {
                    char* buffer;
                    std::shared_ptr<HttpResponse> response;
                };

                std::shared_ptr<HttpResponse> self = shared_from_this();
                auto* ctx = new WriteContext{buf, self};
                req->data = ctx;

                pending_writes.fetch_add(1);

                uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
                    auto* ctx = static_cast<WriteContext*>(req->data);
                    std::shared_ptr<HttpResponse> resp = ctx->response;

                    resp->pending_writes.fetch_sub(1);

                    if (ctx->buffer) free(ctx->buffer);
                    delete ctx;
                    delete req;

                    resp->process_queued_writes();

                    if (resp->write_queue_backpressure && resp->write_queue.empty() && resp->pending_writes.load() < HttpResponse::MAX_PENDING_WRITES) {
                        resp->write_queue_backpressure = false;
                        resp->emit_drain();
                    }

                    if (resp->close_requested.load() && resp->write_queue.empty() && resp->pending_writes.load() == 0) {
                        resp->perform_close();
                    }
                });
            } else {
                // Chunked mode: apply chunk framing
                std::ostringstream chunk_header;
                chunk_header << std::hex << data.size() << "\r\n";
                std::string hdr = chunk_header.str();

                size_t total_size = hdr.size() + data.size() + 2;
                char* buf = static_cast<char*>(malloc(total_size));

                memcpy(buf, hdr.data(), hdr.size());
                memcpy(buf + hdr.size(), data.data(), data.size());
                buf[total_size - 2] = '\r';
                buf[total_size - 1] = '\n';

                uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(total_size));
                uv_write_t* req = new uv_write_t;

                struct WriteContext {
                    char* buffer;
                    std::shared_ptr<HttpResponse> response;
                };

                std::shared_ptr<HttpResponse> self = shared_from_this();
                auto* ctx = new WriteContext{buf, self};
                req->data = ctx;

                pending_writes.fetch_add(1);

                uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
                    auto* ctx = static_cast<WriteContext*>(req->data);
                    std::shared_ptr<HttpResponse> resp = ctx->response;

                    resp->pending_writes.fetch_sub(1);

                    if (ctx->buffer) free(ctx->buffer);
                    delete ctx;
                    delete req;

                    resp->process_queued_writes();

                    if (resp->write_queue_backpressure && resp->write_queue.empty() && resp->pending_writes.load() < HttpResponse::MAX_PENDING_WRITES) {
                        resp->write_queue_backpressure = false;
                        resp->emit_drain();
                    }

                    if (resp->finished && resp->write_queue.empty() && resp->pending_writes.load() == 0) {
                        if (resp->chunked_mode) {
                            const char* terminator = "0\r\n\r\n";
                            char* term_buf = static_cast<char*>(malloc(5));
                            memcpy(term_buf, terminator, 5);
                            uv_buf_t term_uvbuf = uv_buf_init(term_buf, 5);

                            uv_write_t* term_req = new uv_write_t;
                            term_req->data = term_buf;

                            uv_write(term_req, resp->client, &term_uvbuf, 1, [](uv_write_t* req, int) {
                                if (req->data) free(req->data);
                                delete req;
                            });
                        }
                    }

                    if (resp->close_requested.load() && resp->write_queue.empty() && resp->pending_writes.load() == 0) {
                        resp->perform_close();
                    }
                });
            }
        }

        if (write_queue_backpressure && write_queue.empty() && pending_writes.load() < MAX_PENDING_WRITES) {
            write_queue_backpressure = false;
            emit_drain();
        }
    }
    bool write_chunk(const std::vector<uint8_t>& data) {
        if (!client || finished) return false;

        if (sendfile_active) return false;

        if (pending_writes.load() >= MAX_PENDING_WRITES) {
            write_queue.emplace_back(data);
            write_queue_backpressure = true;
            return false;
        }

        if (!headers_flushed) {
            if (!headers.has("Content-Length")) {
                chunked_mode = true;
                headers.set("Transfer-Encoding", "chunked");
            }
            flush_headers();
        }

        if (data.empty()) return true;

        if (!chunked_mode) {
            // Send raw data without chunk framing
            char* buf = static_cast<char*>(malloc(data.size()));
            memcpy(buf, data.data(), data.size());

            uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(data.size()));
            uv_write_t* req = new uv_write_t;

            struct WriteContext {
                char* buffer;
                std::shared_ptr<HttpResponse> response;
            };

            std::shared_ptr<HttpResponse> self = shared_from_this();
            auto* ctx = new WriteContext{buf, self};
            req->data = ctx;

            pending_writes.fetch_add(1);

            uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
                auto* ctx = static_cast<WriteContext*>(req->data);
                std::shared_ptr<HttpResponse> resp = ctx->response;

                resp->pending_writes.fetch_sub(1);

                if (ctx->buffer) free(ctx->buffer);
                delete ctx;
                delete req;

                resp->process_queued_writes();

                if (resp->write_queue_backpressure && resp->write_queue.empty() &&
                    resp->pending_writes.load() < HttpResponse::MAX_PENDING_WRITES) {
                    resp->write_queue_backpressure = false;
                    resp->emit_drain();
                }

                if (resp->close_requested.load() && resp->write_queue.empty() && resp->pending_writes.load() == 0) {
                    resp->perform_close();
                }
            });

            return true;
        }

        // Chunked mode: apply chunk framing
        std::ostringstream chunk_header;
        chunk_header << std::hex << data.size() << "\r\n";
        std::string hdr = chunk_header.str();

        size_t total_size = hdr.size() + data.size() + 2;
        char* buf = static_cast<char*>(malloc(total_size));

        memcpy(buf, hdr.data(), hdr.size());
        memcpy(buf + hdr.size(), data.data(), data.size());
        buf[total_size - 2] = '\r';
        buf[total_size - 1] = '\n';

        uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(total_size));
        uv_write_t* req = new uv_write_t;

        struct WriteContext {
            char* buffer;
            std::shared_ptr<HttpResponse> response;
        };

        std::shared_ptr<HttpResponse> self = shared_from_this();
        auto* ctx = new WriteContext{buf, self};
        req->data = ctx;

        pending_writes.fetch_add(1);

        uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
            auto* ctx = static_cast<WriteContext*>(req->data);
            std::shared_ptr<HttpResponse> resp = ctx->response;

            resp->pending_writes.fetch_sub(1);

            if (ctx->buffer) free(ctx->buffer);
            delete ctx;
            delete req;

            resp->process_queued_writes();

            if (resp->write_queue_backpressure && resp->write_queue.empty() &&
                resp->pending_writes.load() < HttpResponse::MAX_PENDING_WRITES) {
                resp->write_queue_backpressure = false;
                resp->emit_drain();
            }

            if (resp->finished && resp->write_queue.empty() && resp->pending_writes.load() == 0) {
                if (resp->chunked_mode) {
                    const char* terminator = "0\r\n\r\n";
                    char* term_buf = static_cast<char*>(malloc(5));
                    memcpy(term_buf, terminator, 5);
                    uv_buf_t term_uvbuf = uv_buf_init(term_buf, 5);

                    uv_write_t* term_req = new uv_write_t;
                    term_req->data = term_buf;

                    uv_write(term_req, resp->client, &term_uvbuf, 1, [](uv_write_t* req, int) {
                        if (req->data) free(req->data);
                        delete req;
                    });
                }
            }

            if (resp->close_requested.load() && resp->write_queue.empty() && resp->pending_writes.load() == 0) {
                resp->perform_close();
            }
        });

        return true;
    }

    void end_response(const std::vector<uint8_t>& final_data = {}) {
        if (finished) return;

        if (sendfile_active) return;

        finished = true;

        headers.set("Connection", "close");

        if (!headers_flushed) {
            if (!final_data.empty()) {
                headers.set("Content-Length", std::to_string(final_data.size()));
                chunked_mode = false;
            } else {
                headers.set("Content-Length", "0");
                chunked_mode = false;
            }
            flush_headers();
        }

        if (chunked_mode) {
            if (!write_queue.empty() || pending_writes.load() > 0) {
                if (!final_data.empty()) {
                    write_queue.emplace_back(final_data);
                }
                write_queue_backpressure = write_queue_backpressure || (pending_writes.load() >= MAX_PENDING_WRITES);
                return;
            }

            if (!final_data.empty()) {
                std::ostringstream chunk_header;
                chunk_header << std::hex << final_data.size() << "\r\n";
                std::string hdr = chunk_header.str();

                size_t total_size = hdr.size() + final_data.size() + 2;
                char* buf = static_cast<char*>(malloc(total_size));

                memcpy(buf, hdr.data(), hdr.size());
                memcpy(buf + hdr.size(), final_data.data(), final_data.size());
                buf[total_size - 2] = '\r';
                buf[total_size - 1] = '\n';

                uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(total_size));
                uv_write_t* req = new uv_write_t;
                req->data = buf;

                uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
                    if (req->data) free(req->data);
                    delete req;
                });
            }

            const char* terminator = "0\r\n\r\n";
            char* term_buf = static_cast<char*>(malloc(5));
            memcpy(term_buf, terminator, 5);
            uv_buf_t term_uvbuf = uv_buf_init(term_buf, 5);

            uv_write_t* term_req = new uv_write_t;
            term_req->data = term_buf;

            uv_write(term_req, client, &term_uvbuf, 1, [](uv_write_t* req, int) {
                if (req->data) free(req->data);
                delete req;
            });
        } else {
            if (!write_queue.empty() || pending_writes.load() > 0) {
                if (!final_data.empty()) {
                    write_queue.emplace_back(final_data);
                }
                return;
            }

            if (!final_data.empty()) {
                char* buf = static_cast<char*>(malloc(final_data.size()));
                memcpy(buf, final_data.data(), final_data.size());
                uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(final_data.size()));

                uv_write_t* req = new uv_write_t;
                req->data = buf;

                uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
                    if (req->data) free(req->data);
                    delete req;
                });
            }
        }

        if (close_requested.load() && write_queue.empty() && pending_writes.load() == 0) {
            perform_close();
        }
    }
    FilePtr file_source = nullptr;
    uint64_t file_bytes_sent = 0;
    uint64_t file_total_size = 0;

    bool headers_flushed = false;

    void flush_headers() {
        if (headers_flushed || !client) return;
        headers_flushed = true;

        std::ostringstream response;
        std::string rp = !reason.empty() ? reason : reason_for_code(status_code);

        response << "HTTP/1.1 " << status_code;
        if (!rp.empty()) response << " " << rp;
        response << "\r\n";

        if (!headers.has("Content-Type")) {
            headers.set("Content-Type", "text/plain");
        }

        if (headers.has("Content-Length")) {
            chunked_mode = false;
        } else {
            if (!headers.has("Transfer-Encoding")) {
                headers.set("Transfer-Encoding", "chunked");
            }
            chunked_mode = true;
        }

        for (auto it = headers.begin(); it != headers.end(); ++it) {
            auto kv = *it;
            response << kv.first << ": " << kv.second << "\r\n";
        }
        response << "\r\n";

        std::string out = response.str();
        char* buf = static_cast<char*>(malloc(out.size()));
        memcpy(buf, out.data(), out.size());
        uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(out.size()));

        uv_write_t* req = new uv_write_t;
        req->data = buf;

        uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
            if (req->data) free(req->data);
            delete req;
        });
    }

    struct FileStreamContext {
        char* buffer;
        std::shared_ptr<HttpResponse> response;
    };

    void stream_file_chunk() {
        if (!file_source || !client || finished) {
            if (sendfile_callback) {
                std::string error = finished ? "" : "Stream interrupted";
                call_sendfile_callback(error);
            }
            return;
        }

        const size_t CHUNK_SIZE = 64 * 1024;
        std::vector<uint8_t> buffer(CHUNK_SIZE);
        size_t bytes_read = 0;

#ifdef _WIN32
        DWORD read_bytes;
        if (ReadFile((HANDLE)file_source->handle, buffer.data(), CHUNK_SIZE, &read_bytes, NULL)) {
            bytes_read = read_bytes;
        }
#else
        ssize_t r = read(file_source->fd, buffer.data(), CHUNK_SIZE);
        if (r > 0) bytes_read = r;
#endif

        if (bytes_read > 0) {
            buffer.resize(bytes_read);
            file_bytes_sent += bytes_read;

            std::ostringstream chunk_header;
            chunk_header << std::hex << bytes_read << "\r\n";
            std::string hdr = chunk_header.str();

            size_t total_size = hdr.size() + bytes_read + 2;
            char* buf = static_cast<char*>(malloc(total_size));

            memcpy(buf, hdr.data(), hdr.size());
            memcpy(buf + hdr.size(), buffer.data(), bytes_read);
            buf[total_size - 2] = '\r';
            buf[total_size - 1] = '\n';

            uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(total_size));
            uv_write_t* req = new uv_write_t;

            std::shared_ptr<HttpResponse> self = shared_from_this();
            auto* ctx = new FileStreamContext{buf, self};
            req->data = ctx;

            uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int status) {
                auto* ctx = static_cast<FileStreamContext*>(req->data);
                std::shared_ptr<HttpResponse> response = ctx->response;

                if (ctx->buffer) free(ctx->buffer);
                delete ctx;
                delete req;

                if (status != 0) {
                    response->call_sendfile_callback("Write error");
                    response->finish_sendfile();
                    return;
                }

                // Continue streaming or finish
                if (!response->finished &&
                    response->file_bytes_sent < response->file_total_size) {
                    response->stream_file_chunk();
                } else {
                    // File completely sent - success!
                    response->call_sendfile_callback("");  // Empty string = no error
                    response->finish_sendfile();
                }
            });
        } else {
            // Read error
            call_sendfile_callback("File read error");
            finish_sendfile();
        }
    }

    void call_sendfile_callback(const std::string& error) {
        if (!sendfile_callback) return;

        FunctionPtr cb = sendfile_callback;
        sendfile_callback = nullptr;

        scheduler_run_on_loop([cb, error]() {
            try {
                Value err_val = error.empty() ? Value{std::monostate{}} : Value{error};
                CallbackPayload* payload = new CallbackPayload(cb, {err_val});
                enqueue_callback_global(static_cast<void*>(payload));
            } catch (...) {}
        });
    }

    void finish_sendfile() {
        sendfile_active = false;
        file_source = nullptr;

        finished = true;

        // If we have outstanding writes/queued writes, defer terminator to write completion path
        if (!write_queue.empty() || pending_writes.load() > 0) {
            return;
        }

        // Send chunked terminator
        const char* terminator = "0\r\n\r\n";
        char* term_buf = static_cast<char*>(malloc(5));
        memcpy(term_buf, terminator, 5);
        uv_buf_t term_uvbuf = uv_buf_init(term_buf, 5);

        uv_write_t* term_req = new uv_write_t;
        term_req->data = term_buf;

        uv_write(term_req, client, &term_uvbuf, 1, [](uv_write_t* req, int) {
            if (req->data) free(req->data);
            delete req;
        });

        // If a close was requested, perform it now
        if (close_requested.load()) {
            perform_close();
        }
    }

    void send_file(FilePtr file, FunctionPtr callback = nullptr) {
        if (!file || !file->is_open) {
            if (callback) {
                scheduler_run_on_loop([callback]() {
                    try {
                        CallbackPayload* payload = new CallbackPayload(
                            callback,
                            {Value{std::string("File not open")}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    } catch (...) {}
                });
            }
            return;
        }

        sendfile_active = true;
        sendfile_callback = callback;
        file_source = file;

        // Get file size
#ifdef _WIN32
        LARGE_INTEGER filesize_li;
        if (GetFileSizeEx((HANDLE)file->handle, &filesize_li)) {
            file_total_size = static_cast<uint64_t>(filesize_li.QuadPart);
        } else {
            call_sendfile_callback("Cannot get file size");
            finish_sendfile();
            return;
        }
#else
        struct stat st;
        if (fstat(file->fd, &st) == 0) {
            file_total_size = static_cast<uint64_t>(st.st_size);
        } else {
            call_sendfile_callback("Cannot get file size");
            finish_sendfile();
            return;
        }
#endif

        file_bytes_sent = 0;

        // Start streaming
        chunked_mode = true;
        if (!headers_flushed) {
            headers.set("Transfer-Encoding", "chunked");
            flush_headers();
        }
        stream_file_chunk();
    }

    // Request the response / connection to be closed when safe.
    // This marks close_requested and attempts to perform close immediately if drained.
    void request_close() {
        close_requested.store(true);
        if (pending_writes.load() == 0 && write_queue.empty()) {
            perform_close();
        }
    }

    // Perform shutdown -> close now. This schedules uv_shutdown and in its callback
    // arranges for uv_close which will invoke close_client_and_state to delete state & handle.
    void perform_close() {
        if (!client) return;
        if (uv_is_closing((uv_handle_t*)client)) return;

        // Allocate a shutdown request; it will be deleted in the shutdown callback.
        uv_shutdown_t* sreq = new uv_shutdown_t;
        // sreq->data isn't used here.
        uv_shutdown(sreq, client, [](uv_shutdown_t* req, int /*status*/) {
            uv_stream_t* stream = req->handle;
            // Close the handle; close_client_and_state will delete the request state (if present)
            uv_close((uv_handle_t*)stream, [](uv_handle_t* h) {
                close_client_and_state(h);
            });
            delete req;
        });
    }
};

// ============================================================================
// HTTP REQUEST STATE
// ============================================================================

struct BodyChunk {
    std::vector<uint8_t> data;
};

struct HttpRequestState {
    llhttp_t parser;
    llhttp_settings_t settings;

    std::string method;
    std::string url;
    std::string path;
    std::string query;
    HeaderMap headers;
    std::string current_header_field;

    bool headers_complete = false;
    bool message_complete = false;
    bool handler_called = false;
    bool reading_paused = false;

    std::deque<BodyChunk> buffered_chunks;
    bool draining_buffer = false;

    std::vector<FunctionPtr> data_listeners;
    std::vector<FunctionPtr> end_listeners;
    std::vector<FunctionPtr> error_listeners;

    uv_stream_t* client;
    std::shared_ptr<HttpResponse> response;
    FunctionPtr request_handler;
    EnvPtr env;
    Evaluator* evaluator;

    ObjectPtr req_stream_obj;
    ObjectPtr res_obj;

    size_t max_buffer_size = 16 * 1024 * 1024;  // 16MB max buffer
    size_t current_buffer_size = 0;
    bool backpressure_active = false;

    bool closing = false;  // whether close was requested for this state

    void check_backpressure() {
        if (!backpressure_active && current_buffer_size > max_buffer_size) {
            // Stop reading from socket
            if (client && !reading_paused) {
                uv_read_stop(client);
                reading_paused = true;
                backpressure_active = true;
            }
        }
    }

    void release_backpressure() {
        if (backpressure_active && current_buffer_size < max_buffer_size / 2) {
            // Resume reading
            if (client && reading_paused) {
                uv_read_start(client, alloc_buffer, on_read);
                reading_paused = false;
                backpressure_active = false;
            }
        }
    }

    void drain_buffered_chunks() {
        if (draining_buffer) return;
        draining_buffer = true;

        for (const auto& chunk : buffered_chunks) {
            auto buf = std::make_shared<BufferValue>();
            buf->data = chunk.data;
            buf->encoding = "binary";

            for (const auto& listener : data_listeners) {
                if (listener && evaluator) {
                    try {
                        evaluator->invoke_function(listener, {Value{buf}}, env, Token{});
                    } catch (...) {}
                }
            }

            // Update buffer size tracking
            current_buffer_size -= chunk.data.size();
        }

        buffered_chunks.clear();

        // Check if we can resume reading
        release_backpressure();

        if (message_complete) {
            for (const auto& listener : end_listeners) {
                if (listener && evaluator) {
                    try {
                        evaluator->invoke_function(listener, {}, env, Token{});
                    } catch (...) {}
                }
            }
        }
    }
};

// ============================================================================
// LLHTTP CALLBACKS
// ============================================================================

static int on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* state = static_cast<HttpRequestState*>(parser->data);
    state->url.append(at, length);
    return 0;
}

static int on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* state = static_cast<HttpRequestState*>(parser->data);
    state->current_header_field.assign(at, length);
    for (auto& c : state->current_header_field) c = std::tolower(static_cast<unsigned char>(c));
    return 0;
}

static int on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* state = static_cast<HttpRequestState*>(parser->data);
    std::string value(at, length);
    state->headers.set(state->current_header_field, value);  // ✅ Case-insensitive
    return 0;
}
static int on_headers_complete(llhttp_t* parser) {
    auto* state = static_cast<HttpRequestState*>(parser->data);
    state->headers_complete = true;

    // Extract method
    switch (parser->method) {
        case HTTP_GET:
            state->method = "GET";
            break;
        case HTTP_POST:
            state->method = "POST";
            break;
        case HTTP_PUT:
            state->method = "PUT";
            break;
        case HTTP_DELETE:
            state->method = "DELETE";
            break;
        case HTTP_PATCH:
            state->method = "PATCH";
            break;
        case HTTP_HEAD:
            state->method = "HEAD";
            break;
        case HTTP_OPTIONS:
            state->method = "OPTIONS";
            break;
        default:
            state->method = "UNKNOWN";
            break;
    }

    // Parse URL
    size_t qpos = state->url.find('?');
    if (qpos != std::string::npos) {
        state->path = state->url.substr(0, qpos);
        state->query = state->url.substr(qpos + 1);
    } else {
        state->path = state->url;
    }

    // Build request object
    auto req_obj = std::make_shared<ObjectValue>();
    req_obj->properties["method"] = {Value{state->method}, false, false, true, Token{}};
    req_obj->properties["path"] = {Value{state->path}, false, false, true, Token{}};
    req_obj->properties["query"] = {Value{state->query}, false, false, true, Token{}};
    req_obj->properties["url"] = {Value{state->url}, false, false, true, Token{}};

    auto headers_obj = std::make_shared<ObjectValue>();
    for (auto it = state->headers.begin(); it != state->headers.end(); ++it) {
        auto kv = *it;
        headers_obj->properties[kv.first] = {Value{kv.second}, false, false, true, Token{}};
    }
    req_obj->properties["headers"] = {Value{headers_obj}, false, false, true, Token{}};

    // req.on(event, callback)
    auto req_on_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2 || !std::holds_alternative<std::string>(args[0]) ||
            !std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "req.on(event, callback) requires event and function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr callback = std::get<FunctionPtr>(args[1]);

        if (event == "data") {
            state->data_listeners.push_back(callback);
            if (!state->buffered_chunks.empty() && !state->draining_buffer) {
                state->drain_buffered_chunks();
            }
        } else if (event == "end") {
            state->end_listeners.push_back(callback);
            if (state->message_complete && state->buffered_chunks.empty()) {
                try {
                    state->evaluator->invoke_function(callback, {}, state->env, Token{});
                } catch (...) {
                    // Listener error - continue
                }
            }
        } else if (event == "error") {
            state->error_listeners.push_back(callback);
        }

        return std::monostate{};
    };
    req_obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("req.on", req_on_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // req.pause()
    auto req_pause_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!state->reading_paused && state->client) {
            uv_read_stop(state->client);
            state->reading_paused = true;
        }
        return std::monostate{};
    };
    req_obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("req.pause", req_pause_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // req.resume()
    auto req_resume_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (state->reading_paused && state->client) {
            uv_read_start(state->client, alloc_buffer, on_read);
            state->reading_paused = false;
        }
        return std::monostate{};
    };
    req_obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("req.resume", req_resume_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // req.pipe(writable)
    auto pipe_impl = [state](const std::vector<Value>& args, EnvPtr env, const Token& tok) -> Value {
        if (args.empty() || !std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "req.pipe() requires writable stream", tok.loc);
        }

        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);

        // Parse options for end behavior
        bool end_on_finish = true;
        if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[1]);
            auto end_it = opts->properties.find("end");
            if (end_it != opts->properties.end() && std::holds_alternative<bool>(end_it->second.value)) {
                end_on_finish = std::get<bool>(end_it->second.value);
            }
        }

        Token evt_tok{};
        evt_tok.loc = TokenLocation("<req-pipe>", 0, 0, 0);

        // ✅ DATA HANDLER - with backpressure support
        auto data_handler = [dest_obj, state](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
            if (args.empty()) return std::monostate{};

            auto write_it = dest_obj->properties.find("write");
            if (write_it == dest_obj->properties.end()) return std::monostate{};

            if (!std::holds_alternative<FunctionPtr>(write_it->second.value)) return std::monostate{};

            FunctionPtr write_fn = std::get<FunctionPtr>(write_it->second.value);

            if (write_fn->is_native && write_fn->native_impl) {
                try {
                    Value result = write_fn->native_impl({args[0]}, env, token);

                    // ✅ BACKPRESSURE - if write returns false, pause req
                    if (std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                        // Pause reading from socket
                        if (!state->reading_paused && state->client) {
                            uv_read_stop(state->client);
                            state->reading_paused = true;
                        }
                    }

                    return result;
                } catch (...) {
                    return Value{false};
                }
            }

            return Value{false};
        };

        auto data_fn = std::make_shared<FunctionValue>("req-pipe.data", data_handler, nullptr, evt_tok);
        state->data_listeners.push_back(data_fn);

        // ✅ DRAIN HANDLER - resume req when writable drains
        auto drain_handler = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            // Resume reading from socket
            if (state->reading_paused && state->client) {
                uv_read_start(state->client, alloc_buffer, on_read);
                state->reading_paused = false;
            }
            return std::monostate{};
        };

        auto drain_fn = std::make_shared<FunctionValue>("req-pipe.drain", drain_handler, nullptr, evt_tok);

        // Attach drain listener to destination
        auto on_it = dest_obj->properties.find("on");
        if (on_it != dest_obj->properties.end() && std::holds_alternative<FunctionPtr>(on_it->second.value)) {
            FunctionPtr on_fn = std::get<FunctionPtr>(on_it->second.value);
            if (on_fn->is_native && on_fn->native_impl) {
                try {
                    on_fn->native_impl({Value{std::string("drain")}, Value{drain_fn}}, env, evt_tok);
                } catch (...) {}
            }
        }

        // ✅ END HANDLER - call end() on writable when req ends
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

            auto end_fn = std::make_shared<FunctionValue>("req-pipe.end", end_handler, nullptr, evt_tok);
            state->end_listeners.push_back(end_fn);
        }

        // Return destination for chaining
        return Value{dest_obj};
    };
    req_obj->properties["pipe"] = {
        Value{std::make_shared<FunctionValue>("req.pipe", pipe_impl, nullptr, Token{})},
        false, false, false, Token{}};

    state->req_stream_obj = req_obj;

    // Build response object
    auto res_obj = std::make_shared<ObjectValue>();

    static std::atomic<long long> g_next_http_response_id{1000000};  // Start high to avoid collision
    res_obj->properties["_id"] = {
        Value{static_cast<double>(g_next_http_response_id.fetch_add(1))},
        false, false, true, Token{}};

    // res.writeHead(statusCode, headers?)
    auto writeHead_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->response->headers_flushed) {
            throw SwaziError("Error", "Cannot write head after headers sent", token.loc);
        }
        if (args.empty()) {
            throw SwaziError("TypeError", "writeHead requires status code", token.loc);
        }

        int code = static_cast<int>(std::get<double>(args[0]));
        state->response->status_code = code;

        // Optional reason phrase
        if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
            state->response->reason = std::get<std::string>(args[1]);
        } else {
            state->response->reason = HttpResponse::reason_for_code(code);
        }

        // Optional headers object
        size_t headers_idx = (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) ? 2 : 1;
        if (args.size() > headers_idx && std::holds_alternative<ObjectPtr>(args[headers_idx])) {
            ObjectPtr hdrs = std::get<ObjectPtr>(args[headers_idx]);
            for (const auto& kv : hdrs->properties) {
                state->response->headers.set(kv.first, value_to_string_simple_local(kv.second.value));
            }
        }

        return std::monostate{};
    };
    res_obj->properties["writeHead"] = {
        Value{std::make_shared<FunctionValue>("res.writeHead", writeHead_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.setHeader(name, value)
    auto setHeader_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "setHeader requires name and value", token.loc);
        }
        std::string name = value_to_string_simple_local(args[0]);
        std::string value = value_to_string_simple_local(args[1]);
        state->response->headers.set(name, value);  // ✅ Case-insensitive
        return std::monostate{};
    };
    res_obj->properties["setHeader"] = {
        Value{std::make_shared<FunctionValue>("res.setHeader", setHeader_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.removeHeader(name)
    auto removeHeader_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (state->response->headers_flushed) {
            throw SwaziError("Error", "Cannot remove headers after they are sent", token.loc);
        }
        if (args.empty()) {
            throw SwaziError("TypeError", "removeHeader requires name", token.loc);
        }
        std::string name = value_to_string_simple_local(args[0]);
        state->response->headers.remove(name);  // ✅ Case-insensitive
        return std::monostate{};
    };
    res_obj->properties["removeHeader"] = {
        Value{std::make_shared<FunctionValue>("res.removeHeader", removeHeader_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.hasHeader(name)
    auto hasHeader_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "hasHeader requires name", token.loc);
        }
        std::string name = value_to_string_simple_local(args[0]);
        return Value{state->response->headers.has(name)};  // ✅ Case-insensitive
    };
    res_obj->properties["hasHeader"] = {
        Value{std::make_shared<FunctionValue>("res.hasHeader", hasHeader_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.getHeaders()
    auto getHeaders_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        auto headers_obj = std::make_shared<ObjectValue>();
        for (auto it = state->response->headers.begin(); it != state->response->headers.end(); ++it) {
            auto kv = *it;
            headers_obj->properties[kv.first] = {Value{kv.second}, false, false, true, Token{}};
        }
        return Value{headers_obj};
    };
    res_obj->properties["getHeaders"] = {
        Value{std::make_shared<FunctionValue>("res.getHeaders", getHeaders_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.flushHeaders()
    auto flushHeaders_impl = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        state->response->flush_headers();
        return std::monostate{};
    };
    res_obj->properties["flushHeaders"] = {
        Value{std::make_shared<FunctionValue>("res.flushHeaders", flushHeaders_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.headersSent (property)
    auto headersSent_getter = [state](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{state->response->headers_flushed};
    };
    res_obj->properties["headersSent"] = {
        Value{std::make_shared<FunctionValue>("get_headersSent", headersSent_getter, nullptr, Token{})},
        false, true /* second true means a getter */, true, Token{}};

    // res.getHeader(name)
    auto getHeader_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "getHeader requires name", token.loc);
        }
        std::string name = value_to_string_simple_local(args[0]);
        auto val = state->response->headers.get(name);
        if (!val.has_value()) return Value{std::monostate{}};
        return Value{val.value()};
    };
    res_obj->properties["getHeader"] = {
        Value{std::make_shared<FunctionValue>("res.getHeader", getHeader_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.write(chunk)
    auto write_impl = [state](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (args.empty()) return Value{true};

        std::vector<uint8_t> data;
        if (std::holds_alternative<BufferPtr>(args[0])) {
            data = std::get<BufferPtr>(args[0])->data;
        } else if (std::holds_alternative<std::string>(args[0])) {
            std::string str = std::get<std::string>(args[0]);
            data.assign(str.begin(), str.end());
        } else {
            std::string str = value_to_string_simple_local(args[0]);
            data.assign(str.begin(), str.end());
        }

        bool success = state->response->write_chunk(data);
        return Value{success};  // ✅ Return backpressure status
    };
    res_obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("res.write", write_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.end(data?)
    auto end_impl = [state](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (state->response->sendfile_active) {
            return std::monostate{};
        }

        std::vector<uint8_t> data;
        if (!args.empty()) {
            if (std::holds_alternative<BufferPtr>(args[0])) {
                data = std::get<BufferPtr>(args[0])->data;
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string str = std::get<std::string>(args[0]);
                data.assign(str.begin(), str.end());
            } else {
                std::string str = value_to_string_simple_local(args[0]);
                data.assign(str.begin(), str.end());
            }
        }
        state->response->end_response(data);
        return std::monostate{};
    };
    res_obj->properties["end"] = {
        Value{std::make_shared<FunctionValue>("res.end", end_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.on(event, callback)
    auto res_on_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2 || !std::holds_alternative<std::string>(args[0]) ||
            !std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "res.on(event, callback) requires event and function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr callback = std::get<FunctionPtr>(args[1]);

        if (event == "drain") {
            // Store on the response object so write completion can fire it
            state->response->drain_listeners.push_back(callback);
        } else if (event == "finish") {
            // Optional: map to end/finish semantics if you want
        } else if (event == "error") {
            // Optional: store error listeners if desired
        } else {
            // Unknown event: ignore or throw depending on your policy
        }

        return std::monostate{};
    };
    res_obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("res.on", res_on_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.status(code)
    auto status_impl = [state, res_obj](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        if (!args.empty()) {
            state->response->status_code = static_cast<int>(std::get<double>(args[0]));
            state->response->reason = HttpResponse::reason_for_code(state->response->status_code);
        }
        return Value{res_obj};
    };
    res_obj->properties["status"] = {
        Value{std::make_shared<FunctionValue>("res.status", status_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.sendFile(FileValue)
    auto sendFile_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<FilePtr>(args[0])) {
            throw SwaziError("TypeError", "sendFile requires a File object", token.loc);
        }

        FilePtr file = std::get<FilePtr>(args[0]);

        FunctionPtr callback = nullptr;
        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            callback = std::get<FunctionPtr>(args[1]);
        }

        state->response->send_file(file, callback);
        return std::monostate{};
    };
    res_obj->properties["sendFile"] = {
        Value{std::make_shared<FunctionValue>("res.sendFile", sendFile_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.redirect(url, statusCode?)
    auto redirect_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", "redirect requires URL", token.loc);
        }

        std::string location = std::get<std::string>(args[0]);
        int status = 302;

        if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
            status = static_cast<int>(std::get<double>(args[1]));
        }

        if (status != 301 && status != 302 && status != 303 &&
            status != 307 && status != 308) {
            status = 302;
        }

        state->response->status_code = status;
        state->response->headers.set("Location", location);  // ✅ Case-insensitive
        state->response->end_response();

        return std::monostate{};
    };
    res_obj->properties["redirect"] = {
        Value{std::make_shared<FunctionValue>("res.redirect", redirect_impl, nullptr, Token{})},
        false, false, false, Token{}};

    state->res_obj = res_obj;

    // Call handler synchronously
    if (state->request_handler && state->evaluator && !state->handler_called) {
        state->handler_called = true;

        try {
            state->evaluator->invoke_function(
                state->request_handler,
                {Value{state->req_stream_obj}, Value{state->res_obj}},
                state->env,
                Token{});
        } catch (const std::exception& e) {
            if (!state->response->headers_flushed && !state->response->finished) {
                state->response->status_code = 500;
                state->response->headers.set("Content-Type", "text/plain");  // ✅
                std::string error_msg = "Internal Server Error\n";
                std::vector<uint8_t> data(error_msg.begin(), error_msg.end());
                state->response->end_response(data);
            }
            // Handler threw - emit error to error listeners
            for (const auto& listener : state->error_listeners) {
                if (listener) {
                    std::string error = e.what();
                    scheduler_run_on_loop([listener, error]() {
                        try {
                            CallbackPayload* payload = new CallbackPayload(listener, {Value{error}});
                            enqueue_callback_global(static_cast<void*>(payload));
                        } catch (...) {}
                    });
                }
            }
        } catch (...) {
            if (!state->response->headers_flushed && !state->response->finished) {
                state->response->status_code = 500;
                state->response->headers.set("Content-Type", "text/plain");  // ✅
                std::string error_msg = "Internal Server Error\n";
                std::vector<uint8_t> data(error_msg.begin(), error_msg.end());
                state->response->end_response(data);
            }
            // Unknown error
            for (const auto& listener : state->error_listeners) {
                if (listener) {
                    scheduler_run_on_loop([listener]() {
                        try {
                            CallbackPayload* payload = new CallbackPayload(listener, {Value{std::string("Unknown error")}});
                            enqueue_callback_global(static_cast<void*>(payload));
                        } catch (...) {}
                    });
                }
            }
        }
    }

    return 0;
}

static int on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* state = static_cast<HttpRequestState*>(parser->data);

    if (state->data_listeners.empty()) {
        // Buffer but enforce limits
        if (state->current_buffer_size + length > state->max_buffer_size) {
            // Reject request - body too large
            return -1;  // This will trigger parse error
        }

        BodyChunk chunk;
        chunk.data.assign(at, at + length);
        state->current_buffer_size += length;
        state->buffered_chunks.push_back(std::move(chunk));
        state->check_backpressure();
    } else {
        // Stream directly to listeners
        auto buf = std::make_shared<BufferValue>();
        buf->data.assign(at, at + length);
        buf->encoding = "binary";

        for (const auto& listener : state->data_listeners) {
            if (listener && state->evaluator) {
                try {
                    state->evaluator->invoke_function(listener, {Value{buf}}, state->env, Token{});
                } catch (...) {}
            }
        }

        // Listeners processed data, release backpressure if needed
        state->release_backpressure();
    }
    return 0;
}

static int on_message_complete(llhttp_t* parser) {
    auto* state = static_cast<HttpRequestState*>(parser->data);
    state->message_complete = true;

    // Emit end event
    for (const auto& listener : state->end_listeners) {
        if (listener && state->evaluator) {
            try {
                state->evaluator->invoke_function(listener, {}, state->env, Token{});
            } catch (...) {}
        }
    }

    // Connection will close after response is sent (no keep-alive)
    return 0;
}

// ============================================================================
// CONNECTION HANDLING
// ============================================================================

static void on_read(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf) {
    if (nread > 0) {
        auto* state = static_cast<HttpRequestState*>(client->data);
        if (!state) {
            if (buf->base) free(buf->base);
            return;
        }

        llhttp_errno_t err = llhttp_execute(&state->parser, buf->base, nread);

        if (err != HPE_OK) {
            std::string error = std::string("HTTP parse error: ") + llhttp_errno_name(err);

            for (const auto& listener : state->error_listeners) {
                if (listener && state->evaluator) {
                    scheduler_run_on_loop([listener, error]() {
                        try {
                            CallbackPayload* payload = new CallbackPayload(listener, {Value{error}});
                            enqueue_callback_global(static_cast<void*>(payload));
                        } catch (...) {}
                    });
                }
            }

            // Request an orderly close rather than deleting state here.
            if (state->response) {
                state->closing = true;
                state->response->request_close();
            } else {
                // No response object; fallback: just close the handle if not already closing.
                if (!uv_is_closing((uv_handle_t*)client)) {
                    uv_close((uv_handle_t*)client, [](uv_handle_t* h) {
                        // If a state pointer was attached, close_client_and_state would handle it.
                        // Here we simply free the client.
                        delete (uv_tcp_t*)h;
                    });
                }
            }
        }
    } else if (nread < 0) {
        // Client closed connection (normal or error)
        if (buf->base) free(buf->base);

        auto* state = static_cast<HttpRequestState*>(client->data);
        if (state && state->response) {
            state->closing = true;
            state->response->request_close();
        } else {
            // No state/response; just close client handle
            if (!uv_is_closing((uv_handle_t*)client)) {
                uv_close((uv_handle_t*)client, [](uv_handle_t* h) {
                    delete (uv_tcp_t*)h;
                });
            }
        }
        return;  // Return early, don't try to free buf->base again
    }

    if (buf->base) free(buf->base);
}
static void alloc_buffer(uv_handle_t*, size_t suggested_size, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested_size);
    buf->len = static_cast<unsigned int>(suggested_size);
}

// ============================================================================
// SERVER
// ============================================================================

struct ServerInstance {
    uv_tcp_t* server_handle = nullptr;
    FunctionPtr request_handler;
    std::atomic<bool> closed{false};
    EnvPtr env;
    Evaluator* evaluator;
};

static std::mutex g_servers_mutex;
static std::unordered_map<long long, std::shared_ptr<ServerInstance>> g_servers;
static std::atomic<long long> g_next_server_id{1};

static void on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    auto* srv = static_cast<ServerInstance*>(server->data);
    if (!srv || srv->closed.load()) return;

    uv_tcp_t* client = new uv_tcp_t;
    uv_tcp_init(server->loop, client);

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        auto* state = new HttpRequestState();
        state->client = (uv_stream_t*)client;
        state->request_handler = srv->request_handler;
        state->env = srv->env;
        state->evaluator = srv->evaluator;
        state->response = std::make_shared<HttpResponse>();
        state->response->client = (uv_stream_t*)client;
        state->response->env = state->env;
        state->response->evaluator = state->evaluator;

        llhttp_settings_init(&state->settings);
        state->settings.on_url = on_url;
        state->settings.on_header_field = on_header_field;
        state->settings.on_header_value = on_header_value;
        state->settings.on_headers_complete = on_headers_complete;
        state->settings.on_body = on_body;
        state->settings.on_message_complete = on_message_complete;

        llhttp_init(&state->parser, HTTP_REQUEST, &state->settings);
        state->parser.data = state;

        client->data = state;

        uv_read_start((uv_stream_t*)client, alloc_buffer, on_read);
    } else {
        uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
    }
}

// ============================================================================
// EXPORTS
// ============================================================================

Value native_createServer(const std::vector<Value>& args, EnvPtr env, const Token& token, Evaluator* evaluator) {
    if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
        throw SwaziError("TypeError", "createServer requires a request handler function", token.loc);
    }

    auto inst = std::make_shared<ServerInstance>();
    inst->request_handler = std::get<FunctionPtr>(args[0]);
    inst->env = env;
    inst->evaluator = evaluator;

    long long id = g_next_server_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(g_servers_mutex);
        g_servers[id] = inst;
    }

    auto server_obj = std::make_shared<ObjectValue>();

    auto listen_impl = [inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "listen requires port number", token.loc);
        }

        int port = static_cast<int>(std::get<double>(args[0]));
        FunctionPtr cb = (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1]))
            ? std::get<FunctionPtr>(args[1])
            : nullptr;

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            throw SwaziError("RuntimeError", "No event loop available", token.loc);
        }

        scheduler_run_on_loop([inst, port, cb, loop, token]() {
            inst->server_handle = new uv_tcp_t;
            inst->server_handle->data = inst.get();
            uv_tcp_init(loop, inst->server_handle);

            struct sockaddr_in addr;
            uv_ip4_addr("0.0.0.0", port, &addr);

            int s = uv_tcp_bind(inst->server_handle, (const struct sockaddr*)&addr, 0);
            if (s != 0) {
                if (cb) {
                    // pass error as first argument like Node.js: cb(error)
                    CallbackPayload* payload = new CallbackPayload(cb, {Value{std::string(uv_strerror(s))}});
                    enqueue_callback_global(static_cast<void*>(payload));
                } else {
                    std::cerr << ("Server failed to bind port " + std::to_string(port) + ": " + uv_strerror(s)) << "\n";
                }
                return;  // stop, don't try to listen
            }

            int r = uv_listen((uv_stream_t*)inst->server_handle, 128, on_connection);
            if (r != 0) {
                if (cb) {
                    CallbackPayload* payload = new CallbackPayload(cb, {Value{std::string(uv_strerror(r))}});
                    enqueue_callback_global(static_cast<void*>(payload));
                } else {
                    std::cerr << ("Server failed to listen on port " + std::to_string(port) + ": " + uv_strerror(r)) << "\n";
                }
                return;
            }

            // success, call callback with no error
            if (cb) {
                CallbackPayload* payload = new CallbackPayload(cb, {Value{}});  // no error
                enqueue_callback_global(static_cast<void*>(payload));
            }
        });
        return std::monostate{};
    };
    server_obj->properties["listen"] = {
        Value{std::make_shared<FunctionValue>("server.listen", listen_impl, nullptr, Token{})},
        false, false, true, Token{}};

    auto close_impl = [inst, id](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        FunctionPtr cb = (!args.empty() && std::holds_alternative<FunctionPtr>(args[0]))
            ? std::get<FunctionPtr>(args[0])
            : nullptr;

        inst->closed.store(true);

        scheduler_run_on_loop([inst, cb]() {
            if (inst->server_handle) {
                uv_close((uv_handle_t*)inst->server_handle, [](uv_handle_t* h) {
                    delete (uv_tcp_t*)h;
                });
                inst->server_handle = nullptr;
            }
            if (cb) {
                CallbackPayload* payload = new CallbackPayload(cb, {});
                enqueue_callback_global(static_cast<void*>(payload));
            }
        });

        {
            std::lock_guard<std::mutex> lk(g_servers_mutex);
            g_servers.erase(id);
        }
        return std::monostate{};
    };
    server_obj->properties["close"] = {
        Value{std::make_shared<FunctionValue>("server.close", close_impl, nullptr, Token{})},
        false, false, true, Token{}};

    return Value{server_obj};
}

// ============================================================================
// Close helper implemented after types are defined
// ============================================================================

static void close_client_and_state(uv_handle_t* h) {
    if (!h) return;
    // Try to delete associated HttpRequestState if present in handle->data.
    auto* state = static_cast<HttpRequestState*>(h->data);
    if (state) {
        // Clear data so we don't double-delete later.
        h->data = nullptr;
        delete state;
    }

    // Finally free the uv handle memory (we always allocated as uv_tcp_t)
    delete (uv_tcp_t*)h;
}