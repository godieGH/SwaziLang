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

#include "evaluator.hpp"
#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp" // for declaration of native_createServer (optional)
#include <uv.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

// Local simple Value -> string/number helpers (similar to builtins.cpp)
static std::string value_to_string_simple_local(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss; ss << std::get<double>(v); return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::string();
}
static double value_to_number_simple_local(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<std::string>(v)) {
        try { return std::stod(std::get<std::string>(v)); } catch (...) { return 0.0; }
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
    std::string body;
    uv_stream_t* client;
};

struct HttpResponse {
    int status_code = 200;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    bool headers_sent = false;
    uv_stream_t* client = nullptr;

    void writeHead(int code, const std::unordered_map<std::string, std::string>& hdrs) {
        if (headers_sent) return;
        status_code = code;
        headers = hdrs;
        headers_sent = true;
    }

    void end(const std::string& data) {
        if (!headers_sent) {
            headers["Content-Length"] = std::to_string(data.size());
            headers_sent = true;
        }
        body = data;
        send();
    }

    void send() {
        if (!client) return;

        std::ostringstream response;
        response << "HTTP/1.1 " << status_code << " OK\r\n";

        if (headers.find("Content-Type") == headers.end()) {
            headers["Content-Type"] = "text/plain";
        }

        for (const auto& kv : headers) {
            response << kv.first << ": " << kv.second << "\r\n";
        }
        response << "\r\n" << body;

        std::string out = response.str();
        // Allocate a copy for libuv write
        char* buf = static_cast<char*>(malloc(out.size()));
        memcpy(buf, out.data(), out.size());
        uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)out.size());

        uv_write_t* req = new uv_write_t;
        req->data = buf;
        // Write to client and close after write
        uv_write(req, client, &uvbuf, 1, [](uv_write_t* req, int status) {
            if (req->data) free(req->data);
            uv_stream_t* client = req->handle;
            // Close the client handle
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
        req->body = raw.substr(header_end + 4);
    }
    return req;
}

// Server instance bookkeeping
struct ServerInstance {
    uv_tcp_t* server_handle = nullptr;
    FunctionPtr request_handler;
    std::atomic<bool> closed{false};
    int port = 0;
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
            [](uv_handle_t* /*handle*/, size_t suggested, uv_buf_t* buf) {
                buf->base = new char[suggested];
                buf->len = (unsigned int)suggested;
            },
            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
                if (nread > 0) {
                    ServerInstance* srv = static_cast<ServerInstance*>(stream->data);
                    if (!srv) { delete[] buf->base; return; }

                    auto http_req = parse_http_request_simple(buf->base, nread);
                    if (http_req) {
                        http_req->client = stream;

                        // prepare request object (ObjectPtr)
                        auto req_obj = std::make_shared<ObjectValue>();
                        req_obj->properties["method"] = { Value{http_req->method}, false, false, true, Token{} };
                        req_obj->properties["path"] = { Value{http_req->path}, false, false, true, Token{} };
                        req_obj->properties["query"] = { Value{http_req->query}, false, false, true, Token{} };
                        req_obj->properties["body"] = { Value{http_req->body}, false, false, true, Token{} };

                        auto headers_obj = std::make_shared<ObjectValue>();
                        for (const auto& kv : http_req->headers) {
                            headers_obj->properties[kv.first] = { Value{kv.second}, false, false, true, Token{} };
                        }
                        req_obj->properties["headers"] = { Value{headers_obj}, false, false, true, Token{} };

                        // response object
                        auto res_obj = std::make_shared<ObjectValue>();
                        auto http_res = std::make_shared<HttpResponse>();
                        http_res->client = stream;

                        // res.writeHead(code, headers)
                        auto writeHead_impl = [http_res](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*tok*/) -> Value {
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
                            return std::monostate{};
                        };
                        auto writeHead_fn = std::make_shared<FunctionValue>("res.writeHead", writeHead_impl, nullptr, Token{});
                        res_obj->properties["writeHead"] = { Value{writeHead_fn}, false, false, true, Token{} };

                        // res.end(data)
                        auto end_impl = [http_res](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*tok*/) -> Value {
                            std::string data = args.empty() ? "" : value_to_string_simple_local(args[0]);
                            http_res->end(data);
                            return std::monostate{};
                        };
                        auto end_fn = std::make_shared<FunctionValue>("res.end", end_impl, nullptr, Token{});
                        res_obj->properties["end"] = { Value{end_fn}, false, false, true, Token{} };

                        // enqueue invocation of request handler onto main scheduler loop
                        if (srv->request_handler) {
                            FunctionPtr handler = srv->request_handler;
                            // Wrap request/response ObjectPtr into Values for callback
                            scheduler_run_on_loop([handler, req_obj, res_obj]() {
                                try {
                                    // CallbackPayload is a type in your AsyncBridge — create a payload
                                    CallbackPayload* payload = new CallbackPayload(handler, { Value{req_obj}, Value{res_obj} });
                                    // Use the bridge API that accepts void* to avoid linking/type-mismatch.
                                    enqueue_callback_global(static_cast<void*>(payload));
                                } catch (...) {}
                            });
                        }
                    }
                }
                // free buffer
                delete[] buf->base;
                if (nread < 0) {
                    if (nread != UV_EOF) {
                        // log error if desired
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
    server_obj->properties["listen"] = { Value{listen_fn}, false, false, true, Token{} };

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
    server_obj->properties["close"] = { Value{close_fn}, false, false, true, Token{} };

    return Value{ server_obj };
}