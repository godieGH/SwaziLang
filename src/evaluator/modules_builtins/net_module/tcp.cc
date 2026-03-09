// tcp.cc - TCP client and server implementation using libuv
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "./net.hpp"

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
    if (std::holds_alternative<BufferPtr>(v))
        return std::get<BufferPtr>(v)->data;
    if (std::holds_alternative<std::string>(v)) {
        std::string s = std::get<std::string>(v);
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    return {};
}
}  // namespace NetHelpers

static Evaluator* g_evaluator = nullptr;

// ── Structs ──────────────────────────────────────────────────────────────────

struct TcpServerInstance {
    uv_tcp_t* server_handle = nullptr;
    FunctionPtr connection_handler;
    std::atomic<bool> closed{false};
    int port = 0;
    std::string host;
};

struct TcpSocketInstance {
    uv_tcp_t* socket_handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> reading{false};
    FunctionPtr on_data_handler;
    FunctionPtr on_close_handler;
    FunctionPtr on_error_handler;
    FunctionPtr on_connect_handler;
    std::string remote_address;
    int remote_port = 0;
    long long socket_id = 0;

    std::vector<FunctionPtr> drain_callbacks;
    std::mutex drain_mutex;
    static constexpr size_t WRITE_HIGH_WATERMARK = 16 * 1024;
    Evaluator* evaluator = nullptr;

    std::atomic<bool> paused{false};
};

// ResolveConnectData at file scope so try_next_address and on_address_connect
// can see it.
struct ResolveConnectData {
    std::shared_ptr<TcpSocketInstance> sock_inst;
    std::shared_ptr<ObjectValue> socket_obj;
    int port;
    uv_loop_t* loop;
    struct addrinfo* addrlist;
    struct addrinfo* current;
};

static std::mutex g_tcp_servers_mutex;
static std::unordered_map<long long, std::shared_ptr<TcpServerInstance>> g_tcp_servers;
static std::atomic<long long> g_next_tcp_server_id{1};

static std::mutex g_tcp_sockets_mutex;
static std::unordered_map<long long, std::shared_ptr<TcpSocketInstance>> g_tcp_sockets;
static std::atomic<long long> g_next_tcp_socket_id{1};

// ── start_reading_if_needed ───────────────────────────────────────────────────

static void start_reading_if_needed(TcpSocketInstance* inst) {
    if (!inst || !inst->socket_handle || inst->closed.load() || inst->paused.load())
        return;
    if (inst->reading.exchange(true))
        return;

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

            if (buf->base) delete[] buf->base;

            if (nread < 0) {
                inst->reading.store(false);
                {
                    std::lock_guard<std::mutex> lk(inst->drain_mutex);
                    inst->drain_callbacks.clear();
                }
                if (inst->on_close_handler) {
                    FunctionPtr handler = inst->on_close_handler;
                    CallbackPayload* payload = new CallbackPayload(handler, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                }
                if (!inst->closed.exchange(true)) {
                    uv_close((uv_handle_t*)stream, [](uv_handle_t* h) {
                        TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                        if (inst) {
                            g_active_tcp_work.fetch_sub(1);
                            std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                            g_tcp_sockets.erase(inst->socket_id);
                        }
                        delete (uv_tcp_t*)h;
                    });
                }
            }
        });
}

// ── Address walking (curl-style) ──────────────────────────────────────────────

static void try_next_address(ResolveConnectData* rdata);

static void on_address_connect(uv_connect_t* conn_req, int status) {
    ResolveConnectData* rdata = static_cast<ResolveConnectData*>(conn_req->data);
    delete conn_req;

    if (status == 0) {
        // success
        uv_freeaddrinfo(rdata->addrlist);
        auto inst = rdata->sock_inst;
        auto socket_obj = rdata->socket_obj;
        delete rdata;

        // reading starts only after user registers on("data")
        if (inst->on_connect_handler) {
            CallbackPayload* payload = new CallbackPayload(
                inst->on_connect_handler, {Value{socket_obj}});
            enqueue_callback_global(static_cast<void*>(payload));
        }
        return;
    }

    // this address failed — close handle and try next
    if (rdata->sock_inst->socket_handle) {
        uv_close((uv_handle_t*)rdata->sock_inst->socket_handle, [](uv_handle_t* h) {
            delete (uv_tcp_t*)h;
        });
        rdata->sock_inst->socket_handle = nullptr;
        rdata->sock_inst->closed.store(false);
    }

    rdata->current = rdata->current->ai_next;
    try_next_address(rdata);
}

static void try_next_address(ResolveConnectData* rdata) {
    // skip non-IP families
    while (rdata->current &&
        rdata->current->ai_family != AF_INET &&
        rdata->current->ai_family != AF_INET6) {
        rdata->current = rdata->current->ai_next;
    }

    if (!rdata->current) {
        // exhausted every address
        uv_freeaddrinfo(rdata->addrlist);
        auto inst = rdata->sock_inst;
        delete rdata;
        g_active_tcp_work.fetch_sub(1);
        if (inst->on_error_handler) {
            CallbackPayload* payload = new CallbackPayload(
                inst->on_error_handler,
                {Value{std::string("All addresses failed to connect")}});
            enqueue_callback_global(static_cast<void*>(payload));
        }
        return;
    }

    // copy sockaddr and stamp port
    struct sockaddr_storage sa{};
    memcpy(&sa, rdata->current->ai_addr, rdata->current->ai_addrlen);
    if (rdata->current->ai_family == AF_INET)
        reinterpret_cast<struct sockaddr_in*>(&sa)->sin_port = htons(rdata->port);
    else
        reinterpret_cast<struct sockaddr_in6*>(&sa)->sin6_port = htons(rdata->port);

    // fresh TCP handle for this attempt
    auto inst = rdata->sock_inst;
    inst->socket_handle = new uv_tcp_t;
    inst->socket_handle->data = inst.get();
    uv_tcp_init(rdata->loop, inst->socket_handle);

    uv_connect_t* conn_req = new uv_connect_t;
    conn_req->data = rdata;

    int cr = uv_tcp_connect(
        conn_req,
        inst->socket_handle,
        reinterpret_cast<const struct sockaddr*>(&sa),
        on_address_connect);

    if (cr != 0) {
        // synchronous failure — close handle and advance
        uv_close((uv_handle_t*)inst->socket_handle, [](uv_handle_t* h) {
            delete (uv_tcp_t*)h;
        });
        inst->socket_handle = nullptr;
        inst->closed.store(false);
        delete conn_req;
        rdata->current = rdata->current->ai_next;
        try_next_address(rdata);
    }
}

// ── Server accept callback ────────────────────────────────────────────────────

static void on_tcp_connection(uv_stream_t* server, int status) {
    if (status < 0) return;
    TcpServerInstance* srv = static_cast<TcpServerInstance*>(server->data);
    if (!srv || srv->closed.load()) return;

    uv_tcp_t* client = new uv_tcp_t;
    uv_tcp_init(server->loop, client);

    uv_os_fd_t fd;
    if (uv_fileno((uv_handle_t*)client, &fd) == 0) {
        int on = 1;
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
        signal(SIGPIPE, SIG_IGN);
    }

    if (uv_accept(server, (uv_stream_t*)client) != 0) {
        uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
        return;
    }

    auto sock_inst = std::make_shared<TcpSocketInstance>();
    sock_inst->socket_handle = client;
    sock_inst->closed = false;
    sock_inst->evaluator = g_evaluator;

    long long sock_id = g_next_tcp_socket_id.fetch_add(1);
    sock_inst->socket_id = sock_id;
    {
        std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
        g_tcp_sockets[sock_id] = sock_inst;
    }

    client->data = sock_inst.get();

    // remote address
    struct sockaddr_storage addr;
    int namelen = sizeof(addr);
    uv_tcp_getpeername(client, (struct sockaddr*)&addr, &namelen);
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in* a = (struct sockaddr_in*)&addr;
        char ip[INET_ADDRSTRLEN];
        uv_ip4_name(a, ip, sizeof(ip));
        sock_inst->remote_address = ip;
        sock_inst->remote_port = ntohs(a->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6* a = (struct sockaddr_in6*)&addr;
        char ip[INET6_ADDRSTRLEN];
        uv_ip6_name(a, ip, sizeof(ip));
        sock_inst->remote_address = ip;
        sock_inst->remote_port = ntohs(a->sin6_port);
    }

    auto socket_obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<tcp>", 0, 0, 0);

    // write
    auto write_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) return Value{false};
        if (sock_inst->closed.load() || !sock_inst->socket_handle)
            throw SwaziError("IOError", "Socket is closed", token.loc);

        std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
        if (data.empty()) return Value{false};

        struct WriteCtx {
            char* buf;
            std::shared_ptr<TcpSocketInstance> inst;
        };
        char* buf = static_cast<char*>(malloc(data.size()));
        memcpy(buf, data.data(), data.size());

        uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
        uv_write_t* req = new uv_write_t;
        req->data = new WriteCtx{buf, sock_inst};

        int r = uv_write(req, (uv_stream_t*)sock_inst->socket_handle, &uvbuf, 1,
            [](uv_write_t* req, int status) {
                auto* ctx = static_cast<WriteCtx*>(req->data);
                free(ctx->buf);
                auto inst = ctx->inst;
                delete ctx;
                delete req;
                if (status != 0) return;
                if (inst->socket_handle && inst->socket_handle->write_queue_size == 0) {
                    std::vector<FunctionPtr> cbs;
                    {
                        std::lock_guard<std::mutex> lk(inst->drain_mutex);
                        std::swap(cbs, inst->drain_callbacks);
                    }
                    if (!cbs.empty() && inst->evaluator) {
                        Token dtok;
                        dtok.loc = TokenLocation("<tcp>", 0, 0, 0);
                        for (auto& cb : cbs) {
                            try {
                                inst->evaluator->invoke_function(cb, {}, nullptr, dtok);
                            } catch (const SwaziError& e) { std::cerr << e.what() << std::endl; } catch (...) {
                            }
                        }
                    }
                }
            });

        if (r != 0) {
            auto* ctx = static_cast<WriteCtx*>(req->data);
            free(ctx->buf);
            delete ctx;
            delete req;
            if (sock_inst->on_error_handler) {
                auto err = std::string("Write failed: ") + uv_strerror(r);
                CallbackPayload* payload = new CallbackPayload(sock_inst->on_error_handler, {Value{err}});
                enqueue_callback_global(static_cast<void*>(payload));
            }
        }
        return Value{r == 0};
    };
    socket_obj->properties["write"] = {Value{std::make_shared<FunctionValue>("socket.write", write_impl, nullptr, tok)}, false, false, true, tok};

    // close
    auto close_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!sock_inst->closed.exchange(true) && sock_inst->socket_handle) {
            uv_close((uv_handle_t*)sock_inst->socket_handle, [](uv_handle_t* h) {
                TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                if (inst) {
                    std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                    g_tcp_sockets.erase(inst->socket_id);
                }
                delete (uv_tcp_t*)h;
            });
            sock_inst->socket_handle = nullptr;
        }
        return std::monostate{};
    };
    socket_obj->properties["close"] = {Value{std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, tok)}, false, false, true, tok};

    // isOpen
    auto is_open_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{!sock_inst->closed.load() && sock_inst->socket_handle != nullptr};
    };
    socket_obj->properties["isOpen"] = {Value{std::make_shared<FunctionValue>("socket.isOpen", is_open_impl, nullptr, tok)}, false, false, true, tok};

    // writableNeedsDrain
    auto needs_drain_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (sock_inst->closed.load() || !sock_inst->socket_handle) return Value{false};
        return Value{sock_inst->socket_handle->write_queue_size >= TcpSocketInstance::WRITE_HIGH_WATERMARK};
    };
    socket_obj->properties["writableNeedsDrain"] = {Value{std::make_shared<FunctionValue>("socket.writableNeedsDrain", needs_drain_impl, nullptr, tok)}, false, false, true, tok};

    // pause
    auto pause_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!sock_inst->closed.load() && sock_inst->socket_handle && !sock_inst->paused.exchange(true))
            uv_read_stop((uv_stream_t*)sock_inst->socket_handle);
        return std::monostate{};
    };
    socket_obj->properties["pause"] = {Value{std::make_shared<FunctionValue>("socket.pause", pause_impl, nullptr, tok)}, false, false, true, tok};

    // resume
    auto resume_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!sock_inst->closed.load() && sock_inst->socket_handle && sock_inst->paused.exchange(false)) {
            sock_inst->reading.store(false);
            start_reading_if_needed(sock_inst.get());
        }
        return std::monostate{};
    };
    socket_obj->properties["resume"] = {Value{std::make_shared<FunctionValue>("socket.resume", resume_impl, nullptr, tok)}, false, false, true, tok};

    // on
    auto socket_weak = std::weak_ptr<ObjectValue>(socket_obj);
    auto on_impl = [sock_inst, socket_weak](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        auto socket_obj = socket_weak.lock();
        if (!socket_obj) return std::monostate{};
        if (args.size() < 2)
            throw SwaziError("TypeError", "on() requires event name and handler", token.loc);
        std::string event = NetHelpers::value_to_string(args[0]);
        if (!std::holds_alternative<FunctionPtr>(args[1]))
            throw SwaziError("TypeError", "Handler must be a function", token.loc);
        FunctionPtr handler = std::get<FunctionPtr>(args[1]);
        if (event == "data") {
            sock_inst->on_data_handler = handler;
            start_reading_if_needed(sock_inst.get());
        } else if (event == "drain") {
            std::lock_guard<std::mutex> lk(sock_inst->drain_mutex);
            sock_inst->drain_callbacks.push_back(handler);
        } else if (event == "close") {
            sock_inst->on_close_handler = handler;
        } else if (event == "error") {
            sock_inst->on_error_handler = handler;
        } else {
            throw SwaziError("TypeError", "Unknown event: " + event, token.loc);
        }
        return Value{socket_obj};
    };
    socket_obj->properties["on"] = {Value{std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, tok)}, false, false, true, tok};

    socket_obj->properties["remoteAddress"] = {Value{sock_inst->remote_address}, false, false, true, tok};
    socket_obj->properties["remotePort"] = {Value{static_cast<double>(sock_inst->remote_port)}, false, false, true, tok};

    if (srv->connection_handler) {
        CallbackPayload* payload = new CallbackPayload(srv->connection_handler, {Value{socket_obj}});
        enqueue_callback_global(static_cast<void*>(payload));
    }
}

// ── Exports ───────────────────────────────────────────────────────────────────

std::shared_ptr<ObjectValue> make_tcp_exports(EnvPtr env, Evaluator* evaluator) {
    struct sigaction sa{};
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGPIPE, &sa, nullptr);

    auto obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<tcp>", 0, 0, 0);

    g_evaluator = evaluator;

    // ── createServer ──────────────────────────────────────────────────────────
    auto createServer_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0]))
            throw SwaziError("TypeError", "createServer requires a connection handler", token.loc);

        auto inst = std::make_shared<TcpServerInstance>();
        inst->connection_handler = std::get<FunctionPtr>(args[0]);

        long long id = g_next_tcp_server_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(g_tcp_servers_mutex);
            g_tcp_servers[id] = inst;
        }

        auto server_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<tcp>", 0, 0, 0);

        // listen
        auto listen_impl = [inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) throw SwaziError("TypeError", "listen requires port", token.loc);

            int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            std::string host = "0.0.0.0";
            FunctionPtr cb = nullptr;

            if (args.size() >= 2 && std::holds_alternative<std::string>(args[1]))
                host = std::get<std::string>(args[1]);
            if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2]))
                cb = std::get<FunctionPtr>(args[2]);
            else if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1]))
                cb = std::get<FunctionPtr>(args[1]);

            inst->port = port;
            inst->host = host;

            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);

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
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {})));
                    } else {
                        auto err = std::string("Listen failed: ") + uv_strerror(r);
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {Value{err}})));
                    }
                }
            });
            return std::monostate{};
        };
        server_obj->properties["listen"] = {Value{std::make_shared<FunctionValue>("server.listen", listen_impl, nullptr, stok)}, false, false, true, stok};

        // server close
        auto close_impl = [inst, id](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            FunctionPtr cb = (!args.empty() && std::holds_alternative<FunctionPtr>(args[0]))
                ? std::get<FunctionPtr>(args[0])
                : nullptr;
            inst->closed.store(true);
            scheduler_run_on_loop([inst, cb, id]() {
                if (inst->server_handle) {
                    uv_close((uv_handle_t*)inst->server_handle, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                    inst->server_handle = nullptr;
                }
                {
                    std::lock_guard<std::mutex> lk(g_tcp_servers_mutex);
                    g_tcp_servers.erase(id);
                }
                if (cb) enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {})));
            });
            return std::monostate{};
        };
        server_obj->properties["close"] = {Value{std::make_shared<FunctionValue>("server.close", close_impl, nullptr, stok)}, false, false, true, stok};

        return Value{server_obj};
    };
    obj->properties["createServer"] = {Value{std::make_shared<FunctionValue>("tcp.createServer", createServer_impl, env, tok)}, false, false, true, tok};

    // ── connect ───────────────────────────────────────────────────────────────
    auto connect_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty())
            throw SwaziError("TypeError", "connect requires port", token.loc);

        int port = 0;
        std::string host = "127.0.0.1";
        FunctionPtr cb = nullptr;

        if (std::holds_alternative<ObjectPtr>(args[0])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[0]);
            auto pit = opts->properties.find("port");
            if (pit != opts->properties.end())
                port = static_cast<int>(NetHelpers::value_to_number(pit->second.value));
            auto hit = opts->properties.find("host");
            if (hit != opts->properties.end())
                host = NetHelpers::value_to_string(hit->second.value);
            if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1]))
                cb = std::get<FunctionPtr>(args[1]);
        } else {
            port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            if (args.size() >= 2 && std::holds_alternative<std::string>(args[1]))
                host = std::get<std::string>(args[1]);
            if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2]))
                cb = std::get<FunctionPtr>(args[2]);
            else if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1]))
                cb = std::get<FunctionPtr>(args[1]);
        }

        if (port < 1 || port > 65535)
            throw SwaziError("TypeError", "Invalid port number", token.loc);

        g_active_tcp_work.fetch_add(1);

        auto sock_inst = std::make_shared<TcpSocketInstance>();
        sock_inst->on_connect_handler = cb;
        sock_inst->evaluator = g_evaluator;

        long long sock_id = g_next_tcp_socket_id.fetch_add(1);
        sock_inst->socket_id = sock_id;
        {
            std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
            g_tcp_sockets[sock_id] = sock_inst;
        }

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            g_active_tcp_work.fetch_sub(1);
            throw SwaziError("RuntimeError", "No event loop available", token.loc);
        }

        auto socket_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<tcp>", 0, 0, 0);

        // write
        auto write_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return Value{false};
            if (sock_inst->closed.load() || !sock_inst->socket_handle)
                throw SwaziError("IOError", "Socket is closed", token.loc);

            std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
            if (data.empty()) return Value{false};

            struct WriteCtx {
                char* buf;
                std::shared_ptr<TcpSocketInstance> inst;
            };
            char* buf = static_cast<char*>(malloc(data.size()));
            memcpy(buf, data.data(), data.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
            uv_write_t* req = new uv_write_t;
            req->data = new WriteCtx{buf, sock_inst};

            int r = uv_write(req, (uv_stream_t*)sock_inst->socket_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    auto* ctx = static_cast<WriteCtx*>(req->data);
                    free(ctx->buf);
                    auto inst = ctx->inst;
                    delete ctx;
                    delete req;
                    if (status != 0) return;
                    if (inst->socket_handle && inst->socket_handle->write_queue_size == 0) {
                        std::vector<FunctionPtr> cbs;
                        {
                            std::lock_guard<std::mutex> lk(inst->drain_mutex);
                            std::swap(cbs, inst->drain_callbacks);
                        }
                        if (!cbs.empty() && inst->evaluator) {
                            Token dtok;
                            dtok.loc = TokenLocation("<tcp>", 0, 0, 0);
                            for (auto& cb : cbs) {
                                try {
                                    inst->evaluator->invoke_function(cb, {}, nullptr, dtok);
                                } catch (const SwaziError& e) { std::cerr << e.what() << std::endl; } catch (...) {
                                }
                            }
                        }
                    }
                });

            if (r != 0) {
                auto* ctx = static_cast<WriteCtx*>(req->data);
                free(ctx->buf);
                delete ctx;
                delete req;
                if (sock_inst->on_error_handler) {
                    auto err = std::string("Write failed: ") + uv_strerror(r);
                    enqueue_callback_global(static_cast<void*>(new CallbackPayload(sock_inst->on_error_handler, {Value{err}})));
                }
            }
            return Value{r == 0};
        };
        socket_obj->properties["write"] = {Value{std::make_shared<FunctionValue>("socket.write", write_impl, nullptr, stok)}, false, false, true, stok};

        // close
        auto close_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.exchange(true) && sock_inst->socket_handle) {
                uv_close((uv_handle_t*)sock_inst->socket_handle, [](uv_handle_t* h) {
                    TcpSocketInstance* inst = static_cast<TcpSocketInstance*>(h->data);
                    if (inst) {
                        g_active_tcp_work.fetch_sub(1);
                        std::lock_guard<std::mutex> lk(g_tcp_sockets_mutex);
                        g_tcp_sockets.erase(inst->socket_id);
                    }
                    delete (uv_tcp_t*)h;
                });
                sock_inst->socket_handle = nullptr;
            }
            return std::monostate{};
        };
        socket_obj->properties["close"] = {Value{std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, stok)}, false, false, true, stok};

        // isOpen
        auto is_open_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            return Value{!sock_inst->closed.load() && sock_inst->socket_handle != nullptr};
        };
        socket_obj->properties["isOpen"] = {Value{std::make_shared<FunctionValue>("socket.isOpen", is_open_impl, nullptr, stok)}, false, false, true, stok};

        // writableNeedsDrain
        auto needs_drain_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (sock_inst->closed.load() || !sock_inst->socket_handle) return Value{false};
            return Value{sock_inst->socket_handle->write_queue_size >= TcpSocketInstance::WRITE_HIGH_WATERMARK};
        };
        socket_obj->properties["writableNeedsDrain"] = {Value{std::make_shared<FunctionValue>("socket.writableNeedsDrain", needs_drain_impl, nullptr, stok)}, false, false, true, stok};

        // pause
        auto pause_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.load() && sock_inst->socket_handle && !sock_inst->paused.exchange(true))
                uv_read_stop((uv_stream_t*)sock_inst->socket_handle);
            return std::monostate{};
        };
        socket_obj->properties["pause"] = {Value{std::make_shared<FunctionValue>("socket.pause", pause_impl, nullptr, stok)}, false, false, true, stok};

        // resume
        auto resume_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.load() && sock_inst->socket_handle && sock_inst->paused.exchange(false)) {
                sock_inst->reading.store(false);
                start_reading_if_needed(sock_inst.get());
            }
            return std::monostate{};
        };
        socket_obj->properties["resume"] = {Value{std::make_shared<FunctionValue>("socket.resume", resume_impl, nullptr, stok)}, false, false, true, stok};

        // on
        auto socket_weak = std::weak_ptr<ObjectValue>(socket_obj);
        auto on_impl = [sock_inst, socket_weak](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            auto socket_obj = socket_weak.lock();
            if (!socket_obj) return std::monostate{};
            if (args.size() < 2)
                throw SwaziError("TypeError", "on() requires event name and handler", token.loc);
            std::string event = NetHelpers::value_to_string(args[0]);
            if (!std::holds_alternative<FunctionPtr>(args[1]))
                throw SwaziError("TypeError", "Handler must be a function", token.loc);
            FunctionPtr handler = std::get<FunctionPtr>(args[1]);
            if (event == "data") {
                sock_inst->on_data_handler = handler;
                start_reading_if_needed(sock_inst.get());  // start reading once handler is set
            } else if (event == "drain") {
                std::lock_guard<std::mutex> lk(sock_inst->drain_mutex);
                sock_inst->drain_callbacks.push_back(handler);
            } else if (event == "close") {
                sock_inst->on_close_handler = handler;
            } else if (event == "error") {
                sock_inst->on_error_handler = handler;
            } else if (event == "connect") {
                sock_inst->on_connect_handler = handler;
            } else {
                throw SwaziError("TypeError", "Unknown event: " + event, token.loc);
            }
            return Value{socket_obj};
        };
        socket_obj->properties["on"] = {Value{std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, stok)}, false, false, true, stok};

        // ── DNS + connect ─────────────────────────────────────────────────────

        uv_getaddrinfo_t* resolve_req = new uv_getaddrinfo_t;
        resolve_req->data = new ResolveConnectData{sock_inst, socket_obj, port, loop, nullptr, nullptr};

        struct addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = 0;

        int r = uv_getaddrinfo(
            loop,
            resolve_req,
            [](uv_getaddrinfo_t* resolve_req, int status, struct addrinfo* res) {
                ResolveConnectData* rdata = static_cast<ResolveConnectData*>(resolve_req->data);
                delete resolve_req;

                if (status < 0 || !res) {
                    g_active_tcp_work.fetch_sub(1);
                    auto inst = rdata->sock_inst;
                    delete rdata;
                    if (inst->on_error_handler) {
                        auto err = std::string("DNS resolution failed: ") + uv_strerror(status);
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler, {Value{err}})));
                    }
                    if (res) uv_freeaddrinfo(res);
                    return;
                }

                rdata->addrlist = res;
                rdata->current = res;
                try_next_address(rdata);
            },
            host.c_str(),
            nullptr,
            &hints);

        if (r != 0) {
            delete static_cast<ResolveConnectData*>(resolve_req->data);
            delete resolve_req;
            g_active_tcp_work.fetch_sub(1);
            if (sock_inst->on_error_handler) {
                auto err = std::string("DNS resolve initiation failed: ") + uv_strerror(r);
                enqueue_callback_global(static_cast<void*>(new CallbackPayload(sock_inst->on_error_handler, {Value{err}})));
            }
        }

        return Value{socket_obj};
    };
    obj->properties["connect"] = {Value{std::make_shared<FunctionValue>("tcp.connect", connect_impl, env, tok)}, false, false, true, tok};

    return obj;
}
