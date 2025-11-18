#include <cstring>
#include <sstream>

#include "./net.hpp"
#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// Helper: create a native FunctionValue from a lambda
template <typename F>
static FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    auto fn = std::make_shared<FunctionValue>(name, native_impl, env, Token());
    return fn;
}

std::shared_ptr<ObjectValue> make_net_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<net>", 0, 0, 0);

    // net.tcp -> TCP client + server APIs
    {
        obj->properties["tcp"] = PropertyDescriptor{Value{make_tcp_exports(env, evaluator)},
            false, false, true, tok};
    }

    // net.udp -> UDP socket APIs
    {
        obj->properties["udp"] = PropertyDescriptor{Value{make_udp_exports(env, evaluator)},
            false, false, true, tok};
    }

    // net.ws -> WebSocket client + server
    {
        obj->properties["ws"] = PropertyDescriptor{Value{make_ws_exports(env, evaluator)},
            false, false, true, tok};
    }

    // net.resolve(host) -> Promise<array of addresses>
    {
        auto fn_resolve = make_native_fn("net.resolve", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "resolve requires hostname", token.loc);
            }
            
            std::string host = NetHelpers::value_to_string(args[0]);
            
            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;
            
            // Use libuv's getaddrinfo
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("No event loop available")};
                return Value{promise};
            }
            
            // Create getaddrinfo request
            struct GetAddrInfoData {
                std::shared_ptr<PromiseValue> promise;
                std::string host;
            };
            
            auto* data = new GetAddrInfoData{promise, host};
            
            scheduler_run_on_loop([loop, data]() {
                uv_getaddrinfo_t* req = new uv_getaddrinfo_t;
                req->data = data;
                
                struct addrinfo hints;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_UNSPEC;  // Allow IPv4 or IPv6
                hints.ai_socktype = SOCK_STREAM;
                
                int r = uv_getaddrinfo(loop, req, 
                    [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
                        GetAddrInfoData* data = static_cast<GetAddrInfoData*>(req->data);
                        
                        if (status == 0 && res) {
                            auto arr = std::make_shared<ArrayValue>();
                            
                            for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
                                char ip[INET6_ADDRSTRLEN];
                                
                                if (p->ai_family == AF_INET) {
                                    struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
                                    inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                                    arr->elements.push_back(Value{std::string(ip)});
                                } else if (p->ai_family == AF_INET6) {
                                    struct sockaddr_in6* addr = (struct sockaddr_in6*)p->ai_addr;
                                    inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));
                                    arr->elements.push_back(Value{std::string(ip)});
                                }
                            }
                            
                            uv_freeaddrinfo(res);
                            
                            data->promise->state = PromiseValue::State::FULFILLED;
                            data->promise->result = Value{arr};
                            
                            for (auto& cb : data->promise->then_callbacks) {
                                try { cb(data->promise->result); } catch(...) {}
                            }
                        } else {
                            std::string err_msg = "DNS resolution failed";
                            if (status != 0) {
                                err_msg += ": " + std::string(uv_strerror(status));
                            }
                            
                            data->promise->state = PromiseValue::State::REJECTED;
                            data->promise->result = Value{err_msg};
                            
                            for (auto& cb : data->promise->catch_callbacks) {
                                try { cb(data->promise->result); } catch(...) {}
                            }
                        }
                        
                        delete data;
                        delete req;
                    },
                    data->host.c_str(), nullptr, &hints);
                
                if (r != 0) {
                    data->promise->state = PromiseValue::State::REJECTED;
                    data->promise->result = Value{std::string("Failed to start DNS resolution: ") + uv_strerror(r)};
                    
                    for (auto& cb : data->promise->catch_callbacks) {
                        try { cb(data->promise->result); } catch(...) {}
                    }
                    
                    delete data;
                    delete req;
                }
            });
            
            return Value{promise}; }, env);
        obj->properties["resolve"] = PropertyDescriptor{fn_resolve, false, false, true, tok};
    }

    // net.isIPv4(str) -> bool
    {
        auto fn_isIPv4 = make_native_fn("net.isIPv4", [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            if (args.empty()) return Value{false};
            
            std::string ip = NetHelpers::value_to_string(args[0]);
            
            struct sockaddr_in sa;
            return Value{inet_pton(AF_INET, ip.c_str(), &(sa.sin_addr)) == 1}; }, env);
        obj->properties["isIPv4"] = PropertyDescriptor{fn_isIPv4, false, false, true, tok};
    }

    // net.isIPv6(str) -> bool
    {
        auto fn_isIPv6 = make_native_fn("net.isIPv6", [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            if (args.empty()) return Value{false};
            
            std::string ip = NetHelpers::value_to_string(args[0]);
            
            struct sockaddr_in6 sa;
            return Value{inet_pton(AF_INET6, ip.c_str(), &(sa.sin6_addr)) == 1}; }, env);
        obj->properties["isIPv6"] = PropertyDescriptor{fn_isIPv6, false, false, true, tok};
    }

    // net.localIPs() - Array of all local IPs
    {
        auto fn_localIPs = make_native_fn("net.localIPs", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            auto arr = std::make_shared<ArrayValue>();

#ifdef _WIN32
            // Windows implementation
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                return Value{arr};
            }
            
            char hostname[256];
            if (gethostname(hostname, sizeof(hostname)) == 0) {
                struct addrinfo hints, *result;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                
                if (getaddrinfo(hostname, nullptr, &hints, &result) == 0) {
                    for (struct addrinfo* p = result; p != nullptr; p = p->ai_next) {
                        char ip[INET6_ADDRSTRLEN];
                        
                        if (p->ai_family == AF_INET) {
                            struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
                            inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                            arr->elements.push_back(Value{std::string(ip)});
                        } else if (p->ai_family == AF_INET6) {
                            struct sockaddr_in6* addr = (struct sockaddr_in6*)p->ai_addr;
                            inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));
                            arr->elements.push_back(Value{std::string(ip)});
                        }
                    }
                    freeaddrinfo(result);
                }
            }
            
            WSACleanup();
#else
            // Unix/Linux implementation
            struct ifaddrs *ifaddr, *ifa;
            
            if (getifaddrs(&ifaddr) == 0) {
                for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
                    if (ifa->ifa_addr == nullptr) continue;
                    
                    int family = ifa->ifa_addr->sa_family;
                    char ip[INET6_ADDRSTRLEN];
                    
                    if (family == AF_INET) {
                        struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
                        inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
                        arr->elements.push_back(Value{std::string(ip)});
                    } else if (family == AF_INET6) {
                        struct sockaddr_in6* addr = (struct sockaddr_in6*)ifa->ifa_addr;
                        inet_ntop(AF_INET6, &addr->sin6_addr, ip, sizeof(ip));
                        arr->elements.push_back(Value{std::string(ip)});
                    }
                }
                freeifaddrs(ifaddr);
            }
#endif
            
            return Value{arr}; }, env);
        obj->properties["localIPs"] = PropertyDescriptor{fn_localIPs, false, false, true, tok};
    }

    // net.isPortFree(port) - bool
    {
        auto fn_isPortFree = make_native_fn("net.isPortFree", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "isPortFree requires port number", token.loc);
            }
            
            int port = static_cast<int>(NetHelpers::value_to_number(args[0]));
            
            // Try to bind to the port
            int sock = socket(AF_INET, SOCK_STREAM, 0);
            if (sock < 0) return Value{false};
            
            struct sockaddr_in addr;
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port = htons(port);
            
            int opt = 1;
#ifdef _WIN32
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
            
            bool is_free = (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0);

#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
            
            return Value{is_free}; }, env);
        obj->properties["isPortFree"] = PropertyDescriptor{fn_isPortFree, false, false, true, tok};
    }

    // net.ping(host, options?) - Promise
    // Note: ICMP ping requires raw sockets (privileged), so we use TCP connect as fallback
    {
        auto fn_ping = make_native_fn("net.ping", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "ping requires host", token.loc);
            }
            
            std::string host = NetHelpers::value_to_string(args[0]);
            int timeout_ms = 5000;
            int port = 80;  // Default to HTTP port for TCP ping
            
            if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
                ObjectPtr opts = std::get<ObjectPtr>(args[1]);
                
                auto timeout_prop = opts->properties.find("timeout");
                if (timeout_prop != opts->properties.end()) {
                    timeout_ms = static_cast<int>(NetHelpers::value_to_number(timeout_prop->second.value));
                }
                
                auto port_prop = opts->properties.find("port");
                if (port_prop != opts->properties.end()) {
                    port = static_cast<int>(NetHelpers::value_to_number(port_prop->second.value));
                }
            }
            
            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;
            
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("No event loop available")};
                return Value{promise};
            }
            
            struct PingData {
    std::shared_ptr<PromiseValue> promise;
    std::string host;
    int port;
    int timeout_ms;  // ADD THIS LINE
    std::chrono::steady_clock::time_point start_time;
    uv_tcp_t* socket;
    uv_timer_t* timer;
    uv_getaddrinfo_t* resolver;
};
            
            auto* data = new PingData{promise, host, port, timeout_ms, std::chrono::steady_clock::now(), nullptr, nullptr, nullptr};
            
            scheduler_run_on_loop([loop, data, timeout_ms]() {
                data->socket = new uv_tcp_t;
                uv_tcp_init(loop, data->socket);
                
                // Create timeout timer
                data->timer = new uv_timer_t;
                data->timer->data = data;
                uv_timer_init(loop, data->timer);
                
                uv_timer_start(data->timer, [](uv_timer_t* handle) {
                    PingData* data = static_cast<PingData*>(handle->data);
                    
                    auto result = std::make_shared<ObjectValue>();
                    Token tok;
                    tok.loc = TokenLocation("<net>", 0, 0, 0);
                    
                    result->properties["ok"] = {Value{false}, false, false, true, tok};
                    result->properties["host"] = {Value{data->host}, false, false, true, tok};
                    result->properties["method"] = {Value{std::string("tcp")}, false, false, true, tok};
                    result->properties["error"] = {Value{std::string("Timeout")}, false, false, true, tok};
                    result->properties["rtt"] = {Value{static_cast<double>(data->timeout_ms)}, false, false, true, tok};
                    
                    data->promise->state = PromiseValue::State::FULFILLED;
                    data->promise->result = Value{result};
                    
                    for (auto& cb : data->promise->then_callbacks) {
                        try { cb(data->promise->result); } catch(...) {}
                    }
                    
                    if (data->socket) {
                        uv_close((uv_handle_t*)data->socket, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                    }
                    if (data->resolver) {
                        uv_cancel((uv_req_t*)data->resolver);
                    }
                    
                    uv_close((uv_handle_t*)handle, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                    delete data;
                }, timeout_ms, 0);
                
                // DNS resolution first
                data->resolver = new uv_getaddrinfo_t;
                data->resolver->data = data;
                
                struct addrinfo hints;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                
                int r = uv_getaddrinfo(loop, data->resolver, 
                    [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
                        PingData* data = static_cast<PingData*>(req->data);
                        
                        if (status != 0 || !res) {
                            // DNS resolution failed
                            uv_timer_stop(data->timer);
                            uv_close((uv_handle_t*)data->timer, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                            
                            auto result = std::make_shared<ObjectValue>();
                            Token tok;
                            tok.loc = TokenLocation("<net>", 0, 0, 0);
                            
                            result->properties["ok"] = {Value{false}, false, false, true, tok};
                            result->properties["host"] = {Value{data->host}, false, false, true, tok};
                            result->properties["method"] = {Value{std::string("tcp")}, false, false, true, tok};
                            result->properties["error"] = {Value{std::string("DNS resolution failed")}, false, false, true, tok};
                            result->properties["rtt"] = {Value{0.0}, false, false, true, tok};
                            
                            data->promise->state = PromiseValue::State::FULFILLED;
                            data->promise->result = Value{result};
                            
                            for (auto& cb : data->promise->then_callbacks) {
                                try { cb(data->promise->result); } catch(...) {}
                            }
                            
                            uv_close((uv_handle_t*)data->socket, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                            delete data->resolver;
                            delete data;
                            return;
                        }
                        
                        // Start timing after DNS resolution
                        data->start_time = std::chrono::steady_clock::now();
                        
                        // Connect to resolved address
                        struct sockaddr_in addr = *(struct sockaddr_in*)res->ai_addr;
                        addr.sin_port = htons(data->port);
                        
                        uv_freeaddrinfo(res);
                        delete data->resolver;
                        data->resolver = nullptr;
                        
                        uv_connect_t* connect_req = new uv_connect_t;
                        connect_req->data = data;
                        
                        uv_tcp_connect(connect_req, data->socket, (const struct sockaddr*)&addr,
                            [](uv_connect_t* req, int status) {
                                PingData* data = static_cast<PingData*>(req->data);
                                
                                auto end_time = std::chrono::steady_clock::now();
                                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - data->start_time);
                                
                                uv_timer_stop(data->timer);
                                uv_close((uv_handle_t*)data->timer, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                
                                auto result = std::make_shared<ObjectValue>();
                                Token tok;
                                tok.loc = TokenLocation("<net>", 0, 0, 0);
                                
                                result->properties["ok"] = {Value{status == 0}, false, false, true, tok};
                                result->properties["host"] = {Value{data->host}, false, false, true, tok};
                                result->properties["method"] = {Value{std::string("tcp")}, false, false, true, tok};
                                result->properties["rtt"] = {Value{static_cast<double>(duration.count())}, false, false, true, tok};
                                
                                if (status == 0) {
                                    result->properties["error"] = {Value{std::monostate{}}, false, false, true, tok};
                                } else {
                                    result->properties["error"] = {Value{std::string(uv_strerror(status))}, false, false, true, tok};
                                }
                                
                                // ALWAYS fulfill, never reject - let the caller check `ok` field
                                data->promise->state = PromiseValue::State::FULFILLED;
                                data->promise->result = Value{result};
                                
                                for (auto& cb : data->promise->then_callbacks) {
                                    try { cb(data->promise->result); } catch(...) {}
                                }
                                
                                uv_close((uv_handle_t*)data->socket, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                                delete data;
                                delete req;
                            });
                    },
                    data->host.c_str(),
                    std::to_string(data->port).c_str(),
                    &hints);
                
                if (r != 0) {
                    // Immediate failure
                    uv_timer_stop(data->timer);
                    uv_close((uv_handle_t*)data->timer, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                    
                    data->promise->state = PromiseValue::State::REJECTED;
                    data->promise->result = Value{std::string("Failed to start DNS resolution")};
                    
                    for (auto& cb : data->promise->catch_callbacks) {
                        try { cb(data->promise->result); } catch(...) {}
                    }
                    
                    uv_close((uv_handle_t*)data->socket, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                    delete data->resolver;
                    delete data;
                }
            });
            
            return Value{promise}; }, env);
        obj->properties["ping"] = PropertyDescriptor{fn_ping, false, false, true, tok};
    }

    return obj;
}