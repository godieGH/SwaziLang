// udp.cc - UDP socket implementation using libuv
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "./net.hpp"

// Work counter for event loop tracking
static std::atomic<int> g_active_udp_work{0};

// Export function for event loop checking
bool udp_has_active_work() {
    return g_active_udp_work.load() > 0;
}

// UDP Socket instance
struct UdpSocketInstance {
    uv_udp_t* udp_handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> work_counted{false};  // Track if work counter was incremented
    FunctionPtr on_message_handler;
    FunctionPtr on_error_handler;
    FunctionPtr on_close_handler;
    std::string bound_address;
    int bound_port = 0;
    std::string socket_type;  // "udp4" or "udp6"

    ~UdpSocketInstance() {
        // Safety: ensure work counter is decremented on destruction
        if (work_counted.exchange(false)) {
            g_active_udp_work.fetch_sub(1);
        }
    }
};

static std::mutex g_udp_sockets_mutex;
static std::unordered_map<long long, std::shared_ptr<UdpSocketInstance>> g_udp_sockets;
static std::atomic<long long> g_next_udp_socket_id{1};

// Allocation callback for UDP receives
static void udp_alloc_cb(uv_handle_t* handle, size_t suggested, uv_buf_t* buf) {
    buf->base = new char[suggested];
    buf->len = (unsigned int)suggested;
}

// UDP receive callback
static void udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
    const struct sockaddr* addr, unsigned flags) {
    UdpSocketInstance* inst = static_cast<UdpSocketInstance*>(handle->data);

    if (nread == UV_ECONNREFUSED || nread < 0) {
        uv_os_fd_t fd;
        if (uv_fileno((uv_handle_t*)handle, &fd) == 0) {
            char errbuf[512];
            struct msghdr msg = {};
            recvmsg(fd, &msg, MSG_ERRQUEUE);  // drain ICMP error
        }

        if (inst && inst->on_error_handler) {
            std::string err_msg;
            if (nread == UV_ECONNREFUSED)
                err_msg = "Connection refused: peer unreachable";
            else
                err_msg = std::string("UDP receive error: ") + uv_strerror((int)nread);

            FunctionPtr handler = inst->on_error_handler;
            CallbackPayload* payload = new CallbackPayload(handler, {Value{err_msg}});
            enqueue_callback_global(static_cast<void*>(payload));
        }

        delete[] buf->base;
        return;  // don't fall through to the generic nread < 0 handler below
    }

    if (nread > 0 && inst && inst->on_message_handler && addr) {
        // Extract sender info
        std::string sender_addr;
        int sender_port = 0;

        if (addr->sa_family == AF_INET) {
            struct sockaddr_in* addr_in = (struct sockaddr_in*)addr;
            char ip[INET_ADDRSTRLEN];
            uv_ip4_name(addr_in, ip, sizeof(ip));
            sender_addr = ip;
            sender_port = ntohs(addr_in->sin_port);
        } else if (addr->sa_family == AF_INET6) {
            struct sockaddr_in6* addr_in6 = (struct sockaddr_in6*)addr;
            if (IN6_IS_ADDR_V4MAPPED(&addr_in6->sin6_addr)) {
                struct sockaddr_in mapped{};
                mapped.sin_family = AF_INET;
                memcpy(&mapped.sin_addr, addr_in6->sin6_addr.s6_addr + 12, 4);
                char ip[INET_ADDRSTRLEN];
                uv_ip4_name(&mapped, ip, sizeof(ip));
                sender_addr = ip;
                sender_port = ntohs(addr_in6->sin6_port);
            } else {
                char ip[INET6_ADDRSTRLEN];
                uv_ip6_name(addr_in6, ip, sizeof(ip));
                sender_addr = ip;
                sender_port = ntohs(addr_in6->sin6_port);
            }
        }

        // Create buffer with received data
        auto buffer = std::make_shared<BufferValue>();
        buffer->data.assign(buf->base, buf->base + nread);
        buffer->encoding = "binary";

        // Create rinfo object
        auto rinfo = std::make_shared<ObjectValue>();
        Token tok;
        tok.loc = TokenLocation("<udp>", 0, 0, 0);

        rinfo->properties["address"] = {Value{sender_addr}, false, false, true, tok};
        rinfo->properties["port"] = {Value{static_cast<double>(sender_port)}, false, false, true, tok};
        rinfo->properties["family"] = {
            Value{sender_addr.find(':') == std::string::npos ? std::string("IPv4") : std::string("IPv6")},
            false, false, true, tok};
        rinfo->properties["size"] = {Value{static_cast<double>(nread)}, false, false, true, tok};

        // Call handler with (message, rinfo)
        FunctionPtr handler = inst->on_message_handler;
        CallbackPayload* payload = new CallbackPayload(handler, {Value{buffer}, Value{rinfo}});
        enqueue_callback_global(static_cast<void*>(payload));
    }

    delete[] buf->base;
}

std::shared_ptr<ObjectValue> make_udp_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<udp>", 0, 0, 0);

    // udp.createSocket(type, callback?)
    // type: 'udp4' or 'udp6'
    auto createSocket_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        std::string type = "udp4";
        FunctionPtr cb = nullptr;

        if (!args.empty()) {
            if (std::holds_alternative<std::string>(args[0])) {
                type = std::get<std::string>(args[0]);
            } else if (std::holds_alternative<ObjectPtr>(args[0])) {
                // Options object
                ObjectPtr opts = std::get<ObjectPtr>(args[0]);
                auto type_prop = opts->properties.find("type");
                if (type_prop != opts->properties.end()) {
                    type = NetHelpers::value_to_string(type_prop->second.value);
                }
            }
        }

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            cb = std::get<FunctionPtr>(args[1]);
        }

        if (type != "udp4" && type != "udp6") {
            throw SwaziError("TypeError", "Socket type must be 'udp4' or 'udp6'", token.loc);
        }

        auto inst = std::make_shared<UdpSocketInstance>();
        inst->socket_type = type;
        long long sock_id = g_next_udp_socket_id.fetch_add(1);

        // Increment work counter and mark it
        g_active_udp_work.fetch_add(1);
        inst->work_counted.store(true);

        {
            std::lock_guard<std::mutex> lk(g_udp_sockets_mutex);
            g_udp_sockets[sock_id] = inst;
        }

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            // Safety: clean up on error
            if (inst->work_counted.exchange(false)) {
                g_active_udp_work.fetch_sub(1);
            }
            {
                std::lock_guard<std::mutex> lk(g_udp_sockets_mutex);
                g_udp_sockets.erase(sock_id);
            }
            throw SwaziError("RuntimeError", "No event loop available", token.loc);
        }

        // Create socket object
        auto socket_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<udp>", 0, 0, 0);

        // Initialize UDP handle with error handling
        scheduler_run_on_loop([inst, type, loop, sock_id]() {
            inst->udp_handle = new uv_udp_t;
            inst->udp_handle->data = inst.get();

            unsigned int flags = (type == "udp6") ? AF_INET6 : AF_INET;
            int r = uv_udp_init_ex(loop, inst->udp_handle, flags);

            if (r == 0) {
                // Enable ICMP error reporting
                uv_os_fd_t fd;
                if (uv_fileno((uv_handle_t*)inst->udp_handle, &fd) == 0) {
                    int one = 1;
                    if (type == "udp6")
                        setsockopt(fd, IPPROTO_IPV6, IPV6_RECVERR, &one, sizeof(one));
                    else
                        setsockopt(fd, IPPROTO_IP, IP_RECVERR, &one, sizeof(one));
                }
            }

            if (r != 0) {
                // Init failed - clean up
                delete inst->udp_handle;
                inst->udp_handle = nullptr;
                inst->closed.store(true);

                // Decrement work counter
                if (inst->work_counted.exchange(false)) {
                    g_active_udp_work.fetch_sub(1);
                }

                // Remove from map
                std::lock_guard<std::mutex> lk(g_udp_sockets_mutex);
                g_udp_sockets.erase(sock_id);
            }
        });

        // socket.bind(port, address?, callback?)
        auto bind_impl = [inst, sock_id, loop](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "bind requires port", token.loc);
            }

            int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            std::string address = (inst->socket_type == "udp6") ? "::" : "0.0.0.0";
            FunctionPtr cb = nullptr;

            if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
                address = std::get<std::string>(args[1]);
            }

            if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2])) {
                cb = std::get<FunctionPtr>(args[2]);
            } else if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                cb = std::get<FunctionPtr>(args[1]);
            }

            if (port < 0 || port > 65535) {
                throw SwaziError("TypeError", "Invalid port number", token.loc);
            }

            inst->bound_port = port;
            inst->bound_address = address;

            scheduler_run_on_loop([inst, port, address, cb, loop]() {
                if (!inst->udp_handle) {
                    if (cb) {
                        auto err_msg = std::string("Bind failed: socket not initialized");
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {Value{err_msg}})));
                    }
                    return;
                }

                // Try parse as IP literal first — no DNS cost
                struct sockaddr_storage addr{};
                struct sockaddr_in a4{};
                struct sockaddr_in6 a6{};
                bool parsed = false;

                if (uv_ip4_addr(address.c_str(), port, &a4) == 0) {
                    if (inst->socket_type == "udp6") {
                        if (cb) enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                            {Value{std::string("Cannot bind IPv4 address on udp6 socket")}})));
                        return;
                    }
                    memcpy(&addr, &a4, sizeof(a4));
                    parsed = true;
                } else if (uv_ip6_addr(address.c_str(), port, &a6) == 0) {
                    if (inst->socket_type == "udp4") {
                        if (cb) enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                            {Value{std::string("Cannot bind IPv6 address on udp4 socket")}})));
                        return;
                    }
                    memcpy(&addr, &a6, sizeof(a6));
                    parsed = true;
                }

                if (parsed) {
                    int r = uv_udp_bind(inst->udp_handle, (const struct sockaddr*)&addr, UV_UDP_REUSEADDR);
                    if (cb) {
                        if (r == 0)
                            enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {})));
                        else
                            enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                                {Value{std::string("Bind failed: ") + uv_strerror(r)}})));
                    }
                    return;
                }

                // Not an IP literal — resolve via DNS (bind happens once, acceptable cost)
                struct BindResolveCtx {
                    std::shared_ptr<UdpSocketInstance> inst;
                    int port;
                    FunctionPtr cb;
                };
                auto* rctx = new BindResolveCtx{inst, port, cb};
                auto* resolve_req = new uv_getaddrinfo_t;
                resolve_req->data = rctx;

                struct addrinfo hints{};
                hints.ai_family = (inst->socket_type == "udp6") ? AF_INET6 : AF_INET;
                hints.ai_socktype = SOCK_DGRAM;

                int gr = uv_getaddrinfo(loop, resolve_req, [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
                        auto* rctx = static_cast<BindResolveCtx*>(req->data);
                        delete req;

                        if (status < 0 || !res) {
                            if (rctx->cb)
                                enqueue_callback_global(static_cast<void*>(new CallbackPayload(rctx->cb,
                                    {Value{std::string("DNS resolution failed: ") + uv_strerror(status)}})));
                            delete rctx;
                            if (res) uv_freeaddrinfo(res);
                            return;
                        }

                        struct sockaddr_storage addr{};
                        memcpy(&addr, res->ai_addr, res->ai_addrlen);
                        if (addr.ss_family == AF_INET)
                            reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port = htons(rctx->port);
                        else
                            reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port = htons(rctx->port);
                        uv_freeaddrinfo(res);

                        int r = uv_udp_bind(rctx->inst->udp_handle, (const struct sockaddr*)&addr, UV_UDP_REUSEADDR);
                        if (rctx->cb) {
                            if (r == 0)
                                enqueue_callback_global(static_cast<void*>(new CallbackPayload(rctx->cb, {})));
                            else
                                enqueue_callback_global(static_cast<void*>(new CallbackPayload(rctx->cb,
                                    {Value{std::string("Bind failed: ") + uv_strerror(r)}})));
                        }
                        delete rctx; }, address.c_str(), nullptr, &hints);

                if (gr != 0) {
                    if (cb)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                            {Value{std::string("DNS resolve failed: ") + uv_strerror(gr)}})));
                    delete static_cast<BindResolveCtx*>(resolve_req->data);
                    delete resolve_req;
                }
            });
            return std::monostate{};
        };
        auto bind_fn = std::make_shared<FunctionValue>("socket.bind", bind_impl, nullptr, stok);
        socket_obj->properties["bind"] = {Value{bind_fn}, false, false, true, stok};

        // socket.send(buffer, port, address, callback?)
        auto send_impl = [inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError", "send requires (buffer, port, address)", token.loc);
            }

            std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
            int port = static_cast<int>(NetHelpers::value_to_number(args[1]));
            std::string address = NetHelpers::value_to_string(args[2]);
            FunctionPtr cb = (args.size() >= 4 && std::holds_alternative<FunctionPtr>(args[3]))
                ? std::get<FunctionPtr>(args[3])
                : nullptr;

            if (data.empty()) return std::monostate{};

            // Add size validation
            const size_t MAX_UDP_PAYLOAD = 65507;
            if (data.size() > MAX_UDP_PAYLOAD) {
                if (inst->on_error_handler) {
                    auto err_msg = std::string("UDP payload size (") +
                        std::to_string(data.size()) +
                        " bytes) exceeds maximum of " +
                        std::to_string(MAX_UDP_PAYLOAD) + " bytes";
                    enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler, {Value{err_msg}})));
                }
                if (cb) {
                    enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {Value{std::string("Message too large")}})));
                }
                return std::monostate{};
            }

            const size_t SAFE_UDP_SIZE = 1472;
            if (data.size() > SAFE_UDP_SIZE && inst->on_error_handler) {
                auto warning = std::string("Warning: Large UDP packet (") +
                    std::to_string(data.size()) +
                    " bytes) may be fragmented. Consider splitting data.";
                enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler, {Value{warning}})));
            }

            scheduler_run_on_loop([inst, data, port, address, cb]() {
                if (!inst->udp_handle) {
                    if (inst->on_error_handler)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler,
                            {Value{std::string("Socket not initialized")}})));
                    if (cb)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                            {Value{std::string("Send failed: socket not initialized")}})));
                    return;
                }

                // Try both parsers — no string inspection, let inet_pton decide
                struct sockaddr_storage addr_storage{};
                struct sockaddr_in a4{};
                struct sockaddr_in6 a6{};

                if (uv_ip4_addr(address.c_str(), port, &a4) == 0) {
                    if (inst->socket_type == "udp6") {
                        // dual-stack: convert IPv4 to IPv4-mapped IPv6
                        memset(&a6, 0, sizeof(a6));
                        a6.sin6_family = AF_INET6;
                        a6.sin6_port = htons(port);
                        a6.sin6_addr.s6_addr[10] = 0xff;
                        a6.sin6_addr.s6_addr[11] = 0xff;
                        memcpy(&a6.sin6_addr.s6_addr[12], &a4.sin_addr, 4);
                        memcpy(&addr_storage, &a6, sizeof(a6));
                    } else {
                        memcpy(&addr_storage, &a4, sizeof(a4));
                    }
                } else if (uv_ip6_addr(address.c_str(), port, &a6) == 0) {
                    if (inst->socket_type == "udp4") {
                        if (inst->on_error_handler)
                            enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler,
                                {Value{std::string("Cannot send to IPv6 address on udp4 socket")}})));
                        if (cb)
                            enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                                {Value{std::string("Family mismatch")}})));
                        return;
                    }
                    memcpy(&addr_storage, &a6, sizeof(a6));
                } else {
                    // Not a valid IP literal — hostnames are not allowed in send()
                    if (inst->on_error_handler)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler,
                            {Value{std::string("send() requires an IP address, not a hostname")}})));
                    if (cb)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                            {Value{std::string("Invalid address")}})));
                    return;
                }

                char* buf = static_cast<char*>(malloc(data.size()));
                if (!buf) {
                    if (inst->on_error_handler)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler,
                            {Value{std::string("Memory allocation failed")}})));
                    if (cb)
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb,
                            {Value{std::string("Memory allocation failed")}})));
                    return;
                }

                memcpy(buf, data.data(), data.size());

                uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
                uv_udp_send_t* req = new uv_udp_send_t;

                struct SendContext {
                    char* buffer;
                    FunctionPtr callback;
                    FunctionPtr error_handler;
                    UdpSocketInstance* instance;
                };

                auto* ctx = new SendContext{buf, cb, inst->on_error_handler, inst.get()};
                req->data = ctx;

                int result = uv_udp_send(req, inst->udp_handle, &uvbuf, 1,
                    (const struct sockaddr*)&addr_storage,
                    [](uv_udp_send_t* req, int status) {
                        auto* ctx = static_cast<SendContext*>(req->data);

                        if (status != 0) {
                            if (ctx->error_handler) {
                                auto err_msg = std::string("Send failed: ") + uv_strerror(status);
                                enqueue_callback_global(static_cast<void*>(new CallbackPayload(ctx->error_handler, {Value{err_msg}})));
                            }
                            if (ctx->callback) {
                                enqueue_callback_global(static_cast<void*>(new CallbackPayload(ctx->callback, {Value{std::string("Send failed")}})));
                            }
                        } else {
                            if (ctx->callback) {
                                enqueue_callback_global(static_cast<void*>(new CallbackPayload(ctx->callback, {})));
                            }
                        }

                        if (ctx->buffer) free(ctx->buffer);
                        delete ctx;
                        delete req;
                    });

                if (result != 0) {
                    if (inst->on_error_handler) {
                        auto err_msg = std::string("Send initiation failed: ") + uv_strerror(result);
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(inst->on_error_handler, {Value{err_msg}})));
                    }
                    if (cb) {
                        enqueue_callback_global(static_cast<void*>(new CallbackPayload(cb, {Value{std::string("Send failed")}})));
                    }
                    free(buf);
                    delete ctx;
                    delete req;
                }
            });
            return std::monostate{};
        };
        auto send_fn = std::make_shared<FunctionValue>("socket.send", send_impl, nullptr, stok);
        socket_obj->properties["send"] = {Value{send_fn}, false, false, true, stok};

        // socket.on(event, handler)
        auto socket_weak = std::weak_ptr<ObjectValue>(socket_obj);
        auto on_impl = [inst, socket_weak](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            auto socket_obj = socket_weak.lock();
            if (!socket_obj) return std::monostate{};

            if (args.size() < 2) {
                throw SwaziError("TypeError", "on() requires event name and handler", token.loc);
            }

            std::string event = NetHelpers::value_to_string(args[0]);
            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "Handler must be a function", token.loc);
            }

            FunctionPtr handler = std::get<FunctionPtr>(args[1]);

            if (event == "message") {
                inst->on_message_handler = handler;

                // Start receiving
                scheduler_run_on_loop([inst]() {
                    if (inst->udp_handle && !inst->closed.load()) {
                        uv_udp_recv_start(inst->udp_handle, udp_alloc_cb, udp_recv_cb);
                    }
                });
            } else if (event == "error") {
                inst->on_error_handler = handler;
            } else if (event == "close") {
                inst->on_close_handler = handler;
            } else {
                std::ostringstream ss;
                ss << "Unknown event name: " << event;
                throw SwaziError("TypeError", ss.str(), token.loc);
            }

            return Value{socket_obj};
        };
        auto on_fn = std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, stok);
        socket_obj->properties["on"] = {Value{on_fn}, false, false, true, stok};

        auto is_open_impl = [inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            return Value{!inst->closed.load() && inst->udp_handle != nullptr};
        };
        auto is_open_fn = std::make_shared<FunctionValue>("socket.isOpen", is_open_impl, nullptr, stok);
        socket_obj->properties["isOpen"] = {Value{is_open_fn}, false, false, true, stok};

        // socket.close(callback?)
        auto close_impl = [inst, sock_id](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            FunctionPtr cb = (!args.empty() && std::holds_alternative<FunctionPtr>(args[0]))
                ? std::get<FunctionPtr>(args[0])
                : nullptr;

            // Only proceed if not already closed
            if (!inst->closed.exchange(true)) {
                scheduler_run_on_loop([inst, cb, sock_id]() {
                    if (inst->udp_handle) {
                        uv_udp_recv_stop(inst->udp_handle);

                        // Close handle and decrement work counter in close callback
                        uv_close((uv_handle_t*)inst->udp_handle, [](uv_handle_t* h) {
                            UdpSocketInstance* inst = static_cast<UdpSocketInstance*>(h->data);
                            if (inst && inst->work_counted.exchange(false)) {
                                g_active_udp_work.fetch_sub(1);
                            }

                            // Call close handler if set
                            if (inst && inst->on_close_handler) {
                                FunctionPtr handler = inst->on_close_handler;
                                CallbackPayload* payload = new CallbackPayload(handler, {});
                                enqueue_callback_global(static_cast<void*>(payload));
                            }

                            delete (uv_udp_t*)h;
                        });
                        inst->udp_handle = nullptr;
                    } else {
                        // Handle was never created or already destroyed
                        if (inst->work_counted.exchange(false)) {
                            g_active_udp_work.fetch_sub(1);
                        }
                    }

                    if (cb) {
                        CallbackPayload* payload = new CallbackPayload(cb, {});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                });

                // Remove from map
                {
                    std::lock_guard<std::mutex> lk(g_udp_sockets_mutex);
                    g_udp_sockets.erase(sock_id);
                }
            }

            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, stok);
        socket_obj->properties["close"] = {Value{close_fn}, false, false, true, stok};

        // socket.address() -> {address, port, family}
        auto address_impl = [inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto info = std::make_shared<ObjectValue>();
            Token tok;
            tok.loc = TokenLocation("<udp>", 0, 0, 0);

            if (inst->udp_handle && !inst->closed.load()) {
                struct sockaddr_storage addr;
                int namelen = sizeof(addr);
                int r = uv_udp_getsockname(inst->udp_handle, (struct sockaddr*)&addr, &namelen);

                if (r == 0) {
                    if (addr.ss_family == AF_INET) {
                        struct sockaddr_in* addr_in = (struct sockaddr_in*)&addr;
                        char ip[INET_ADDRSTRLEN];
                        uv_ip4_name(addr_in, ip, sizeof(ip));

                        info->properties["address"] = {Value{std::string(ip)}, false, false, true, tok};
                        info->properties["port"] = {Value{static_cast<double>(ntohs(addr_in->sin_port))}, false, false, true, tok};
                        info->properties["family"] = {Value{std::string("IPv4")}, false, false, true, tok};
                    } else if (addr.ss_family == AF_INET6) {
                        struct sockaddr_in6* addr_in6 = (struct sockaddr_in6*)&addr;
                        if (IN6_IS_ADDR_V4MAPPED(&addr_in6->sin6_addr)) {
                            struct sockaddr_in mapped{};
                            mapped.sin_family = AF_INET;
                            memcpy(&mapped.sin_addr, addr_in6->sin6_addr.s6_addr + 12, 4);
                            char ip[INET_ADDRSTRLEN];
                            uv_ip4_name(&mapped, ip, sizeof(ip));
                            info->properties["address"] = {Value{std::string(ip)}, false, false, true, tok};
                            info->properties["port"] = {Value{static_cast<double>(ntohs(addr_in6->sin6_port))}, false, false, true, tok};
                            info->properties["family"] = {Value{std::string("IPv4")}, false, false, true, tok};
                        } else {
                            char ip[INET6_ADDRSTRLEN];
                            uv_ip6_name(addr_in6, ip, sizeof(ip));
                            info->properties["address"] = {Value{std::string(ip)}, false, false, true, tok};
                            info->properties["port"] = {Value{static_cast<double>(ntohs(addr_in6->sin6_port))}, false, false, true, tok};
                            info->properties["family"] = {Value{std::string("IPv6")}, false, false, true, tok};
                        }
                    }
                }
            }

            return Value{info};
        };
        auto address_fn = std::make_shared<FunctionValue>("socket.address", address_impl, nullptr, stok);
        socket_obj->properties["address"] = {Value{address_fn}, false, false, true, stok};

        // If callback provided, register it as 'message' listener
        if (cb) {
            inst->on_message_handler = cb;
            scheduler_run_on_loop([inst]() {
                if (inst->udp_handle && !inst->closed.load()) {
                    uv_udp_recv_start(inst->udp_handle, udp_alloc_cb, udp_recv_cb);
                }
            });
        }

        return Value{socket_obj};
    };

    auto createSocket_fn = std::make_shared<FunctionValue>("udp.createSocket", createSocket_impl, env, tok);
    obj->properties["createSocket"] = {Value{createSocket_fn}, false, false, true, tok};

    return obj;
}
