// HttpAPI.cpp
// Libuv-backed minimal createServer implementation that integrates with your AsyncBridge.
// Make sure this file is compiled into your binary and that you link with -luv.
//
// Notes:
// - This file assumes the AsyncBridge functions/types below exist and are provided by your project:
//     - void enqueue_callback_global(void* p);
//     - void scheduler_run_on_loop(const std::function<void()>& fn);
//     - uv_loop_t* scheduler_get_loop();   // returns the uv_loop_t* used by the runtime
//     - class CallbackPayload { ... }     // already used elsewhere in your codebase
//   Those symbols appear referenced in your other code (AsyncBridge.hpp), so include that header.
// - The code below uses small local helpers to convert Value -> string / number.

#include <uv.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"  // for declaration of native_createServer (optional)
#include "evaluator.hpp"

// Local simple Value -> string/number helpers (similar to builtins.cpp)
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
static double value_to_number_simple_local(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stod(std::get<std::string>(v));
        } catch (...) { return 0.0; }
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
    return 0.0;
}

// Minimal HTTP parsing/response helpers
struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body_data;  // Changed from string
    uv_stream_t* client;
};

struct HttpResponse {
    int status_code = 200;
    std::string reason = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool headers_sent = false;
    uv_stream_t* client = nullptr;
    bool chunked_mode = false;  // NEW: Track if using chunked encoding

    void writeHead(int code, const std::unordered_map<std::string, std::string>& hdrs) {
        if (headers_sent) return;
        status_code = code;
        headers = hdrs;
        headers_sent = true;
    }

    void end(const std::string& data) {
        if (chunked_mode) {
            // If in chunked mode, send final chunk (0-length) and close
            std::string final_chunk = "0\r\n\r\n";

            char* buf = static_cast<char*>(malloc(final_chunk.size()));
            memcpy(buf, final_chunk.data(), final_chunk.size());
            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)final_chunk.size());

            uv_write_t* req = new uv_write_t;
            req->data = buf;

            uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int status) {
                if (req->data) free(req->data);
                uv_stream_t* client = req->handle;
                uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                delete req;
            });
            return;
        }

        // Normal (non-chunked) response
        if (status_code == 204 || status_code == 304) {
            body.clear();
            headers.erase("Content-Length");
            headers.erase("content-length");
            headers_sent = true;
        } else {
            if (!headers_sent) {
                headers["Content-Length"] = std::to_string(data.size());
                headers_sent = true;
            }
            body = data;
        }
        send();
    }

    static std::string reason_for_code(int code) {
        switch (code) {
            case 100:
                return "Continue";
            case 101:
                return "Switching Protocols";
            case 200:
                return "OK";
            case 201:
                return "Created";
            case 202:
                return "Accepted";
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
            case 405:
                return "Method Not Allowed";
            case 410:
                return "Gone";
            case 500:
                return "Internal Server Error";
            case 501:
                return "Not Implemented";
            case 502:
                return "Bad Gateway";
            case 503:
                return "Service Unavailable";
            default:
                return std::string();
        }
    }

    void send() {
        if (!client) return;

        std::ostringstream response;
        std::string rp = !reason.empty() ? reason : reason_for_code(status_code);

        if (!rp.empty()) {
            response << "HTTP/1.1 " << status_code << " " << rp << "\r\n";
        } else {
            response << "HTTP/1.1 " << status_code << "\r\n";
        }

        if (headers.find("Content-Type") == headers.end() &&
            headers.find("content-type") == headers.end()) {
            headers["Content-Type"] = "text/plain";
        }

        for (const auto& kv : headers) {
            response << kv.first << ": " << kv.second << "\r\n";
        }
        response << "\r\n";

        if (!(status_code == 204 || status_code == 304))
            response << body;

        std::string out = response.str();
        char* buf = static_cast<char*>(malloc(out.size()));
        memcpy(buf, out.data(), out.size());
        uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)out.size());

        uv_write_t* req = new uv_write_t;
        req->data = buf;

        uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int status) {
            if (req->data) free(req->data);
            uv_stream_t* client = req->handle;
            uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
            delete req;
        });
    }
};
static std::shared_ptr<HttpRequest> parse_http_request_simple(const char* data, ssize_t len) {
    std::string raw(data, len);
    auto req = std::make_shared<HttpRequest>();

    size_t first_line_end = raw.find("\r\n");
    if (first_line_end == std::string::npos) return nullptr;
    std::string first = raw.substr(0, first_line_end);
    std::istringstream iss(first);
    std::string path_query;
    iss >> req->method >> path_query;
    size_t qpos = path_query.find('?');
    if (qpos != std::string::npos) {
        req->path = path_query.substr(0, qpos);
        req->query = path_query.substr(qpos + 1);
    } else {
        req->path = path_query;
    }

    // headers
    size_t header_start = first_line_end + 2;
    size_t header_end = raw.find("\r\n\r\n", header_start);
    if (header_end != std::string::npos) {
        std::string headers_block = raw.substr(header_start, header_end - header_start);
        std::istringstream hs(headers_block);
        std::string line;
        while (std::getline(hs, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string val = line.substr(colon + 1);
                // trim
                val.erase(0, val.find_first_not_of(" \t"));
                req->headers[key] = val;
            }
        }
        size_t body_start = header_end + 4;
        req->body_data.assign(raw.begin() + body_start, raw.end());
    }
    return req;
}

// Server instance bookkeeping
struct ServerInstance {
    uv_tcp_t* server_handle = nullptr;
    FunctionPtr request_handler;
    std::atomic<bool> closed{false};
    int port = 0;

    // ===== ADD THIS =====
    struct InProgressRequest {
        std::string method;
        std::string path;
        std::string query;
        std::unordered_map<std::string, std::string> headers;
        std::vector<uint8_t> body_data;
        size_t expected_length = 0;
        bool headers_complete = false;
    };

    std::unordered_map<uv_stream_t*, InProgressRequest> pending_requests;
    // ===== END ADD =====
};
static std::mutex g_servers_mutex;
static std::unordered_map<long long, std::shared_ptr<ServerInstance>> g_servers;
static std::atomic<long long> g_next_server_id{1};

// on_connection callback
static void on_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    ServerInstance* srv = static_cast<ServerInstance*>(server->data);
    if (!srv || srv->closed.load()) return;

    uv_tcp_t* client = new uv_tcp_t;
    uv_tcp_init(server->loop, client);

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        client->data = srv;

        uv_read_start((uv_stream_t*)client,
            // Allocation callback
            [](uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
                buf->base = new char[suggested];
                buf->len = (unsigned int)suggested;
            },
            // Read callback - THIS IS WHERE THE FIX GOES
            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
                if (nread > 0) {
                    ServerInstance* srv = static_cast<ServerInstance*>(stream->data);
                    if (!srv) {
                        delete[] buf->base;
                        return;
                    }

                    // ===== NEW: Get or create in-progress request =====
                    auto& in_progress = srv->pending_requests[stream];

                    if (!in_progress.headers_complete) {
                        // Still reading headers
                        std::string chunk(buf->base, nread);
                        size_t header_end = chunk.find("\r\n\r\n");

                        if (header_end != std::string::npos) {
                            // Headers complete! Parse them
                            std::istringstream iss(chunk.substr(0, header_end));
                            std::string first_line;
                            std::getline(iss, first_line);

                            // Parse first line
                            std::istringstream line_stream(first_line);
                            std::string path_query;
                            line_stream >> in_progress.method >> path_query;

                            size_t qpos = path_query.find('?');
                            if (qpos != std::string::npos) {
                                in_progress.path = path_query.substr(0, qpos);
                                in_progress.query = path_query.substr(qpos + 1);
                            } else {
                                in_progress.path = path_query;
                            }

                            // Parse headers
                            std::string header_line;
                            while (std::getline(iss, header_line) && !header_line.empty()) {
                                if (header_line.back() == '\r') header_line.pop_back();
                                size_t colon = header_line.find(':');
                                if (colon != std::string::npos) {
                                    std::string key = header_line.substr(0, colon);
                                    std::string val = header_line.substr(colon + 1);
                                    val.erase(0, val.find_first_not_of(" \t"));
                                    in_progress.headers[key] = val;
                                }
                            }

                            in_progress.headers_complete = true;

                            // Get Content-Length
                            auto cl_it = in_progress.headers.find("Content-Length");
                            if (cl_it != in_progress.headers.end()) {
                                in_progress.expected_length = std::stoull(cl_it->second);
                            }

                            // Append any body data from this chunk
                            size_t body_start = header_end + 4;
                            if (body_start < chunk.size()) {
                                in_progress.body_data.insert(
                                    in_progress.body_data.end(),
                                    chunk.begin() + body_start,
                                    chunk.end());
                            }
                        }
                    } else {
                        // Reading body chunks
                        in_progress.body_data.insert(
                            in_progress.body_data.end(),
                            buf->base,
                            buf->base + nread);
                    }

                    // ===== Check if we have complete request =====
                    if (in_progress.headers_complete &&
                        in_progress.body_data.size() >= in_progress.expected_length) {
                        // NOW we have the complete request!
                        auto http_req = std::make_shared<HttpRequest>();
                        http_req->method = in_progress.method;
                        http_req->path = in_progress.path;
                        http_req->query = in_progress.query;
                        http_req->headers = in_progress.headers;
                        http_req->body_data = std::move(in_progress.body_data);
                        http_req->client = stream;

                        // ===== Your existing request/response handling code =====
                        auto req_obj = std::make_shared<ObjectValue>();
                        req_obj->properties["method"] = {Value{http_req->method}, false, false, true, Token{}};
                        req_obj->properties["path"] = {Value{http_req->path}, false, false, true, Token{}};
                        req_obj->properties["query"] = {Value{http_req->query}, false, false, true, Token{}};

                        // normalize content-type lookup
                        std::string content_type;
                        for (auto& kv : http_req->headers) {
                            std::string k = kv.first;
                            std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                            if (k == "content-type") {
                                content_type = kv.second;
                                break;
                            }
                        }

                        // parse out mime and params
                        auto parse_content_type = [](const std::string& ct) {
                            auto s = ct;
                            std::string low = s;
                            std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                            size_t semi = low.find(';');
                            std::string mime = semi == std::string::npos ? low : low.substr(0, semi);
                            std::string params = semi == std::string::npos ? "" : low.substr(semi + 1);
                            return std::pair<std::string, std::string>{mime, params};
                        };
                        auto [mime, params] = parse_content_type(content_type);
                        bool is_text = false;
                        if (mime.rfind("text/", 0) == 0) is_text = true;
                        if (mime == "application/json" || mime.find("+json") != std::string::npos) is_text = true;
                        if (mime == "application/xml" || mime.find("+xml") != std::string::npos) is_text = true;
                        if (mime == "application/x-www-form-urlencoded") is_text = true;

                        // always expose raw buffer
                        auto body_buffer = std::make_shared<BufferValue>();
                        body_buffer->data = http_req->body_data;
                        body_buffer->encoding = "binary";
                        req_obj->properties["bodyBuffer"] = {Value{body_buffer}, false, false, true, Token{}};

                        if (is_text) {
                            std::string s(body_buffer->data.begin(), body_buffer->data.end());
                            req_obj->properties["body"] = {Value{s}, false, false, true, Token{}};
                        } else {
                            req_obj->properties["body"] = {Value{body_buffer}, false, false, true, Token{}};
                        }

                        auto headers_obj = std::make_shared<ObjectValue>();
                        for (const auto& kv : http_req->headers) {
                            headers_obj->properties[kv.first] = {Value{kv.second}, false, false, true, Token{}};
                        }
                        req_obj->properties["headers"] = {Value{headers_obj}, false, false, true, Token{}};
                        auto req_stream = create_network_readable_stream_object((uv_tcp_t*)stream);
                        req_obj->properties["stream"] = {Value{req_stream}, false, false, true, Token{}};

                        // response object
                        auto res_obj = std::make_shared<ObjectValue>();
                        auto http_res = std::make_shared<HttpResponse>();
                        http_res->client = stream;
                        auto res_stream = create_network_writable_stream_object((uv_tcp_t*)stream);
                        res_obj->properties["stream"] = {Value{res_stream}, false, false, true, Token{}};

                        // res.writeHead(code, headers)
                        auto writeHead_impl = [http_res](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
                            if (args.empty()) return std::monostate{};
                            int code = static_cast<int>(value_to_number_simple_local(args[0]));
                            std::unordered_map<std::string, std::string> hdrs;
                            if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
                                ObjectPtr hobj = std::get<ObjectPtr>(args[1]);
                                for (const auto& kv : hobj->properties) {
                                    hdrs[kv.first] = value_to_string_simple_local(kv.second.value);
                                }
                            }
                            http_res->writeHead(code, hdrs);
                            if (http_res->reason.empty()) http_res->reason = HttpResponse::reason_for_code(code);
                            return std::monostate{};
                        };
                        auto writeHead_fn = std::make_shared<FunctionValue>("res.writeHead", writeHead_impl, nullptr, Token{});
                        res_obj->properties["writeHead"] = {Value{writeHead_fn}, false, false, true, Token{}};

                        // ========== res.end() - Support Buffer ==========
                        auto end_impl = [http_res](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                            std::string data;

                            if (args.empty()) {
                                data = "";
                            } else if (std::holds_alternative<BufferPtr>(args[0])) {
                                // Handle Buffer - convert to string for HTTP body
                                BufferPtr buf = std::get<BufferPtr>(args[0]);
                                data.assign(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
                            } else if (std::holds_alternative<std::string>(args[0])) {
                                // Handle string
                                data = std::get<std::string>(args[0]);
                            } else {
                                // Handle other types (convert to string)
                                data = value_to_string_simple_local(args[0]);
                            }

                            // Honor status codes that forbid body
                            if (http_res->status_code == 204 || http_res->status_code == 304) {
                                http_res->end("");
                            } else {
                                http_res->end(data);
                            }
                            return std::monostate{};
                        };
                        auto end_fn = std::make_shared<FunctionValue>("res.end", end_impl, nullptr, Token{});
                        res_obj->properties["end"] = {Value{end_fn}, false, false, true, Token{}};

                        // ========== NEW res.write() - Support chunked responses ==========
                        auto write_impl = [http_res](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                            if (args.empty()) {
                                return Value{true};
                            }

                            http_res->chunked_mode = true;
                            std::string chunk_data;

                            if (std::holds_alternative<BufferPtr>(args[0])) {
                                // Handle Buffer
                                BufferPtr buf = std::get<BufferPtr>(args[0]);
                                chunk_data.assign(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
                            } else if (std::holds_alternative<std::string>(args[0])) {
                                // Handle string
                                chunk_data = std::get<std::string>(args[0]);
                            } else {
                                // Handle other types
                                chunk_data = value_to_string_simple_local(args[0]);
                            }

                            // Send headers if not sent yet
                            if (!http_res->headers_sent) {
                                std::ostringstream header_stream;
                                std::string rp = !http_res->reason.empty() ? http_res->reason
                                                                           : HttpResponse::reason_for_code(http_res->status_code);

                                if (!rp.empty()) {
                                    header_stream << "HTTP/1.1 " << http_res->status_code << " " << rp << "\r\n";
                                } else {
                                    header_stream << "HTTP/1.1 " << http_res->status_code << "\r\n";
                                }

                                // Add Transfer-Encoding: chunked for streaming
                                http_res->headers["Transfer-Encoding"] = "chunked";

                                // Set default content-type if not set
                                if (http_res->headers.find("Content-Type") == http_res->headers.end() &&
                                    http_res->headers.find("content-type") == http_res->headers.end()) {
                                    http_res->headers["Content-Type"] = "text/plain";
                                }

                                for (const auto& kv : http_res->headers) {
                                    header_stream << kv.first << ": " << kv.second << "\r\n";
                                }
                                header_stream << "\r\n";

                                std::string headers_str = header_stream.str();

                                // Send headers
                                char* header_buf = static_cast<char*>(malloc(headers_str.size()));
                                memcpy(header_buf, headers_str.data(), headers_str.size());
                                uv_buf_t header_uvbuf = uv_buf_init(header_buf, (unsigned int)headers_str.size());

                                uv_write_t* header_req = new uv_write_t;
                                header_req->data = header_buf;

                                uv_write(header_req, http_res->client, &header_uvbuf, 1, [](uv_write_t* req, int status) {
                                    if (req->data) free(req->data);
                                    delete req;
                                });

                                http_res->headers_sent = true;
                            }

                            // Send chunk in HTTP chunked format
                            if (!chunk_data.empty()) {
                                std::ostringstream chunk_stream;
                                chunk_stream << std::hex << chunk_data.size() << "\r\n";
                                chunk_stream << chunk_data << "\r\n";

                                std::string chunk_str = chunk_stream.str();

                                char* chunk_buf = static_cast<char*>(malloc(chunk_str.size()));
                                memcpy(chunk_buf, chunk_str.data(), chunk_str.size());
                                uv_buf_t chunk_uvbuf = uv_buf_init(chunk_buf, (unsigned int)chunk_str.size());

                                uv_write_t* chunk_req = new uv_write_t;
                                chunk_req->data = chunk_buf;

                                uv_write(chunk_req, http_res->client, &chunk_uvbuf, 1, [](uv_write_t* req, int status) {
                                    if (req->data) free(req->data);
                                    delete req;
                                });
                            }

                            return Value{true};
                        };
                        auto write_fn = std::make_shared<FunctionValue>("res.write", write_impl, nullptr, Token{});
                        res_obj->properties["write"] = {Value{write_fn}, false, false, true, Token{}};

                        // req.save()
                        auto save_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                            if (args.size() < 2) {
                                throw SwaziError("TypeError", "save requires (buffer, path)", token.loc);
                            }
                            if (!std::holds_alternative<BufferPtr>(args[0])) {
                                throw SwaziError("TypeError", "First arg must be Buffer", token.loc);
                            }
                            BufferPtr buf = std::get<BufferPtr>(args[0]);
                            std::string path = value_to_string_simple_local(args[1]);
                            std::ofstream out(path, std::ios::binary);
                            if (!out) {
                                throw SwaziError("IOError", "Failed to open " + path, token.loc);
                            }
                            out.write(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
                            return Value{true};
                        };
                        auto save_fn = std::make_shared<FunctionValue>("save", save_impl, nullptr, Token{});
                        req_obj->properties["save"] = {Value{save_fn}, false, false, true, Token{}};

                        // res.status(code)
                        auto status_impl = [http_res, res_obj](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
                            if (args.empty()) return Value{res_obj};
                            int code = static_cast<int>(value_to_number_simple_local(args[0]));
                            http_res->status_code = code;
                            http_res->reason = HttpResponse::reason_for_code(code);
                            return Value{res_obj};
                        };
                        auto status_fn = std::make_shared<FunctionValue>("res.status", status_impl, nullptr, Token{});
                        res_obj->properties["status"] = {Value{status_fn}, false, false, true, Token{}};

                        // res.message(text)
                        auto message_impl = [http_res, res_obj](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
                            if (!args.empty()) {
                                http_res->reason = value_to_string_simple_local(args[0]);
                            }
                            return Value{res_obj};
                        };
                        auto message_fn = std::make_shared<FunctionValue>("res.message", message_impl, nullptr, Token{});
                        res_obj->properties["message"] = {Value{message_fn}, false, false, true, Token{}};

                        // Call handler
                        if (srv->request_handler) {
                            FunctionPtr handler = srv->request_handler;
                            scheduler_run_on_loop([handler, req_obj, res_obj]() {
                                try {
                                    CallbackPayload* payload = new CallbackPayload(handler, {Value{req_obj}, Value{res_obj}});
                                    enqueue_callback_global(static_cast<void*>(payload));
                                } catch (...) {}
                            });
                        }

                        // ===== Clean up in-progress request =====
                        srv->pending_requests.erase(stream);
                    }
                }

                // free buffer
                delete[] buf->base;

                if (nread < 0) {
                    // Connection closed - cleanup
                    ServerInstance* srv = static_cast<ServerInstance*>(stream->data);
                    if (srv) {
                        srv->pending_requests.erase(stream);
                    }
                    uv_close((uv_handle_t*)stream, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                }
            });
    } else {
        uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
    }
}
// native_createServer exported to builtins.cpp
Value native_createServer(const std::vector<Value>& args, EnvPtr /*env*/, const Token& /*token*/) {
    if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
        throw SwaziError("TypeError", "createServer requires a request handler function", TokenLocation());
    }
    FunctionPtr handler = std::get<FunctionPtr>(args[0]);

    auto inst = std::make_shared<ServerInstance>();
    inst->request_handler = handler;

    long long id = g_next_server_id.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(g_servers_mutex);
        g_servers[id] = inst;
    }

    // build server object to return to JS
    auto server_obj = std::make_shared<ObjectValue>();

    // server.listen(port, callback?)
    auto listen_impl = [inst, id](const std::vector<Value>& args, EnvPtr /*env*/, const Token& token) -> Value {
        if (args.empty()) throw SwaziError("TypeError", "listen requires port number", token.loc);
        int port = static_cast<int>(value_to_number_simple_local(args[0]));
        FunctionPtr cb = (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) ? std::get<FunctionPtr>(args[1]) : nullptr;

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) throw SwaziError("RuntimeError", "No event loop available for server", token.loc);

        scheduler_run_on_loop([inst, port, cb, loop]() {
            inst->server_handle = new uv_tcp_t;
            inst->server_handle->data = inst.get();
            uv_tcp_init(loop, inst->server_handle);
            struct sockaddr_in addr;
            uv_ip4_addr("0.0.0.0", port, &addr);
            uv_tcp_bind(inst->server_handle, (const struct sockaddr*)&addr, 0);
            int r = uv_listen((uv_stream_t*)inst->server_handle, 128, on_connection);
            if (r == 0) {
                // optionally call the callback on the runtime thread
                if (cb) {
                    CallbackPayload* payload = new CallbackPayload(cb, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                }
            } else {
                // listen failed: could enqueue error to callback if desired
            }
        });
        return std::monostate{};
    };
    auto listen_fn = std::make_shared<FunctionValue>("server.listen", listen_impl, nullptr, Token{});
    server_obj->properties["listen"] = {Value{listen_fn}, false, false, true, Token{}};

    // server.close(callback?)
    auto close_impl = [inst, id](const std::vector<Value>& args, EnvPtr /*env*/, const Token& /*token*/) -> Value {
        FunctionPtr cb = (!args.empty() && std::holds_alternative<FunctionPtr>(args[0])) ? std::get<FunctionPtr>(args[0]) : nullptr;
        inst->closed.store(true);

        scheduler_run_on_loop([inst, cb]() {
            if (inst->server_handle) {
                uv_close((uv_handle_t*)inst->server_handle, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                inst->server_handle = nullptr;
            }
            if (cb) {
                CallbackPayload* payload = new CallbackPayload(cb, {});
                enqueue_callback_global(static_cast<void*>(payload));
            }
        });

        // remove from map
        std::lock_guard<std::mutex> lk(g_servers_mutex);
        g_servers.erase(id);
        return std::monostate{};
    };
    auto close_fn = std::make_shared<FunctionValue>("server.close", close_impl, nullptr, Token{});
    server_obj->properties["close"] = {Value{close_fn}, false, false, true, Token{}};

    return Value{server_obj};
}