// tcp.cc - TCP client and server implementation using libuv
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "./net.hpp"

// At file scope in tcp.cc
static std::atomic<int> g_active_tcp_work{0};

bool tcp_has_active_work() {
    return g_active_tcp_work.load() > 0;
}

namespace NetHelpers {
std::string value_to_string(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "true" : "false";
    return std::string();
}

double value_to_number(const Value& v) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<std::string>(v)) {
        try {
            return std::stod(std::get<std::string>(v));
        } catch (...) { return 0.0; }
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? 1.0 : 0.0;
    return 0.0;
}

bool is_buffer(const Value& v) {
    return std::holds_alternative<BufferPtr>(v);
}

std::vector<uint8_t> get_buffer_data(const Value& v) {
    if (std::holds_alternative<BufferPtr>(v)) {
        return std::get<BufferPtr>(v)->data;
    }
    if (std::holds_alternative<std::string>(v)) {
        std::string s = std::get<std::string>(v);
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    return {};
}
}  // namespace NetHelpers

// TCP Server instance
struct TcpServerInstance {
    uv_tcp_t* server_handle = nullptr;
    FunctionPtr connection_handler;
    std::atomic<bool> closed{false};
    int port = 0;
    std::string host;
};

// TCP Socket (client or server connection)
struct TcpSocketInstance {
    uv_tcp_t* socket_handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> reading{false};  // Track if we're already reading
    FunctionPtr on_data_handler;
    FunctionPtr on_close_handler;
    FunctionPtr on_error_handler;
    FunctionPtr on_connect_handler;
    std::string remote_address;
    int remote_port = 0;
    long long socket_id = 0;  // Store our own ID for cleanup
};

static std::mutex g_tcp_servers_mutex;
static std::unordered_map<long long, std::shared_ptr<TcpServerInstance>> g_tcp_servers;
static std::atomic<long long> g_next_tcp_server_id{1};

static std::mutex g_tcp_sockets_mutex;
static std::unordered_map<long long, std::shared_ptr<TcpSocketInstance>> g_tcp_sockets;
static std::atomic<long long> g_next_tcp_socket_id{1};

// Helper to start reading on a socket (idempotent)
static void start_reading_if_needed(TcpSocketInstance* inst) {
    if (!inst || !inst->socket_handle || inst->closed.load()) {
        return;
    }

    if (inst->reading.exchange(true)) {
        return;  // Already reading
    }

    uv_read_start((uv_stream_t*)inst->socket_handle,
        [](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
            buf->base = new char[suggested];
            buf->len = (unsigned int)suggested;
        },
        [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
            TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(stream->data);

            if (nread > 0 && inst && inst->on_data_handler) {
                auto buffer = std::make_shared<BufferValue>();
                buffer->data.assign(buf->base, buf->base + nread);
                buffer->encoding = "binary";

                FunctionPtr handler = inst->on_data_handler;
                CallbackPayload* payload = new CallbackPayload(handler, {Value{buffer}});
                enqueue_callback_global(static_cast<void*>(payload));
            }

            if (buf->base) {
                delete[] buf->base;
            }

            if (nread < 0) {
                inst->reading.store(false);

                if (inst && inst->on_close_handler) {
                    FunctionPtr handler = inst->on_close_handler;
                    CallbackPayload* payload = new CallbackPayload(handler, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                }

                if (!inst->closed.exchange(true)) {
                    long long sock_id = inst->socket_id;
                    uv_close((uv_handle_t*)stream, [](uv_handle_t* h) {
                        TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                        if (inst) {
                            long long sock_id = inst->socket_id;
                            std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                            g_tcp_sockets.erase(sock_id);
                        }
                        delete (uv_tcp_t*)h;
                    });
                }
            }
        });
}
// TCP connection callback
static void on_tcp_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    TcpServerInstance* srv = static_cast<TcpServerInstance*>(server->data);
    if (!srv || srv->closed.load()) return;

    uv_tcp_t* client = new uv_tcp_t;
    uv_tcp_init(server->loop, client);

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        // Create socket instance
        auto sock_inst = std::make_shared<TcpSocketInstance>();
        sock_inst->socket_handle = client;
        sock_inst->closed = false;

        long long sock_id = g_next_tcp_socket_id.fetch_add(1);
        sock_inst->socket_id = sock_id;

        {
            std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
            g_tcp_sockets[sock_id] = sock_inst;
        }

        client->data = sock_inst.get();

        // Get remote address
        struct sockaddr_storage addr;
        int namelen = sizeof(addr);
        uv_tcp_getpeername(client, (struct sockaddr*)&addr, &namelen);

        if (addr.ss_family == AF_INET) {
            struct sockaddr_in* addr_in = (struct sockaddr_in*)&addr;
            char ip[INET_ADDRSTRLEN];
            uv_ip4_name(addr_in, ip, sizeof(ip));
            sock_inst->remote_address = ip;
            sock_inst->remote_port = ntohs(addr_in->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6* addr_in6 = (struct sockaddr_in6*)&addr;
            char ip[INET6_ADDRSTRLEN];
            uv_ip6_name(addr_in6, ip, sizeof(ip));
            sock_inst->remote_address = ip;
            sock_inst->remote_port = ntohs(addr_in6->sin6_port);
        }

        // Create socket object to pass to handler
        auto socket_obj = std::make_shared<ObjectValue>();
        Token tok;
        tok.loc = TokenLocation("<tcp>", 0, 0, 0);

        // socket.write(data)
        auto write_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return Value{false};
            if (sock_inst->closed.load() || !sock_inst->socket_handle) {
                throw SwaziError("IOError", "Socket is closed", token.loc);
            }

            std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
            if (data.empty()) return Value{false};

            char* buf = static_cast<char*>(malloc(data.size()));
            memcpy(buf, data.data(), data.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
            uv_write_t* req = new uv_write_t;
            req->data = buf;

            int r = uv_write(req, (uv_stream_t*)sock_inst->socket_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    if (req->data) free(req->data);
                    delete req;
                });

            return Value{r == 0};
        };
        auto write_fn = std::make_shared<FunctionValue>("socket.write", write_impl, nullptr, tok);
        socket_obj->properties["write"] = {Value{write_fn}, false, false, true, tok};

        // socket.close()
        auto close_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.exchange(true) && sock_inst->socket_handle) {
                long long sock_id = sock_inst->socket_id;
                uv_close((uv_handle_t*)sock_inst->socket_handle, [](uv_handle_t* h) {
                    TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                    if (inst) {
                        long long sock_id = inst->socket_id;
                        std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                        g_tcp_sockets.erase(sock_id);
                    }
                    delete (uv_tcp_t*)h;
                });
                sock_inst->socket_handle = nullptr;
            }
            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, tok);
        socket_obj->properties["close"] = {Value{close_fn}, false, false, true, tok};

        // socket.on(event, handler)
        auto on_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "on() requires event name and handler", token.loc);
            }

            std::string event = NetHelpers::value_to_string(args[0]);
            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "Handler must be a function", token.loc);
            }

            FunctionPtr handler = std::get<FunctionPtr>(args[1]);

            if (event == "data") {
                sock_inst->on_data_handler = handler;
                start_reading_if_needed(sock_inst.get());
            } else if (event == "close") {
                sock_inst->on_close_handler = handler;
            } else if (event == "error") {
                sock_inst->on_error_handler = handler;
            }

            return std::monostate{};
        };
        auto on_fn = std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, tok);
        socket_obj->properties["on"] = {Value{on_fn}, false, false, true, tok};

        // Add remote address info
        socket_obj->properties["remoteAddress"] = {Value{sock_inst->remote_address}, false, false, true, tok};
        socket_obj->properties["remotePort"] = {Value{static_cast<double>(sock_inst->remote_port)}, false, false, true, tok};

        // Call connection handler
        if (srv->connection_handler) {
            FunctionPtr handler = srv->connection_handler;
            CallbackPayload* payload = new CallbackPayload(handler, {Value{socket_obj}});
            enqueue_callback_global(static_cast<void*>(payload));
        }
    } else {
        uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
    }
}

std::shared_ptr<ObjectValue> make_tcp_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<tcp>", 0, 0, 0);

    // tcp.createServer(connectionHandler)
    auto createServer_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
            throw SwaziError("TypeError", "createServer requires a connection handler", token.loc);
        }

        FunctionPtr handler = std::get<FunctionPtr>(args[0]);

        auto inst = std::make_shared<TcpServerInstance>();
        inst->connection_handler = handler;

        long long id = g_next_tcp_server_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(g_tcp_servers_mutex);
            g_tcp_servers[id] = inst;
        }

        auto server_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<tcp>", 0, 0, 0);

        // server.listen(port, host?, callback?)
        auto listen_impl = [inst, id](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "listen requires port", token.loc);
            }

            int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            std::string host = "0.0.0.0";
            FunctionPtr cb = nullptr;

            if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
                host = std::get<std::string>(args[1]);
            }

            if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2])) {
                cb = std::get<FunctionPtr>(args[2]);
            } else if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                cb = std::get<FunctionPtr>(args[1]);
            }

            inst->port = port;
            inst->host = host;

            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                throw SwaziError("RuntimeError", "No event loop available", token.loc);
            }

            scheduler_run_on_loop([inst, port, host, cb, loop]() {
                inst->server_handle = new uv_tcp_t;
                inst->server_handle->data = inst.get();
                uv_tcp_init(loop, inst->server_handle);

                struct sockaddr_in addr;
                uv_ip4_addr(host.c_str(), port, &addr);
                uv_tcp_bind(inst->server_handle, (const struct sockaddr*)&addr, 0);

                int r = uv_listen((uv_stream_t*)inst->server_handle, 128, on_tcp_connection);

                if (cb) {
                    if (r == 0) {
                        CallbackPayload* payload = new CallbackPayload(cb, {});
                        enqueue_callback_global(static_cast<void*>(payload));
                    } else {
                        auto err_msg = std::string("Listen failed: ") + uv_strerror(r);
                        CallbackPayload* payload = new CallbackPayload(cb, {Value{err_msg}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                }
            });

            return std::monostate{};
        };
        auto listen_fn = std::make_shared<FunctionValue>("server.listen", listen_impl, nullptr, stok);
        server_obj->properties["listen"] = {Value{listen_fn}, false, false, true, stok};

        // server.close(callback?)
        auto close_impl = [inst, id](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            FunctionPtr cb = (!args.empty() && std::holds_alternative<FunctionPtr>(args[0]))
                ? std::get<FunctionPtr>(args[0])
                : nullptr;

            inst->closed.store(true);

            scheduler_run_on_loop([inst, cb, id]() {
                if (inst->server_handle) {
                    uv_close((uv_handle_t*)inst->server_handle, [](uv_handle_t* h) {
                        delete (uv_tcp_t*)h;
                    });
                    inst->server_handle = nullptr;
                }

                {
                    std::lock_guard<std::mutex> lk(g_tcp_servers_mutex);
                    g_tcp_servers.erase(id);
                }

                if (cb) {
                    CallbackPayload* payload = new CallbackPayload(cb, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                }
            });

            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("server.close", close_impl, nullptr, stok);
        server_obj->properties["close"] = {Value{close_fn}, false, false, true, stok};

        return Value{server_obj};
    };

    auto createServer_fn = std::make_shared<FunctionValue>("tcp.createServer", createServer_impl, env, tok);
    obj->properties["createServer"] = {Value{createServer_fn}, false, false, true, tok};

    // tcp.connect(port, host?, callback?)
    auto connect_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "connect requires port", token.loc);
        }

        int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
        std::string host = "127.0.0.1";
        FunctionPtr cb = nullptr;

        g_active_tcp_work.fetch_add(1);

        if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
            host = std::get<std::string>(args[1]);
        }

        if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2])) {
            cb = std::get<FunctionPtr>(args[2]);
        } else if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            cb = std::get<FunctionPtr>(args[1]);
        }

        auto sock_inst = std::make_shared<TcpSocketInstance>();
        sock_inst->on_connect_handler = cb;

        long long sock_id = g_next_tcp_socket_id.fetch_add(1);
        sock_inst->socket_id = sock_id;

        {
            std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
            g_tcp_sockets[sock_id] = sock_inst;
        }

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            throw SwaziError("RuntimeError", "No event loop available", token.loc);
        }

        // CREATE SOCKET HANDLE IMMEDIATELY (synchronously)
        sock_inst->socket_handle = new uv_tcp_t;
        sock_inst->socket_handle->data = sock_inst.get();
        uv_tcp_init(loop, sock_inst->socket_handle);

        // Create socket object with methods (same as before)
        auto socket_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<tcp>", 0, 0, 0);

        // socket.write(data)
        auto write_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return Value{false};
            if (sock_inst->closed.load() || !sock_inst->socket_handle) {
                throw SwaziError("IOError", "Socket is closed", token.loc);
            }

            std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
            if (data.empty()) return Value{false};

            char* buf = static_cast<char*>(malloc(data.size()));
            memcpy(buf, data.data(), data.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
            uv_write_t* req = new uv_write_t;
            req->data = buf;

            int r = uv_write(req, (uv_stream_t*)sock_inst->socket_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    if (req->data) free(req->data);
                    delete req;
                });

            return Value{r == 0};
        };
        auto write_fn = std::make_shared<FunctionValue>("socket.write", write_impl, nullptr, stok);
        socket_obj->properties["write"] = {Value{write_fn}, false, false, true, stok};

        // socket.close()
        auto close_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.exchange(true) && sock_inst->socket_handle) {
                long long sock_id = sock_inst->socket_id;
                uv_close((uv_handle_t*)sock_inst->socket_handle, [](uv_handle_t* h) {
                    TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                    if (inst) {
                        long long sock_id = inst->socket_id;
                        std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                        g_tcp_sockets.erase(sock_id);
                    }
                    delete (uv_tcp_t*)h;
                });
                sock_inst->socket_handle = nullptr;
            }
            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, stok);
        socket_obj->properties["close"] = {Value{close_fn}, false, false, true, stok};

        // socket.on(event, handler)
        auto on_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "on() requires event name and handler", token.loc);
            }

            std::string event = NetHelpers::value_to_string(args[0]);

            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "Handler must be a function", token.loc);
            }

            FunctionPtr handler = std::get<FunctionPtr>(args[1]);

            if (event == "data") {
                sock_inst->on_data_handler = handler;
                // DON'T call start_reading_if_needed here - socket isn't connected yet!
                // start_reading_if_needed(sock_inst.get());  <-- REMOVE THIS
            } else if (event == "close") {
                sock_inst->on_close_handler = handler;
            } else if (event == "error") {
                sock_inst->on_error_handler = handler;
            } else if (event == "connect") {
                sock_inst->on_connect_handler = handler;
            }

            return std::monostate{};
        };
        auto on_fn = std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, stok);
        socket_obj->properties["on"] = {Value{on_fn}, false, false, true, stok};

        // NOW schedule the connection attempt (but handle already exists)
        struct sockaddr_in addr;
        uv_ip4_addr(host.c_str(), port, &addr);

        struct ConnectData {
            std::shared_ptr<TcpSocketInstance> sock_inst;
            std::shared_ptr<ObjectValue> socket_obj;
        };

        uv_connect_t* connect_req = new uv_connect_t;
        connect_req->data = new ConnectData{sock_inst, socket_obj};

        uv_tcp_connect(connect_req, sock_inst->socket_handle,
            (const struct sockaddr*)&addr,
            [](uv_connect_t* req, int status) {
                auto* conn_data = static_cast<ConnectData*>(req->data);
                std::shared_ptr<TcpSocketInstance> inst = conn_data->sock_inst;
                std::shared_ptr<ObjectValue> socket_obj = conn_data->socket_obj;
                delete conn_data;

                g_active_tcp_work.fetch_sub(1);

                if (status == 0) {
                    // Start reading
                    start_reading_if_needed(inst.get());

                    if (inst->on_connect_handler) {
                        FunctionPtr handler = inst->on_connect_handler;
                        CallbackPayload* payload = new CallbackPayload(handler, {socket_obj});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                } else {
                    if (inst->on_error_handler) {
                        auto err_msg = std::string("Connection failed: ") + uv_strerror(status);
                        FunctionPtr handler = inst->on_error_handler;
                        CallbackPayload* payload = new CallbackPayload(handler, {Value{err_msg}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }

                    // Clean up failed connection
                    if (!inst->closed.exchange(true) && inst->socket_handle) {
                        long long sock_id = inst->socket_id;
                        uv_close((uv_handle_t*)inst->socket_handle, [](uv_handle_t* h) {
                            TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                            if (inst) {
                                long long sock_id = inst->socket_id;
                                std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                                g_tcp_sockets.erase(sock_id);
                            }
                            delete (uv_tcp_t*)h;
                        });
                    }
                }

                delete req;
            });

        return Value{socket_obj};
    };

    auto connect_fn = std::make_shared<FunctionValue>("tcp.connect", connect_impl, env, tok);
    obj->properties["connect"] = {Value{connect_fn}, false, false, true, tok};

    return obj;
}