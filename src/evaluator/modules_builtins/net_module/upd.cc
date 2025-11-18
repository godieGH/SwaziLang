// udp.cc - UDP socket implementation using libuv
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "./net.hpp"

// UDP Socket instance
struct UdpSocketInstance {
    uv_udp_t* udp_handle = nullptr;
    std::atomic<bool> closed{false};
    FunctionPtr on_message_handler;
    FunctionPtr on_error_handler;
    FunctionPtr on_close_handler;
    std::string bound_address;
    int bound_port = 0;
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
            char ip[INET6_ADDRSTRLEN];
            uv_ip6_name(addr_in6, ip, sizeof(ip));
            sender_addr = ip;
            sender_port = ntohs(addr_in6->sin6_port);
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
            Value{addr->sa_family == AF_INET ? std::string("IPv4") : std::string("IPv6")},
            false, false, true, tok};
        rinfo->properties["size"] = {Value{static_cast<double>(nread)}, false, false, true, tok};

        // Call handler with (message, rinfo)
        FunctionPtr handler = inst->on_message_handler;
        CallbackPayload* payload = new CallbackPayload(handler, {Value{buffer}, Value{rinfo}});
        enqueue_callback_global(static_cast<void*>(payload));
    }

    if (nread < 0 && inst && inst->on_error_handler) {
        auto err_msg = std::string("UDP receive error: ") + uv_strerror((int)nread);
        FunctionPtr handler = inst->on_error_handler;
        CallbackPayload* payload = new CallbackPayload(handler, {Value{err_msg}});
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
        long long sock_id = g_next_udp_socket_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(g_udp_sockets_mutex);
            g_udp_sockets[sock_id] = inst;
        }

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            throw SwaziError("RuntimeError", "No event loop available", token.loc);
        }

        // Create socket object
        auto socket_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<udp>", 0, 0, 0);

        // Initialize UDP handle
        scheduler_run_on_loop([inst, type, loop]() {
            inst->udp_handle = new uv_udp_t;
            inst->udp_handle->data = inst.get();

            unsigned int flags = (type == "udp6") ? AF_INET6 : AF_INET;
            uv_udp_init_ex(loop, inst->udp_handle, flags);
        });

        // socket.bind(port, address?, callback?)
        auto bind_impl = [inst, sock_id](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "bind requires port", token.loc);
            }

            int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            std::string address = "0.0.0.0";
            FunctionPtr cb = nullptr;

            if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
                address = std::get<std::string>(args[1]);
            }

            if (args.size() >= 3 && std::holds_alternative<FunctionPtr>(args[2])) {
                cb = std::get<FunctionPtr>(args[2]);
            } else if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                cb = std::get<FunctionPtr>(args[1]);
            }

            inst->bound_port = port;
            inst->bound_address = address;

            scheduler_run_on_loop([inst, port, address, cb]() {
                struct sockaddr_in addr;
                uv_ip4_addr(address.c_str(), port, &addr);

                int r = uv_udp_bind(inst->udp_handle, (const struct sockaddr*)&addr, UV_UDP_REUSEADDR);

                if (cb) {
                    if (r == 0) {
                        CallbackPayload* payload = new CallbackPayload(cb, {});
                        enqueue_callback_global(static_cast<void*>(payload));
                    } else {
                        auto err_msg = std::string("Bind failed: ") + uv_strerror(r);
                        CallbackPayload* payload = new CallbackPayload(cb, {Value{err_msg}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
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

            scheduler_run_on_loop([inst, data, port, address, cb]() {
                struct sockaddr_in addr;
                uv_ip4_addr(address.c_str(), port, &addr);

                char* buf = static_cast<char*>(malloc(data.size()));
                memcpy(buf, data.data(), data.size());

                uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
                uv_udp_send_t* req = new uv_udp_send_t;
                req->data = buf;

                uv_udp_send(req, inst->udp_handle, &uvbuf, 1, (const struct sockaddr*)&addr,
                    [](uv_udp_send_t* req, int status) {
                        if (req->data) free(req->data);

                        // Extract callback from somewhere if needed
                        delete req;
                    });

                if (cb) {
                    CallbackPayload* payload = new CallbackPayload(cb, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                }
            });

            return std::monostate{};
        };
        auto send_fn = std::make_shared<FunctionValue>("socket.send", send_impl, nullptr, stok);
        socket_obj->properties["send"] = {Value{send_fn}, false, false, true, stok};

        // socket.on(event, handler)
        auto on_impl = [inst, socket_obj](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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
                    if (inst->udp_handle) {
                        uv_udp_recv_start(inst->udp_handle, udp_alloc_cb, udp_recv_cb);
                    }
                });
            } else if (event == "error") {
                inst->on_error_handler = handler;
            } else if (event == "close") {
                inst->on_close_handler = handler;
            }

            return Value{socket_obj};
        };
        auto on_fn = std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, stok);
        socket_obj->properties["on"] = {Value{on_fn}, false, false, true, stok};

        // socket.close(callback?)
        auto close_impl = [inst, sock_id](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            FunctionPtr cb = (!args.empty() && std::holds_alternative<FunctionPtr>(args[0]))
                ? std::get<FunctionPtr>(args[0])
                : nullptr;

            if (!inst->closed.exchange(true)) {
                scheduler_run_on_loop([inst, cb]() {
                    if (inst->udp_handle) {
                        uv_udp_recv_stop(inst->udp_handle);
                        uv_close((uv_handle_t*)inst->udp_handle, [](uv_handle_t* h) {
                            delete (uv_udp_t*)h;
                        });
                        inst->udp_handle = nullptr;
                    }

                    if (cb) {
                        CallbackPayload* payload = new CallbackPayload(cb, {});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                });
            }

            std::lock_guard<std::mutex> lk(g_udp_sockets_mutex);
            g_udp_sockets.erase(sock_id);

            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, stok);
        socket_obj->properties["close"] = {Value{close_fn}, false, false, true, stok};

        // socket.address() -> {address, port, family}
        auto address_impl = [inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto info = std::make_shared<ObjectValue>();
            Token tok;
            tok.loc = TokenLocation("<udp>", 0, 0, 0);

            if (inst->udp_handle) {
                struct sockaddr_storage addr;
                int namelen = sizeof(addr);
                uv_udp_getsockname(inst->udp_handle, (struct sockaddr*)&addr, &namelen);

                if (addr.ss_family == AF_INET) {
                    struct sockaddr_in* addr_in = (struct sockaddr_in*)&addr;
                    char ip[INET_ADDRSTRLEN];
                    uv_ip4_name(addr_in, ip, sizeof(ip));

                    info->properties["address"] = {Value{std::string(ip)}, false, false, true, tok};
                    info->properties["port"] = {Value{static_cast<double>(ntohs(addr_in->sin_port))}, false, false, true, tok};
                    info->properties["family"] = {Value{std::string("IPv4")}, false, false, true, tok};
                } else if (addr.ss_family == AF_INET6) {
                    struct sockaddr_in6* addr_in6 = (struct sockaddr_in6*)&addr;
                    char ip[INET6_ADDRSTRLEN];
                    uv_ip6_name(addr_in6, ip, sizeof(ip));

                    info->properties["address"] = {Value{std::string(ip)}, false, false, true, tok};
                    info->properties["port"] = {Value{static_cast<double>(ntohs(addr_in6->sin6_port))}, false, false, true, tok};
                    info->properties["family"] = {Value{std::string("IPv6")}, false, false, true, tok};
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
                if (inst->udp_handle) {
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