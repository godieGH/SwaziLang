// unix_socket.cc - Unix domain socket implementation using libuv
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "./net.hpp"

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static std::atomic<int> g_active_unix_work{0};

bool unix_has_active_work() {
    return g_active_unix_work.load() > 0;
}

static Evaluator* g_unix_evaluator = nullptr;

// Unix Socket instance (client or server connection)
struct UnixSocketInstance {
    uv_pipe_t* pipe_handle = nullptr;
    std::atomic<bool> closed{false};
    std::atomic<bool> reading{false};
    FunctionPtr on_data_handler;
    FunctionPtr on_close_handler;
    FunctionPtr on_error_handler;
    FunctionPtr on_connect_handler;
    std::string socket_path;
    long long socket_id = 0;

    std::vector<FunctionPtr> drain_callbacks;
    std::mutex drain_mutex;
    static constexpr size_t WRITE_HIGH_WATERMARK = 16 * 1024;
    Evaluator* evaluator = nullptr;
    std::atomic<bool> paused{false};
};

// Unix Server instance
struct UnixServerInstance {
    uv_pipe_t* server_handle = nullptr;
    FunctionPtr connection_handler;
    std::atomic<bool> closed{false};
    std::string socket_path;
};

static std::mutex g_unix_servers_mutex;
static std::unordered_map<long long, std::shared_ptr<UnixServerInstance>> g_unix_servers;
static std::atomic<long long> g_next_unix_server_id{1};

static std::mutex g_unix_sockets_mutex;
static std::unordered_map<long long, std::shared_ptr<UnixSocketInstance>> g_unix_sockets;
static std::atomic<long long> g_next_unix_socket_id{1};

// Helper to start reading on a socket (idempotent)
static void start_reading_if_needed(UnixSocketInstance* inst) {
    if (!inst || !inst->pipe_handle || inst->closed.load() || inst->paused.load()) {
        return;
    }

    if (inst->reading.exchange(true)) {
        return;  // Already reading
    }

    uv_read_start((uv_stream_t*)inst->pipe_handle,
        [](uv_handle_t*, size_t suggested, uv_buf_t* buf) {
            buf->base = new char[suggested];
            buf->len = (unsigned int)suggested;
        },
        [](uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
            UnixSocketInstance* inst = static_cast<UnixSocketInstance*>(stream->data);

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

                {
                    std::lock_guard<std::mutex> lk(inst->drain_mutex);
                    inst->drain_callbacks.clear();
                }

                if (inst && inst->on_close_handler) {
                    FunctionPtr handler = inst->on_close_handler;
                    CallbackPayload* payload = new CallbackPayload(handler, {});
                    enqueue_callback_global(static_cast<void*>(payload));
                }

                if (!inst->closed.exchange(true)) {
                    long long sock_id = inst->socket_id;
                    uv_close((uv_handle_t*)stream, [](uv_handle_t* h) {
                        UnixSocketInstance* inst = static_cast<UnixSocketInstance*>(h->data);
                        if (inst) {
                            long long sock_id = inst->socket_id;
                            std::lock_guard<std::mutex> lk(g_unix_sockets_mutex);
                            g_unix_sockets.erase(sock_id);
                        }
                        delete (uv_pipe_t*)h;
                    });
                }
            }
        });
}

// Unix socket connection callback
static void on_unix_connection(uv_stream_t* server, int status) {
    if (status < 0) return;

    UnixServerInstance* srv = static_cast<UnixServerInstance*>(server->data);
    if (!srv || srv->closed.load()) return;

    uv_pipe_t* client = new uv_pipe_t;
    uv_pipe_init(server->loop, client, 0);

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        auto sock_inst = std::make_shared<UnixSocketInstance>();
        sock_inst->pipe_handle = client;
        sock_inst->closed = false;
        sock_inst->evaluator = g_unix_evaluator;

        long long sock_id = g_next_unix_socket_id.fetch_add(1);
        sock_inst->socket_id = sock_id;

        {
            std::lock_guard<std::mutex> lk(g_unix_sockets_mutex);
            g_unix_sockets[sock_id] = sock_inst;
        }

        client->data = sock_inst.get();

        // Create socket object to pass to handler
        auto socket_obj = std::make_shared<ObjectValue>();
        Token tok;
        tok.loc = TokenLocation("<unix>", 0, 0, 0);

        // socket.write(data)
        auto write_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return Value{false};
            if (sock_inst->closed.load() || !sock_inst->pipe_handle) {
                throw SwaziError("IOError", "Socket is closed", token.loc);
            }

            std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
            if (data.empty()) return Value{false};

            struct WriteCtx {
                char* buf;
                std::shared_ptr<UnixSocketInstance> inst;
            };

            char* buf = static_cast<char*>(malloc(data.size()));
            memcpy(buf, data.data(), data.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
            uv_write_t* req = new uv_write_t;
            req->data = new WriteCtx{buf, sock_inst};

            int r = uv_write(req, (uv_stream_t*)sock_inst->pipe_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    auto* ctx = static_cast<WriteCtx*>(req->data);
                    free(ctx->buf);
                    auto inst = ctx->inst;
                    delete ctx;
                    delete req;

                    if (inst->pipe_handle && inst->pipe_handle->write_queue_size == 0) {
                        std::vector<FunctionPtr> cbs;
                        {
                            std::lock_guard<std::mutex> lk(inst->drain_mutex);
                            std::swap(cbs, inst->drain_callbacks);
                        }
                        if (!cbs.empty() && inst->evaluator) {
                            Token dtok;
                            dtok.loc = TokenLocation("<unix>", 0, 0, 0);
                            for (auto& cb : cbs) {
                                try {
                                    inst->evaluator->invoke_function(cb, {}, nullptr, dtok);
                                } catch (const SwaziError& e) {
                                    std::cerr << "Unhandled Exception: " << e.what() << std::endl;
                                } catch (...) {}
                            }
                        }
                    }
                });

            if (r != 0) {
                auto* ctx = static_cast<WriteCtx*>(req->data);
                free(ctx->buf);
                delete ctx;
                delete req;
            }

            return Value{r == 0};
        };
        auto write_fn = std::make_shared<FunctionValue>("socket.write", write_impl, nullptr, tok);
        socket_obj->properties["write"] = {Value{write_fn}, false, false, true, tok};

        // socket.close()
        auto close_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.exchange(true) && sock_inst->pipe_handle) {
                long long sock_id = sock_inst->socket_id;
                uv_close((uv_handle_t*)sock_inst->pipe_handle, [](uv_handle_t* h) {
                    UnixSocketInstance* inst = static_cast<UnixSocketInstance*>(h->data);
                    if (inst) {
                        long long sock_id = inst->socket_id;
                        std::lock_guard<std::mutex> lk(g_unix_sockets_mutex);
                        g_unix_sockets.erase(sock_id);
                    }
                    delete (uv_pipe_t*)h;
                });
                sock_inst->pipe_handle = nullptr;
            }
            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, tok);
        socket_obj->properties["close"] = {Value{close_fn}, false, false, true, tok};

        // socket.isOpen()
        auto is_open_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            return Value{!sock_inst->closed.load() && sock_inst->pipe_handle != nullptr};
        };
        auto is_open_fn = std::make_shared<FunctionValue>("socket.isOpen", is_open_impl, nullptr, tok);
        socket_obj->properties["isOpen"] = {Value{is_open_fn}, false, false, true, tok};

        // socket.writableNeedsDrain()
        auto needs_drain_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (sock_inst->closed.load() || !sock_inst->pipe_handle) return Value{false};
            return Value{sock_inst->pipe_handle->write_queue_size >= UnixSocketInstance::WRITE_HIGH_WATERMARK};
        };
        auto needs_drain_fn = std::make_shared<FunctionValue>("socket.writableNeedsDrain", needs_drain_impl, nullptr, tok);
        socket_obj->properties["writableNeedsDrain"] = {Value{needs_drain_fn}, false, false, true, tok};

        // socket.pause()
        auto pause_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.load() && sock_inst->pipe_handle && !sock_inst->paused.exchange(true)) {
                uv_read_stop((uv_stream_t*)sock_inst->pipe_handle);
            }
            return std::monostate{};
        };
        auto pause_fn = std::make_shared<FunctionValue>("socket.pause", pause_impl, nullptr, tok);
        socket_obj->properties["pause"] = {Value{pause_fn}, false, false, true, tok};

        // socket.resume()
        auto resume_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.load() && sock_inst->pipe_handle && sock_inst->paused.exchange(false)) {
                sock_inst->reading.store(false);
                start_reading_if_needed(sock_inst.get());
            }
            return std::monostate{};
        };
        auto resume_fn = std::make_shared<FunctionValue>("socket.resume", resume_impl, nullptr, tok);
        socket_obj->properties["resume"] = {Value{resume_fn}, false, false, true, tok};

        // socket.on(event, handler)
        auto socket_weak = std::weak_ptr<ObjectValue>(socket_obj);
        auto on_impl = [sock_inst, socket_weak](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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
                std::ostringstream ss;
                ss << "Unknown event name: " << event;
                throw SwaziError("TypeError", ss.str(), token.loc);
            }

            return Value{socket_obj};
        };
        auto on_fn = std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, tok);
        socket_obj->properties["on"] = {Value{on_fn}, false, false, true, tok};

        // socket.path property
        socket_obj->properties["path"] = {Value{sock_inst->socket_path}, false, false, true, tok};

        // Call connection handler
        if (srv->connection_handler) {
            FunctionPtr handler = srv->connection_handler;
            CallbackPayload* payload = new CallbackPayload(handler, {Value{socket_obj}});
            enqueue_callback_global(static_cast<void*>(payload));
        }
    } else {
        uv_close((uv_handle_t*)client, [](uv_handle_t* h) { delete (uv_pipe_t*)h; });
    }
}

std::shared_ptr<ObjectValue> make_unix_socket_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<unix>", 0, 0, 0);

    g_unix_evaluator = evaluator;

    // unix.createServer(connectionHandler)
    auto createServer_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
            throw SwaziError("TypeError", "createServer requires a connection handler", token.loc);
        }

        FunctionPtr handler = std::get<FunctionPtr>(args[0]);

        auto inst = std::make_shared<UnixServerInstance>();
        inst->connection_handler = handler;

        long long id = g_next_unix_server_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(g_unix_servers_mutex);
            g_unix_servers[id] = inst;
        }

        auto server_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<unix>", 0, 0, 0);

        // server.listen(path, callback?)
        auto listen_impl = [inst, id](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "listen requires socket path", token.loc);
            }

            std::string path = NetHelpers::value_to_string(args[0]);
            FunctionPtr cb = nullptr;

            if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
                cb = std::get<FunctionPtr>(args[1]);
            }

            inst->socket_path = path;

            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                throw SwaziError("RuntimeError", "No event loop available", token.loc);
            }

            scheduler_run_on_loop([inst, path, cb, loop]() {
                inst->server_handle = new uv_pipe_t;
                inst->server_handle->data = inst.get();
                uv_pipe_init(loop, inst->server_handle, 0);  // 0 = not for IPC

#ifndef _WIN32
                // Remove old socket file if exists
                unlink(path.c_str());
#endif

                int r = uv_pipe_bind(inst->server_handle, path.c_str());
                if (r == 0) {
                    r = uv_listen((uv_stream_t*)inst->server_handle, 128, on_unix_connection);
                }

#ifndef _WIN32
                // Set socket permissions (optional)
                chmod(path.c_str(), 0666);
#endif

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
                        UnixServerInstance* srv = static_cast<UnixServerInstance*>(h->data);
#ifndef _WIN32
                        // Remove socket file on close
                        if (srv && !srv->socket_path.empty()) {
                            unlink(srv->socket_path.c_str());
                        }
#endif
                        delete (uv_pipe_t*)h;
                    });
                    inst->server_handle = nullptr;
                }

                {
                    std::lock_guard<std::mutex> lk(g_unix_servers_mutex);
                    g_unix_servers.erase(id);
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

        // server.path property
        server_obj->properties["path"] = {Value{inst->socket_path}, false, false, true, stok};

        return Value{server_obj};
    };

    auto createServer_fn = std::make_shared<FunctionValue>("unix.createServer", createServer_impl, env, tok);
    obj->properties["createServer"] = {Value{createServer_fn}, false, false, true, tok};

    // unix.connect(path, callback?)
    auto connect_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "connect requires socket path", token.loc);
        }

        std::string path = NetHelpers::value_to_string(args[0]);
        FunctionPtr cb = nullptr;

        g_active_unix_work.fetch_add(1);

        if (args.size() >= 2 && std::holds_alternative<FunctionPtr>(args[1])) {
            cb = std::get<FunctionPtr>(args[1]);
        }

        auto sock_inst = std::make_shared<UnixSocketInstance>();
        sock_inst->on_connect_handler = cb;
        sock_inst->socket_path = path;
        sock_inst->evaluator = g_unix_evaluator;

        long long sock_id = g_next_unix_socket_id.fetch_add(1);
        sock_inst->socket_id = sock_id;

        {
            std::lock_guard<std::mutex> lk(g_unix_sockets_mutex);
            g_unix_sockets[sock_id] = sock_inst;
        }

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            throw SwaziError("RuntimeError", "No event loop available", token.loc);
        }

        // Create pipe handle immediately
        sock_inst->pipe_handle = new uv_pipe_t;
        sock_inst->pipe_handle->data = sock_inst.get();
        uv_pipe_init(loop, sock_inst->pipe_handle, 0);

        // Create socket object
        auto socket_obj = std::make_shared<ObjectValue>();
        Token stok;
        stok.loc = TokenLocation("<unix>", 0, 0, 0);

        // socket.write(data)
        auto write_impl = [sock_inst](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return Value{false};
            if (sock_inst->closed.load() || !sock_inst->pipe_handle) {
                throw SwaziError("IOError", "Socket is closed", token.loc);
            }

            std::vector<uint8_t> data = NetHelpers::get_buffer_data(args[0]);
            if (data.empty()) return Value{false};

            struct WriteCtx {
                char* buf;
                std::shared_ptr<UnixSocketInstance> inst;
            };

            char* buf = static_cast<char*>(malloc(data.size()));
            memcpy(buf, data.data(), data.size());

            uv_buf_t uvbuf = uv_buf_init(buf, (unsigned int)data.size());
            uv_write_t* req = new uv_write_t;
            req->data = new WriteCtx{buf, sock_inst};

            int r = uv_write(req, (uv_stream_t*)sock_inst->pipe_handle, &uvbuf, 1,
                [](uv_write_t* req, int status) {
                    auto* ctx = static_cast<WriteCtx*>(req->data);
                    free(ctx->buf);
                    auto inst = ctx->inst;
                    delete ctx;
                    delete req;

                    if (inst->pipe_handle && inst->pipe_handle->write_queue_size == 0) {
                        std::vector<FunctionPtr> cbs;
                        {
                            std::lock_guard<std::mutex> lk(inst->drain_mutex);
                            std::swap(cbs, inst->drain_callbacks);
                        }
                        if (!cbs.empty() && inst->evaluator) {
                            Token dtok;
                            dtok.loc = TokenLocation("<unix>", 0, 0, 0);
                            for (auto& cb : cbs) {
                                try {
                                    inst->evaluator->invoke_function(cb, {}, nullptr, dtok);
                                } catch (const SwaziError& e) {
                                    std::cerr << "Unhandled Exception: " << e.what() << std::endl;
                                } catch (...) {}
                            }
                        }
                    }
                });

            if (r != 0) {
                auto* ctx = static_cast<WriteCtx*>(req->data);
                free(ctx->buf);
                delete ctx;
                delete req;
            }

            return Value{r == 0};
        };
        auto write_fn = std::make_shared<FunctionValue>("socket.write", write_impl, nullptr, stok);
        socket_obj->properties["write"] = {Value{write_fn}, false, false, true, stok};

        // socket.close()
        auto close_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.exchange(true) && sock_inst->pipe_handle) {
                long long sock_id = sock_inst->socket_id;
                uv_close((uv_handle_t*)sock_inst->pipe_handle, [](uv_handle_t* h) {
                    UnixSocketInstance* inst = static_cast<UnixSocketInstance*>(h->data);
                    if (inst) {
                        long long sock_id = inst->socket_id;
                        std::lock_guard<std::mutex> lk(g_unix_sockets_mutex);
                        g_unix_sockets.erase(sock_id);
                    }
                    delete (uv_pipe_t*)h;
                });
                sock_inst->pipe_handle = nullptr;
            }
            return std::monostate{};
        };
        auto close_fn = std::make_shared<FunctionValue>("socket.close", close_impl, nullptr, stok);
        socket_obj->properties["close"] = {Value{close_fn}, false, false, true, stok};

        // socket.isOpen()
        auto is_open_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            return Value{!sock_inst->closed.load() && sock_inst->pipe_handle != nullptr};
        };
        auto is_open_fn = std::make_shared<FunctionValue>("socket.isOpen", is_open_impl, nullptr, stok);
        socket_obj->properties["isOpen"] = {Value{is_open_fn}, false, false, true, stok};

        // socket.writableNeedsDrain()
        auto needs_drain_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (sock_inst->closed.load() || !sock_inst->pipe_handle) return Value{false};
            return Value{sock_inst->pipe_handle->write_queue_size >= UnixSocketInstance::WRITE_HIGH_WATERMARK};
        };
        auto needs_drain_fn = std::make_shared<FunctionValue>("socket.writableNeedsDrain", needs_drain_impl, nullptr, stok);
        socket_obj->properties["writableNeedsDrain"] = {Value{needs_drain_fn}, false, false, true, stok};

        // socket.pause()
        auto pause_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.load() && sock_inst->pipe_handle && !sock_inst->paused.exchange(true)) {
                uv_read_stop((uv_stream_t*)sock_inst->pipe_handle);
            }
            return std::monostate{};
        };
        auto pause_fn = std::make_shared<FunctionValue>("socket.pause", pause_impl, nullptr, stok);
        socket_obj->properties["pause"] = {Value{pause_fn}, false, false, true, stok};

        // socket.resume()
        auto resume_impl = [sock_inst](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            if (!sock_inst->closed.load() && sock_inst->pipe_handle && sock_inst->paused.exchange(false)) {
                sock_inst->reading.store(false);
                start_reading_if_needed(sock_inst.get());
            }
            return std::monostate{};
        };
        auto resume_fn = std::make_shared<FunctionValue>("socket.resume", resume_impl, nullptr, stok);
        socket_obj->properties["resume"] = {Value{resume_fn}, false, false, true, stok};

        // socket.on(event, handler)
        auto socket_weak = std::weak_ptr<ObjectValue>(socket_obj);
        auto on_impl = [sock_inst, socket_weak](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
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

            if (event == "data") {
                sock_inst->on_data_handler = handler;
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
                std::ostringstream ss;
                ss << "Unknown event name: " << event;
                throw SwaziError("TypeError", ss.str(), token.loc);
            }

            return Value{socket_obj};
        };
        auto on_fn = std::make_shared<FunctionValue>("socket.on", on_impl, nullptr, stok);
        socket_obj->properties["on"] = {Value{on_fn}, false, false, true, stok};

        // socket.path property
        socket_obj->properties["path"] = {Value{path}, false, false, true, stok};

        // Schedule connection attempt
        struct ConnectData {
            std::shared_ptr<UnixSocketInstance> sock_inst;
            std::shared_ptr<ObjectValue> socket_obj;
        };

        uv_connect_t* connect_req = new uv_connect_t;
        connect_req->data = new ConnectData{sock_inst, socket_obj};

        uv_pipe_connect(connect_req, sock_inst->pipe_handle, path.c_str(),
            [](uv_connect_t* req, int status) {
                auto* conn_data = static_cast<ConnectData*>(req->data);
                std::shared_ptr<UnixSocketInstance> inst = conn_data->sock_inst;
                std::shared_ptr<ObjectValue> socket_obj = conn_data->socket_obj;
                delete conn_data;

                g_active_unix_work.fetch_sub(1);

                if (status == 0) {
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

                    if (!inst->closed.exchange(true) && inst->pipe_handle) {
                        long long sock_id = inst->socket_id;
                        uv_close((uv_handle_t*)inst->pipe_handle, [](uv_handle_t* h) {
                            UnixSocketInstance* inst = static_cast<UnixSocketInstance*>(h->data);
                            if (inst) {
                                long long sock_id = inst->socket_id;
                                std::lock_guard<std::mutex> lk(g_unix_sockets_mutex);
                                g_unix_sockets.erase(sock_id);
                            }
                            delete (uv_pipe_t*)h;
                        });
                    }
                }

                delete req;
            });

        return Value{socket_obj};
    };

    auto connect_fn = std::make_shared<FunctionValue>("unix.connect", connect_impl, env, tok);
    obj->properties["connect"] = {Value{connect_fn}, false, false, true, tok};

    return obj;
}
