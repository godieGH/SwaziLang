// http_server.cc

#include <llhttp.h>
#include <uv.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

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

struct HttpResponse {
    int status_code = 200;
    std::string reason;
    std::map<std::string, std::string> headers;
    bool headers_sent = false;
    uv_stream_t* client = nullptr;
    bool chunked_mode = false;
    bool finished = false;

    std::map<std::string, std::string>* request_headers = nullptr;

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

    void send_headers() {
        if (headers_sent || !client) return;

        std::ostringstream response;
        std::string rp = !reason.empty() ? reason : reason_for_code(status_code);

        response << "HTTP/1.1 " << status_code;
        if (!rp.empty()) response << " " << rp;
        response << "\r\n";

        if (headers.find("Content-Type") == headers.end() &&
            headers.find("content-type") == headers.end()) {
            headers["Content-Type"] = "text/plain";
        }

        for (const auto& kv : headers) {
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

        headers_sent = true;
    }

    void write_chunk(const std::vector<uint8_t>& data) {
        if (!client || finished) return;

        if (!headers_sent) {
            chunked_mode = true;
            headers["Transfer-Encoding"] = "chunked";
            send_headers();
        }

        if (data.empty()) return;

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
        req->data = buf;

        uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
            if (req->data) free(req->data);
            delete req;
        });
    }

    void end_response(const std::vector<uint8_t>& final_data = {}) {
        if (finished) return;
        finished = true;

        headers["Connection"] = "close";

        if (chunked_mode) {
            if (!headers_sent) {
                headers["Transfer-Encoding"] = "chunked";
                send_headers();  // Now headers are set correctly
            }
            // Send final data chunk if provided
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

            // Send terminator
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
            if (!headers_sent) {
                headers["Content-Length"] = std::to_string(final_data.size());
                send_headers();  // Headers set correctly above
            }

            if (!final_data.empty()) {
                char* buf = static_cast<char*>(malloc(final_data.size()));
                memcpy(buf, final_data.data(), final_data.size());
                uv_buf_t uvbuf = uv_buf_init(buf, static_cast<unsigned int>(final_data.size()));

                uv_write_t* req = new uv_write_t;
                req->data = buf;

                // ✅ Don't close immediately after write
                uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int) {
                    if (req->data) free(req->data);
                    delete req;
                    // Don't close here either
                });
            }
            // If no final data and headers already sent, we can close gracefully
            // But it's safer to let the client close after receiving the response
        }
    }

    FilePtr file_source = nullptr;
    uint64_t file_bytes_sent = 0;
    uint64_t file_total_size = 0;
    bool streaming_file = false;

    void stream_file_chunk() {
        if (!file_source || !client || finished) return;

        const size_t CHUNK_SIZE = 64 * 1024;  // 64KB
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

            // Send chunk
            write_chunk(buffer);

            // Continue streaming
            if (file_bytes_sent < file_total_size) {
                // Schedule next chunk
                uv_idle_t* idle = new uv_idle_t;
                idle->data = this;
                uv_idle_init(uv_default_loop(), idle);
                uv_idle_start(idle, [](uv_idle_t* handle) {
                    auto* response = static_cast<HttpResponse*>(handle->data);
                    uv_idle_stop(handle);
                    uv_close((uv_handle_t*)handle, [](uv_handle_t* h) { delete (uv_idle_t*)h; });
                    response->stream_file_chunk();
                });
            } else {
                // Done streaming
                end_response();
            }
        } else {
            // Error or EOF
            end_response();
        }
    }

    void send_file(FilePtr file) {
        if (!file || !file->is_open) return;

        file_source = file;
        streaming_file = true;

        // Get file size
#ifdef _WIN32
        LARGE_INTEGER filesize_li;
        if (GetFileSizeEx((HANDLE)file->handle, &filesize_li)) {
            file_total_size = static_cast<uint64_t>(filesize_li.QuadPart);
        }
#else
        struct stat st;
        if (fstat(file->fd, &st) == 0) {
            file_total_size = static_cast<uint64_t>(st.st_size);
        }
#endif

        // Start streaming
        chunked_mode = true;
        headers["Transfer-Encoding"] = "chunked";
        send_headers();
        stream_file_chunk();
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
    std::map<std::string, std::string> headers;
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
    state->headers[state->current_header_field].assign(at, length);
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
    for (const auto& kv : state->headers) {
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

    state->req_stream_obj = req_obj;

    // Build response object
    auto res_obj = std::make_shared<ObjectValue>();

    // res.writeHead(statusCode, headers?)
    auto writeHead_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "writeHead requires status code", token.loc);
        }

        int code = static_cast<int>(std::get<double>(args[0]));
        state->response->status_code = code;
        state->response->reason = HttpResponse::reason_for_code(code);

        if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr hdrs = std::get<ObjectPtr>(args[1]);
            for (const auto& kv : hdrs->properties) {
                state->response->headers[kv.first] = value_to_string_simple_local(kv.second.value);
            }
        }

        state->response->send_headers();
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
        state->response->headers[name] = value;
        return std::monostate{};
    };
    res_obj->properties["setHeader"] = {
        Value{std::make_shared<FunctionValue>("res.setHeader", setHeader_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.getHeader(name)
    auto getHeader_impl = [state](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "getHeader requires name", token.loc);
        }
        std::string name = value_to_string_simple_local(args[0]);
        auto it = state->response->headers.find(name);
        return (it != state->response->headers.end()) ? Value{it->second} : Value{std::monostate{}};
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

        state->response->write_chunk(data);
        return Value{true};
    };
    res_obj->properties["write"] = {
        Value{std::make_shared<FunctionValue>("res.write", write_impl, nullptr, Token{})},
        false, false, false, Token{}};

    // res.end(data?)
    auto end_impl = [state](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
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
        if (!file->is_open) {
            throw SwaziError("IOError", "File must be open", token.loc);
        }

        state->response->send_file(file);
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
        int status = 302;  // Default to 302 Found

        if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
            status = static_cast<int>(std::get<double>(args[1]));
        }

        // Validate redirect status codes
        if (status != 301 && status != 302 && status != 303 &&
            status != 307 && status != 308) {
            status = 302;  // Default to safe redirect
        }

        state->response->status_code = status;
        state->response->headers["Location"] = location;
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

            uv_close((uv_handle_t*)client, [](uv_handle_t* h) {
                auto* state = static_cast<HttpRequestState*>(h->data);
                if (state) delete state;
                delete (uv_tcp_t*)h;
            });
        }
    } else if (nread < 0) {
        // ✅ Client closed connection (normal or error)
        if (buf->base) free(buf->base);

        uv_close((uv_handle_t*)client, [](uv_handle_t* h) {
            auto* state = static_cast<HttpRequestState*>(h->data);
            if (state) delete state;
            delete (uv_tcp_t*)h;
        });
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

        scheduler_run_on_loop([inst, port, cb, loop]() {
            inst->server_handle = new uv_tcp_t;
            inst->server_handle->data = inst.get();
            uv_tcp_init(loop, inst->server_handle);

            struct sockaddr_in addr;
            uv_ip4_addr("0.0.0.0", port, &addr);
            uv_tcp_bind(inst->server_handle, (const struct sockaddr*)&addr, 0);

            int r = uv_listen((uv_stream_t*)inst->server_handle, 128, on_connection);
            if (r == 0 && cb) {
                CallbackPayload* payload = new CallbackPayload(cb, {});
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
