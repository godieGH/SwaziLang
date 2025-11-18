// ws.cc - WebSocket implementation (basic)
// Note: Full WebSocket implementation requires protocol handling (handshake, framing, masking)
// This is a simplified version showing the structure
#include <atomic>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <unordered_map>

#include "./net.hpp"

// WebSocket opcodes
enum class WsOpcode : uint8_t {
    CONTINUATION = 0x0,
    TEXT = 0x1,
    BINARY = 0x2,
    CLOSE = 0x8,
    PING = 0x9,
    PONG = 0xA
};

// WebSocket frame structure
struct WsFrame {
    bool fin;
    WsOpcode opcode;
    bool masked;
    uint64_t payload_length;
    uint8_t mask[4];
    std::vector<uint8_t> payload;
};

// WebSocket connection instance
struct WsConnectionInstance {
    uv_tcp_t* socket_handle = nullptr;
    std::atomic<bool> closed{false};
    bool is_server = false;

    FunctionPtr on_open_handler;
    FunctionPtr on_message_handler;
    FunctionPtr on_close_handler;
    FunctionPtr on_error_handler;
    FunctionPtr on_ping_handler;
    FunctionPtr on_pong_handler;

    std::vector<uint8_t> receive_buffer;
    bool handshake_complete = false;
};

// WebSocket server instance
struct WsServerInstance {
    uv_tcp_t* server_handle = nullptr;
    FunctionPtr connection_handler;
    std::atomic<bool> closed{false};
    int port = 0;
    std::string path = "/";
};

static std::mutex g_ws_servers_mutex;
static std::unordered_map<long long, std::shared_ptr<WsServerInstance>> g_ws_servers;
static std::atomic<long long> g_next_ws_server_id{1};

static std::mutex g_ws_connections_mutex;
static std::unordered_map<long long, std::shared_ptr<WsConnectionInstance>> g_ws_connections;
static std::atomic<long long> g_next_ws_connection_id{1};

// Helper: Parse WebSocket frame from buffer
static bool parse_ws_frame(const std::vector<uint8_t>& data, size_t& offset, WsFrame& frame) {
    if (data.size() < offset + 2) return false;

    uint8_t byte1 = data[offset];
    uint8_t byte2 = data[offset + 1];

    frame.fin = (byte1 & 0x80) != 0;
    frame.opcode = static_cast<WsOpcode>(byte1 & 0x0F);
    frame.masked = (byte2 & 0x80) != 0;

    uint64_t len = byte2 & 0x7F;
    offset += 2;

    if (len == 126) {
        if (data.size() < offset + 2) return false;
        len = (data[offset] << 8) | data[offset + 1];
        offset += 2;
    } else if (len == 127) {
        if (data.size() < offset + 8) return false;
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | data[offset + i];
        }
        offset += 8;
    }

    frame.payload_length = len;

    if (frame.masked) {
        if (data.size() < offset + 4) return false;
        memcpy(frame.mask, &data[offset], 4);
        offset += 4;
    }

    if (data.size() < offset + len) return false;

    frame.payload.resize(len);
    memcpy(frame.payload.data(), &data[offset], len);
    offset += len;

    // Unmask payload if needed
    if (frame.masked) {
        for (size_t i = 0; i < len; i++) {
            frame.payload[i] ^= frame.mask[i % 4];
        }
    }

    return true;
}

// Helper: Create WebSocket frame
static std::vector<uint8_t> create_ws_frame(WsOpcode opcode, const std::vector<uint8_t>& payload, bool mask = false) {
    std::vector<uint8_t> frame;

    // Byte 1: FIN + opcode
    frame.push_back(0x80 | static_cast<uint8_t>(opcode));

    // Byte 2: MASK + payload length
    uint64_t len = payload.size();
    uint8_t byte2 = mask ? 0x80 : 0x00;

    if (len < 126) {
        frame.push_back(byte2 | static_cast<uint8_t>(len));
    } else if (len < 65536) {
        frame.push_back(byte2 | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    } else {
        frame.push_back(byte2 | 127);
        for (int i = 7; i >= 0; i--) {
            frame.push_back((len >> (i * 8)) & 0xFF);
        }
    }

    // Mask key (if masking)
    uint8_t mask_key[4] = {0};
    if (mask) {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        for (int i = 0; i < 4; i++) {
            mask_key[i] = dis(gen);
            frame.push_back(mask_key[i]);
        }
    }

    // Payload (masked if needed)
    for (size_t i = 0; i < payload.size(); i++) {
        uint8_t byte = payload[i];
        if (mask) byte ^= mask_key[i % 4];
        frame.push_back(byte);
    }

    return frame;
}

// Helper: Generate WebSocket accept key
static std::string generate_accept_key(const std::string& key) {
    // In real implementation, would use SHA-1 + Base64
    // This is a placeholder - you'd need to implement proper SHA-1 hashing
    return "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";  // dummy value
}

// Helper: Check if data contains complete HTTP handshake
static bool parse_ws_handshake(const std::vector<uint8_t>& data, std::string& ws_key, std::string& path) {
    std::string request(data.begin(), data.end());

    // Find end of headers
    size_t end_pos = request.find("\r\n\r\n");
    if (end_pos == std::string::npos) return false;

    // Extract path from first line
    size_t path_start = request.find(' ');
    size_t path_end = request.find(' ', path_start + 1);
    if (path_start != std::string::npos && path_end != std::string::npos) {
        path = request.substr(path_start + 1, path_end - path_start - 1);
    }

    // Extract Sec-WebSocket-Key
    size_t key_pos = request.find("Sec-WebSocket-Key:");
    if (key_pos != std::string::npos) {
        size_t key_start = request.find_first_not_of(" \t", key_pos + 18);
        size_t key_end = request.find("\r\n", key_start);
        ws_key = request.substr(key_start, key_end - key_start);
        return true;
    }

    return false;
}

std::shared_ptr<ObjectValue> make_ws_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<ws>", 0, 0, 0);

    // Helper to create WebSocket connection object
    auto create_ws_connection_obj = [](std::shared_ptr<WsConnectionInstance> inst, long long conn_id) -> ObjectPtr {
        auto ws_obj = std::make_shared<ObjectValue>();
        Token wtok;
        wtok.loc = TokenLocation("<ws>", 0, 0, 0);

        // ws.send(data, options?)
        auto send_impl = [inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return Value{false};
            if (inst->closed.load() || !inst->socket_handle || !inst->handshake_complete) {
                throw SwaziError("IOError", "WebSocket connection is not open", token.loc);
            }

            std::vector<uint8_t> payload = NetHelpers::get_buffer_data(args[0]);
            bool is_binary = NetHelpers::is_buffer(args[0]);

            WsOpcode opcode = is_binary ? WsOpcode::BINARY : WsOpcode::TEXT;

            // Client must mask, server must not
            bool should_mask = !inst->is_server;
            std::vector<uint8_t> frame = create_ws_frame(opcode, payload, should_mask);

            char* buf = static_cast<char*>(malloc(frame.size()));
            memcpy(buf, frame.data(), frame.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)frame.size());
            uv_write_t* req = new uv_write_t;
            req->data = buf;

            int r = uv_write(req, (uv_stream_t*)inst->socket_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    if (req->data) free(req->data);
                    delete req;
                });

            return Value{r == 0};
        };
        auto send_fn = std::make_shared<FunctionValue>("ws.send", send_impl, nullptr, wtok);
        ws_obj->properties["send"] = {Value{send_fn}, false, false, true, wtok};

        // ws.ping(data?)
        auto ping_impl = [inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (inst->closed.load() || !inst->socket_handle || !inst->handshake_complete) {
                throw SwaziError("IOError", "WebSocket connection is not open", token.loc);
            }

            std::vector<uint8_t> payload;
            if (!args.empty()) {
                payload = NetHelpers::get_buffer_data(args[0]);
            }

            bool should_mask = !inst->is_server;
            std::vector<uint8_t> frame = create_ws_frame(WsOpcode::PING, payload, should_mask);

            char* buf = static_cast<char*>(malloc(frame.size()));
            memcpy(buf, frame.data(), frame.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)frame.size());
            uv_write_t* req = new uv_write_t;
            req->data = buf;

            uv_write(req, (uv_stream_t*)inst->socket_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    if (req->data) free(req->data);
                    delete req;
                });

            return std::monostate{};
        };
        auto ping_fn = std::make_shared<FunctionValue>("ws.ping", ping_impl, nullptr, wtok);
        ws_obj->properties["ping"] = {Value{ping_fn}, false, false, true, wtok};

        // ws.close(code?, reason?)
        auto close_impl = [inst, conn_id](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            if (!inst->closed.exchange(true)) {
                scheduler_run_on_loop([inst]() {
                    if (inst->socket_handle) {
                        // Send close frame
                        std::vector<uint8_t> payload;
                        bool should_mask = !inst->is_server;
                        std::vector<uint8_t> frame = create_ws_frame(WsOpcode::CLOSE, payload, should_mask);

                        char* buf = static_cast<char*>(malloc(frame.size()));
                        memcpy(buf, frame.data(), frame.size());

                        uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)frame.size());
                        uv_write_t* req = new uv_write_t;
                        req->data = buf;

                        uv_write(req, (uv_stream_t*)inst->socket_handle, &uvbuf, 1,
                            [](uv_write_t* req, int status) {
                                if (req->data) free(req->data);
                                delete req;
                            });

                        uv_close((uv_handle_t*)inst->socket_handle, [](uv_handle_t* h) {
                            delete (uv_tcp_t*)h;
                        });
                        inst->socket_handle = nullptr;
                    }
                });
            }

            std::lock_guard<std::mutex> lk(g_ws_connections_mutex);
            g_ws_connections.erase(conn_id);

            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("ws.close", close_impl, nullptr, wtok);
        ws_obj->properties["close"] = {Value{close_fn}, false, false, true, wtok};

        // ws.on(event, handler)
        auto on_impl = [inst, ws_obj](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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
            } else if (event == "open") {
                inst->on_open_handler = handler;
            } else if (event == "close") {
                inst->on_close_handler = handler;
            } else if (event == "error") {
                inst->on_error_handler = handler;
            } else if (event == "ping") {
                inst->on_ping_handler = handler;
            } else if (event == "pong") {
                inst->on_pong_handler = handler;
            }

            return Value{ws_obj};
        };
        auto on_fn = std::make_shared<FunctionValue>("ws.on", on_impl, nullptr, wtok);
        ws_obj->properties["on"] = {Value{on_fn}, false, false, true, wtok};

        return ws_obj;
    };

    // ws.createServer(options?, connectionHandler?)
    auto createServer_impl = [create_ws_connection_obj](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        FunctionPtr handler = nullptr;
        std::string path = "/";

        if (!args.empty()) {
            if (std::holds_alternative<FunctionPtr>(args[0])) {
                handler = std::get<FunctionPtr>(args[0]);
            } else if (std::holds_alternative<ObjectPtr>(args[0])) {
                // Options object
                ObjectPtr opts = std::get<ObjectPtr>(args[0]);
                auto path_prop = opts->properties.find("path");
                if (path_prop != opts->properties.end()) {
                    path = NetHelpers::value_to_string(path_prop->second.value);
                }
            }
        }

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            handler = std::get<FunctionPtr>(args[1]);
        }

        auto inst = std::make_shared<WsServerInstance>();
        inst->connection_handler = handler;
        inst->path = path;

        long long id = g_next_ws_server_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(g_ws_servers_mutex);
            g_ws_servers[id] = inst;
        }

        auto server_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<ws>", 0, 0, 0);

        // server.listen(port, callback?)
        auto listen_impl = [inst, id, create_ws_connection_obj](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "listen requires port", token.loc);
            }

            int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            FunctionPtr cb = (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1]))
                ? std::get<FunctionPtr>(args[1])
                : nullptr;

            inst->port = port;

            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                throw SwaziError("RuntimeError", "No event loop available", token.loc);
            }

            scheduler_run_on_loop([inst, port, loop, cb, create_ws_connection_obj]() {
                inst->server_handle = new uv_tcp_t;
                inst->server_handle->data = inst.get();
                uv_tcp_init(loop, inst->server_handle);

                struct sockaddr_in addr;
                uv_ip4_addr("0.0.0.0", port, &addr);
                uv_tcp_bind(inst->server_handle, (const struct sockaddr*)&addr, 0);

                // Connection callback
                auto on_connection = [](uv_stream_t* server, int status) {
                    if (status < 0) return;

                    WsServerInstance* srv = static_cast<WsServerInstance*>(server->data);
                    if (!srv || srv->closed.load()) return;

                    uv_tcp_t* client = new uv_tcp_t;
                    uv_tcp_init(server->loop, client);

                    if (uv_accept(server, (uv_stream_t*)client) == 0) {
                        auto conn_inst = std::make_shared<WsConnectionInstance>();
                        conn_inst->socket_handle = client;
                        conn_inst->is_server = true;

                        long long conn_id = g_next_ws_connection_id.fetch_add(1);
                        {
                            std::lock_guard<std::mutex> lk(g_ws_connections_mutex);
                            g_ws_connections[conn_id] = conn_inst;
                        }

                        client->data = conn_inst.get();

                        // Start reading for handshake
                        uv_read_start((uv_stream_t*)client,
                            [](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
                                buf->base = new char[suggested];
                                buf->len = (unsigned int)suggested;
                            },
                            [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
                                WsConnectionInstance* inst = static_cast<WsConnectionInstance*>(stream->data);

                                if (nread > 0 && inst) {
                                    inst->receive_buffer.insert(inst->receive_buffer.end(),
                                        buf->base, buf->base + nread);

                                    if (!inst->handshake_complete) {
                                        // Try to parse handshake
                                        std::string ws_key, path;
                                        if (parse_ws_handshake(inst->receive_buffer, ws_key, path)) {
                                            // Send handshake response
                                            std::string accept_key = generate_accept_key(ws_key);
                                            std::ostringstream response;
                                            response << "HTTP/1.1 101 Switching Protocols\r\n";
                                            response << "Upgrade: websocket\r\n";
                                            response << "Connection: Upgrade\r\n";
                                            response << "Sec-WebSocket-Accept: " << accept_key << "\r\n";
                                            response << "\r\n";

                                            std::string resp_str = response.str();
                                            char* resp_buf = static_cast<char*>(malloc(resp_str.size()));
                                            memcpy(resp_buf, resp_str.data(), resp_str.size());

                                            uv_buf_t uvbuf = uv_buf_init(resp_buf, (unsigned int)resp_str.size());
                                            uv_write_t* req = new uv_write_t;
                                            req->data = resp_buf;

                                            uv_write(req, stream, &uvbuf, 1,
                                                [](uv_write_t* req, int status) {
                                                    if (req->data) free(req->data);
                                                    delete req;
                                                });

                                            inst->handshake_complete = true;
                                            inst->receive_buffer.clear();

                                            // Trigger open event
                                            if (inst->on_open_handler) {
                                                FunctionPtr handler = inst->on_open_handler;
                                                CallbackPayload* payload = new CallbackPayload(handler, {});
                                                enqueue_callback_global(static_cast<void*>(payload));
                                            }
                                        }
                                    } else {
                                        // Parse WebSocket frames
                                        size_t offset = 0;
                                        WsFrame frame;

                                        while (parse_ws_frame(inst->receive_buffer, offset, frame)) {
                                            if (frame.opcode == WsOpcode::TEXT || frame.opcode == WsOpcode::BINARY) {
                                                if (inst->on_message_handler) {
                                                    Value msg_data;
                                                    if (frame.opcode == WsOpcode::TEXT) {
                                                        msg_data = Value{std::string(frame.payload.begin(), frame.payload.end())};
                                                    } else {
                                                        auto buf = std::make_shared<BufferValue>();
                                                        buf->data = frame.payload;
                                                        buf->encoding = "binary";
                                                        msg_data = Value{buf};
                                                    }

                                                    FunctionPtr handler = inst->on_message_handler;
                                                    CallbackPayload* payload = new CallbackPayload(handler, {msg_data});
                                                    enqueue_callback_global(static_cast<void*>(payload));
                                                }
                                            } else if (frame.opcode == WsOpcode::CLOSE) {
                                                if (inst->on_close_handler) {
                                                    FunctionPtr handler = inst->on_close_handler;
                                                    CallbackPayload* payload = new CallbackPayload(handler, {});
                                                    enqueue_callback_global(static_cast<void*>(payload));
                                                }
                                                uv_close((uv_handle_t*)stream, [](uv_handle_t* h) {
                                                    delete (uv_tcp_t*)h;
                                                });
                                                break;
                                            }
                                        }

                                        // Remove processed data
                                        if (offset > 0) {
                                            inst->receive_buffer.erase(inst->receive_buffer.begin(),
                                                inst->receive_buffer.begin() + offset);
                                        }
                                    }
                                }

                                delete[] buf->base;

                                if (nread < 0) {
                                    if (inst && inst->on_close_handler) {
                                        FunctionPtr handler = inst->on_close_handler;
                                        CallbackPayload* payload = new CallbackPayload(handler, {});
                                        enqueue_callback_global(static_cast<void*>(payload));
                                    }
                                    uv_close((uv_handle_t*)stream, [](uv_handle_t* h) {
                                        delete (uv_tcp_t*)h;
                                    });
                                }
                            });

                        // Note: The create_ws_connection_obj call would happen after handshake
                        // For simplicity, we're not fully implementing that here
                    }
                };

                int r = uv_listen((uv_stream_t*)inst->server_handle, 128, on_connection);

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

        return Value{server_obj};
    };

    auto createServer_fn = std::make_shared<FunctionValue>("ws.createServer", createServer_impl, env, tok);
    obj->properties["createServer"] = {Value{createServer_fn}, false, false, true, tok};

    return obj;
}