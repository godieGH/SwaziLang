#include <chrono>
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
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

static std::atomic<int> g_active_net_work{0};
bool net_has_active_work() {
    return g_active_net_work.load() > 0;
}

// Helper function to check ICMP availability
static bool can_use_icmp() {
#ifdef _WIN32
    SOCKET sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        return true;
    }
    return false;
#else
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock >= 0) {
        close(sock);
        return true;
    }
    return false;
#endif
}

// ICMP structures
struct ICMPHeader {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
};

struct ICMPPacket {
    ICMPHeader header;
    uint64_t timestamp;
    char payload[56];
};

// Calculate checksum
static uint16_t calculate_checksum(void* data, size_t len) {
    uint16_t* buf = (uint16_t*)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *buf++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(uint8_t*)buf;
    }

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);

    return (uint16_t)(~sum);
}

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

    // net.unix -> Unix domain socket APIs
    {
        obj->properties["unix"] = PropertyDescriptor{Value{make_unix_socket_exports(env, evaluator)},
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
            
            g_active_net_work.fetch_add(1); 
            
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
                        
                        g_active_net_work.fetch_sub(1);
                        
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
    {
        auto fn_ping = make_native_fn("net.ping", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "ping requires host", token.loc);
        }
        
        std::string host = NetHelpers::value_to_string(args[0]);
        int timeout_ms = 5000;
        int count = 4;
        bool use_icmp = true;
        int tcp_port = 80;
        
        if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[1]);
            
            auto timeout_prop = opts->properties.find("timeout");
            if (timeout_prop != opts->properties.end()) {
                timeout_ms = static_cast<int>(NetHelpers::value_to_number(timeout_prop->second.value));
            }
            
            auto count_prop = opts->properties.find("count");
            if (count_prop != opts->properties.end()) {
                count = static_cast<int>(NetHelpers::value_to_number(count_prop->second.value));
            }
            
            auto method_prop = opts->properties.find("method");
            if (method_prop != opts->properties.end()) {
                std::string method = NetHelpers::value_to_string(method_prop->second.value);
                if (method == "tcp") use_icmp = false;
            }
            
            auto port_prop = opts->properties.find("port");
            if (port_prop != opts->properties.end()) {
                tcp_port = static_cast<int>(NetHelpers::value_to_number(port_prop->second.value));
                use_icmp = false;
            }
        }
        
        if (use_icmp && !can_use_icmp()) {
            use_icmp = false;
        }
        
        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;
        
        g_active_net_work.fetch_add(1);
        
        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            g_active_net_work.fetch_sub(1);
            promise->state = PromiseValue::State::REJECTED;
            promise->result = Value{std::string("No event loop available")};
            return Value{promise};
        }
        
        if (use_icmp) {
            // ==================== ICMP IMPLEMENTATION ====================
            struct ICMPPingData {
                std::shared_ptr<PromiseValue> promise;
                std::string host;
                std::string resolved_ip;
                int timeout_ms;
                int count;
                int current_attempt;
                uint16_t sequence;
                std::vector<double> rtts;
                int successful_pings;
                int failed_pings;
                double dns_time_ms;
                struct sockaddr_in target_addr;
                int raw_socket;
                uv_poll_t* poll_handle;
                uv_timer_t* timeout_timer;
                uv_loop_t* loop;  // ADD LOOP HERE
                std::chrono::steady_clock::time_point dns_start;
                std::chrono::steady_clock::time_point send_time;
                bool waiting_for_reply;
            };
            
            auto* data = new ICMPPingData{
                promise, host, "", timeout_ms, count, 0, 
                static_cast<uint16_t>(getpid() & 0xFFFF),
                {}, 0, 0, 0.0, {}, -1, nullptr, nullptr, loop,  // ADD loop HERE
                std::chrono::steady_clock::now(),
                std::chrono::steady_clock::now(),
                false
            };
            
            scheduler_run_on_loop([data]() {  // ONLY capture data
                uv_getaddrinfo_t* resolver = new uv_getaddrinfo_t;
                resolver->data = data;
                
                struct addrinfo hints;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_RAW;
                hints.ai_protocol = IPPROTO_ICMP;
                
                int r = uv_getaddrinfo(data->loop, resolver,
                    [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) {  // NO CAPTURES
                        ICMPPingData* data = static_cast<ICMPPingData*>(req->data);
                        
                        if (status != 0 || !res) {
                            g_active_net_work.fetch_sub(1);
                            
                            auto result = std::make_shared<ObjectValue>();
                            Token tok;
                            tok.loc = TokenLocation("<net>", 0, 0, 0);
                            
                            result->properties["ok"] = {Value{false}, false, false, true, tok};
                            result->properties["host"] = {Value{data->host}, false, false, true, tok};
                            result->properties["error"] = {Value{std::string("DNS resolution failed")}, false, false, true, tok};
                            
                            data->promise->state = PromiseValue::State::FULFILLED;
                            data->promise->result = Value{result};
                            
                            for (auto& cb : data->promise->then_callbacks) {
                                try { cb(data->promise->result); } catch(...) {}
                            }
                            
                            delete req;
                            delete data;
                            return;
                        }
                        
                        auto dns_end = std::chrono::steady_clock::now();
                        auto dns_duration = std::chrono::duration_cast<std::chrono::microseconds>(dns_end - data->dns_start);
                        data->dns_time_ms = dns_duration.count() / 1000.0;
                        
                        data->target_addr = *(struct sockaddr_in*)res->ai_addr;
                        
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &data->target_addr.sin_addr, ip, sizeof(ip));
                        data->resolved_ip = ip;
                        
                        uv_freeaddrinfo(res);
                        delete req;

                        // Create raw socket
#ifdef _WIN32
                        data->raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
#else
                        data->raw_socket = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
                        int flags = fcntl(data->raw_socket, F_GETFL, 0);
                        fcntl(data->raw_socket, F_SETFL, flags | O_NONBLOCK);
#endif
                        
                        if (data->raw_socket < 0) {
                            g_active_net_work.fetch_sub(1);
                            
                            auto result = std::make_shared<ObjectValue>();
                            Token tok;
                            tok.loc = TokenLocation("<net>", 0, 0, 0);
                            
                            result->properties["ok"] = {Value{false}, false, false, true, tok};
                            result->properties["host"] = {Value{data->host}, false, false, true, tok};
                            result->properties["error"] = {Value{std::string("Failed to create raw socket (requires root/admin)")}, false, false, true, tok};
                            
                            data->promise->state = PromiseValue::State::FULFILLED;
                            data->promise->result = Value{result};
                            
                            for (auto& cb : data->promise->then_callbacks) {
                                try { cb(data->promise->result); } catch(...) {}
                            }
                            
                            delete data;
                            return;
                        }
                        
                        // Setup poll
                        data->poll_handle = new uv_poll_t;
                        data->poll_handle->data = data;
                        uv_poll_init_socket(data->loop, data->poll_handle, data->raw_socket);
                        
                        // Start poll for reading
                        uv_poll_start(data->poll_handle, UV_READABLE, [](uv_poll_t* handle, int status, int events) {
                            ICMPPingData* data = static_cast<ICMPPingData*>(handle->data);
                            
                            if (status < 0 || !(events & UV_READABLE) || !data->waiting_for_reply) return;
                            
                            char buffer[1024];
                            struct sockaddr_in from;
                            socklen_t fromlen = sizeof(from);
                            
                            ssize_t received = recvfrom(data->raw_socket, buffer, sizeof(buffer), 0,
                                (struct sockaddr*)&from, &fromlen);
                            
                            if (received < 28) return;
                            
                            ICMPHeader* reply = (ICMPHeader*)(buffer + 20);
                            
                            if (reply->type == 0) {
                                auto now = std::chrono::steady_clock::now();
                                auto duration = std::chrono::duration_cast<std::chrono::microseconds>(now - data->send_time);
                                double rtt_ms = duration.count() / 1000.0;
                                
                                data->successful_pings++;
                                data->rtts.push_back(rtt_ms);
                                data->waiting_for_reply = false;
                                
                                if (data->timeout_timer) {
                                    uv_timer_stop(data->timeout_timer);
                                    uv_close((uv_handle_t*)data->timeout_timer, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                    data->timeout_timer = nullptr;
                                }
                            }
                        });
                        
                        // Ping sending function
                        std::function<void()>* send_ping = new std::function<void()>();
                        *send_ping = [data, send_ping]() {  // NOW SAFE - data has loop
                            if (data->current_attempt >= data->count) {
                                g_active_net_work.fetch_sub(1);
                                
                                uv_poll_stop(data->poll_handle);
                                uv_close((uv_handle_t*)data->poll_handle, [](uv_handle_t* h) { delete (uv_poll_t*)h; });
#ifdef _WIN32
                                closesocket(data->raw_socket);
#else
                                close(data->raw_socket);
#endif
                                
                                auto result = std::make_shared<ObjectValue>();
                                Token tok;
                                tok.loc = TokenLocation("<net>", 0, 0, 0);
                                
                                result->properties["ok"] = {Value{data->successful_pings > 0}, false, false, true, tok};
                                result->properties["host"] = {Value{data->host}, false, false, true, tok};
                                result->properties["ip"] = {Value{data->resolved_ip}, false, false, true, tok};
                                result->properties["method"] = {Value{std::string("icmp")}, false, false, true, tok};
                                result->properties["dnsTime"] = {Value{data->dns_time_ms}, false, false, true, tok};
                                result->properties["count"] = {Value{static_cast<double>(data->count)}, false, false, true, tok};
                                result->properties["successful"] = {Value{static_cast<double>(data->successful_pings)}, false, false, true, tok};
                                result->properties["failed"] = {Value{static_cast<double>(data->failed_pings)}, false, false, true, tok};
                                
                                if (!data->rtts.empty()) {
                                    double sum = 0, min_rtt = data->rtts[0], max_rtt = data->rtts[0];
                                    for (double rtt : data->rtts) {
                                        sum += rtt;
                                        if (rtt < min_rtt) min_rtt = rtt;
                                        if (rtt > max_rtt) max_rtt = rtt;
                                    }
                                    double avg = sum / data->rtts.size();
                                    
                                    result->properties["min"] = {Value{min_rtt}, false, false, true, tok};
                                    result->properties["max"] = {Value{max_rtt}, false, false, true, tok};
                                    result->properties["avg"] = {Value{avg}, false, false, true, tok};
                                    
                                    double loss = (data->failed_pings * 100.0) / data->count;
                                    result->properties["loss"] = {Value{loss}, false, false, true, tok};
                                } else {
                                    result->properties["loss"] = {Value{100.0}, false, false, true, tok};
                                }
                                
                                data->promise->state = PromiseValue::State::FULFILLED;
                                data->promise->result = Value{result};
                                
                                for (auto& cb : data->promise->then_callbacks) {
                                    try { cb(data->promise->result); } catch(...) {}
                                }
                                
                                delete send_ping;
                                delete data;
                                return;
                            }
                            
                            // Build and send ICMP packet
                            ICMPPacket packet;
                            memset(&packet, 0, sizeof(packet));
                            
                            packet.header.type = 8;
                            packet.header.code = 0;
                            packet.header.id = htons(getpid() & 0xFFFF);
                            packet.header.sequence = htons(data->sequence++);
                            
                            auto now = std::chrono::steady_clock::now();
                            packet.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
                            
                            for (int i = 0; i < 56; i++) {
                                packet.payload[i] = i;
                            }
                            
                            packet.header.checksum = 0;
                            packet.header.checksum = calculate_checksum(&packet, sizeof(packet));
                            
                            data->send_time = std::chrono::steady_clock::now();
                            data->waiting_for_reply = true;
                            
                            ssize_t sent = sendto(data->raw_socket, (char*)&packet, sizeof(packet), 0,
                                (struct sockaddr*)&data->target_addr, sizeof(data->target_addr));
                            
                            data->current_attempt++;
                            
                            if (sent < 0) {
                                data->failed_pings++;
                                data->waiting_for_reply = false;
                                
                                uv_timer_t* timer = new uv_timer_t;
                                timer->data = send_ping;
                                uv_timer_init(data->loop, timer);
                                uv_timer_start(timer, [](uv_timer_t* t) {
                                    auto* fn = static_cast<std::function<void()>*>(t->data);
                                    (*fn)();
                                    uv_close((uv_handle_t*)t, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                }, 1000, 0);
                                return;
                            }
                            
                            // Setup timeout
                            data->timeout_timer = new uv_timer_t;
                            struct TimeoutData {
                                ICMPPingData* ping_data;
                                std::function<void()>* send_ping;
                            };
                            auto* td = new TimeoutData{data, send_ping};
                            data->timeout_timer->data = td;
                            uv_timer_init(data->loop, data->timeout_timer);
                            
                            uv_timer_start(data->timeout_timer, [](uv_timer_t* handle) {  // NO CAPTURES
                                auto* td = static_cast<TimeoutData*>(handle->data);
                                ICMPPingData* data = td->ping_data;
                                
                                if (data->waiting_for_reply) {
                                    data->failed_pings++;
                                    data->waiting_for_reply = false;
                                }
                                
                                uv_close((uv_handle_t*)handle, [](uv_handle_t* h) {
                                    auto* td = static_cast<TimeoutData*>(h->data);
                                    delete td;
                                    delete (uv_timer_t*)h;
                                });
                                
                                data->timeout_timer = nullptr;
                                
                                // Schedule next ping
                                uv_timer_t* timer = new uv_timer_t;
                                timer->data = td->send_ping;
                                uv_timer_init(data->loop, timer);
                                uv_timer_start(timer, [](uv_timer_t* t) {
                                    auto* fn = static_cast<std::function<void()>*>(t->data);
                                    (*fn)();
                                    uv_close((uv_handle_t*)t, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                }, 1000, 0);
                            }, data->timeout_ms, 0);
                        };
                        
                        (*send_ping)();
                    },
                    data->host.c_str(), nullptr, &hints);
                
                if (r != 0) {
                    g_active_net_work.fetch_sub(1);
                    
                    data->promise->state = PromiseValue::State::REJECTED;
                    data->promise->result = Value{std::string("Failed to start DNS resolution")};
                    
                    for (auto& cb : data->promise->catch_callbacks) {
                        try { cb(data->promise->result); } catch(...) {}
                    }
                    
                    delete data;
                }
            });
            
        } else {
            // ==================== TCP FALLBACK ====================
            struct TCPPingData {
                std::shared_ptr<PromiseValue> promise;
                std::string host;
                std::string resolved_ip;
                int port;
                int timeout_ms;
                int count;
                int current_attempt;
                std::vector<double> rtts;
                int successful_pings;
                int failed_pings;
                double dns_time_ms;
                struct sockaddr_in target_addr;
                bool dns_resolved;
                uv_loop_t* loop;  // ADD LOOP HERE
                std::chrono::steady_clock::time_point dns_start;
                std::chrono::steady_clock::time_point ping_start;
            };
            
            auto* data = new TCPPingData{
                promise, host, "", tcp_port, timeout_ms, count, 0,
                {}, 0, 0, 0.0, {}, false, loop,  // ADD loop HERE
                std::chrono::steady_clock::now(),
                std::chrono::steady_clock::now()
            };
            
            scheduler_run_on_loop([data]() {  // ONLY capture data
                uv_getaddrinfo_t* resolver = new uv_getaddrinfo_t;
                resolver->data = data;
                
                struct addrinfo hints;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;
                
                int r = uv_getaddrinfo(data->loop, resolver,
                    [](uv_getaddrinfo_t* req, int status, struct addrinfo* res) {  // NO CAPTURES
                        TCPPingData* data = static_cast<TCPPingData*>(req->data);
                        
                        if (status != 0 || !res) {
                            g_active_net_work.fetch_sub(1);
                            
                            auto result = std::make_shared<ObjectValue>();
                            Token tok;
                            tok.loc = TokenLocation("<net>", 0, 0, 0);
                            
                            result->properties["ok"] = {Value{false}, false, false, true, tok};
                            result->properties["host"] = {Value{data->host}, false, false, true, tok};
                            result->properties["error"] = {Value{std::string("DNS resolution failed")}, false, false, true, tok};
                            
                            data->promise->state = PromiseValue::State::FULFILLED;
                            data->promise->result = Value{result};
                            
                            for (auto& cb : data->promise->then_callbacks) {
                                try { cb(data->promise->result); } catch(...) {}
                            }
                            
                            delete req;
                            delete data;
                            return;
                        }
                        
                        auto dns_end = std::chrono::steady_clock::now();
                        auto dns_duration = std::chrono::duration_cast<std::chrono::microseconds>(dns_end - data->dns_start);
                        data->dns_time_ms = dns_duration.count() / 1000.0;
                        
                        data->target_addr = *(struct sockaddr_in*)res->ai_addr;
                        data->target_addr.sin_port = htons(data->port);
                        
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &data->target_addr.sin_addr, ip, sizeof(ip));
                        data->resolved_ip = ip;
                        
                        uv_freeaddrinfo(res);
                        delete req;
                        data->dns_resolved = true;
                        
                        std::function<void()>* ping_once = new std::function<void()>();
                        *ping_once = [data, ping_once]() {  // NOW SAFE - data has loop
                            if (data->current_attempt >= data->count) {
                                g_active_net_work.fetch_sub(1);
                                
                                auto result = std::make_shared<ObjectValue>();
                                Token tok;
                                tok.loc = TokenLocation("<net>", 0, 0, 0);
                                
                                result->properties["ok"] = {Value{data->successful_pings > 0}, false, false, true, tok};
                                result->properties["host"] = {Value{data->host}, false, false, true, tok};
                                result->properties["ip"] = {Value{data->resolved_ip}, false, false, true, tok};
                                result->properties["method"] = {Value{std::string("tcp")}, false, false, true, tok};
                                result->properties["dnsTime"] = {Value{data->dns_time_ms}, false, false, true, tok};
                                result->properties["count"] = {Value{static_cast<double>(data->count)}, false, false, true, tok};
                                result->properties["successful"] = {Value{static_cast<double>(data->successful_pings)}, false, false, true, tok};
                                result->properties["failed"] = {Value{static_cast<double>(data->failed_pings)}, false, false, true, tok};
                                
                                if (!data->rtts.empty()) {
                                    double sum = 0, min_rtt = data->rtts[0], max_rtt = data->rtts[0];
                                    for (double rtt : data->rtts) {
                                        sum += rtt;
                                        if (rtt < min_rtt) min_rtt = rtt;
                                        if (rtt > max_rtt) max_rtt = rtt;
                                    }
                                    double avg = sum / data->rtts.size();
                                    
                                    result->properties["min"] = {Value{min_rtt}, false, false, true, tok};
                                    result->properties["max"] = {Value{max_rtt}, false, false, true, tok};
                                    result->properties["avg"] = {Value{avg}, false, false, true, tok};
                                    
                                    double loss = (data->failed_pings * 100.0) / data->count;
                                    result->properties["loss"] = {Value{loss}, false, false, true, tok};
                                } else {
                                    result->properties["loss"] = {Value{100.0}, false, false, true, tok};
                                }
                                
                                data->promise->state = PromiseValue::State::FULFILLED;
                                data->promise->result = Value{result};
                                
                                for (auto& cb : data->promise->then_callbacks) {
                                    try { cb(data->promise->result); } catch(...) {}
                                }
                                
                                delete ping_once;
                                delete data;
                                return;
                            }
                            
                            data->current_attempt++;
                            data->ping_start = std::chrono::steady_clock::now();
                            
                            uv_tcp_t* socket = new uv_tcp_t;
                            uv_tcp_init(data->loop, socket);
                            
                            uv_timer_t* timer = new uv_timer_t;
                            uv_timer_init(data->loop, timer);
                            
                            struct PingContext {
                                TCPPingData* data;
                                std::function<void()>* ping_once;
                                uv_tcp_t* socket;
                                uv_timer_t* timer;
                                bool completed;
                            };
                            
                            PingContext* ctx = new PingContext{data, ping_once, socket, timer, false};
                            timer->data = ctx;
                            socket->data = ctx;
                            
                            uv_timer_start(timer, [](uv_timer_t* handle) {  // NO CAPTURES
                                PingContext* ctx = static_cast<PingContext*>(handle->data);
                                if (ctx->completed) {
                                    delete ctx;
                                    return;
                                }
                                ctx->completed = true;
                                ctx->data->failed_pings++;
                                
                                uv_close((uv_handle_t*)ctx->socket, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                                uv_close((uv_handle_t*)handle, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                
                                uv_timer_t* interval = new uv_timer_t;
                                interval->data = ctx->ping_once;
                                uv_timer_init(ctx->data->loop, interval);
                                uv_timer_start(interval, [](uv_timer_t* t) {
                                    auto* fn = static_cast<std::function<void()>*>(t->data);
                                    (*fn)();
                                    uv_close((uv_handle_t*)t, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                }, 1000, 0);
                                
                                delete ctx;
                            }, data->timeout_ms, 0);
                            
                            uv_connect_t* connect_req = new uv_connect_t;
                            connect_req->data = ctx;
                            
                            uv_tcp_connect(connect_req, socket, (const struct sockaddr*)&data->target_addr,
                                [](uv_connect_t* req, int status) {  // NO CAPTURES
                                    PingContext* ctx = static_cast<PingContext*>(req->data);
                                    if (ctx->completed) {
                                        delete req;
                                        return;
                                    }
                                    ctx->completed = true;
                                    
                                    auto end = std::chrono::steady_clock::now();
                                    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - ctx->data->ping_start);
                                    double rtt_ms = duration.count() / 1000.0;
                                    
                                    uv_timer_stop(ctx->timer);
                                    uv_close((uv_handle_t*)ctx->timer, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                    
                                    if (status == 0) {
                                        ctx->data->successful_pings++;
                                        ctx->data->rtts.push_back(rtt_ms);
                                    } else {
                                        ctx->data->failed_pings++;
                                    }
                                    
                                    uv_close((uv_handle_t*)ctx->socket, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                                    
                                    uv_timer_t* interval = new uv_timer_t;
                                    interval->data = ctx->ping_once;
                                    uv_timer_init(ctx->data->loop, interval);
                                    uv_timer_start(interval, [](uv_timer_t* t) {
                                        auto* fn = static_cast<std::function<void()>*>(t->data);
                                        (*fn)();
                                        uv_close((uv_handle_t*)t, [](uv_handle_t* h) { delete (uv_timer_t*)h; });
                                    }, 1000, 0);
                                    
                                    delete ctx;
                                    delete req;
                                });
                        };
                        
                        (*ping_once)();
                    },
                    data->host.c_str(),
                    std::to_string(data->port).c_str(),
                    &hints);
                
                if (r != 0) {
                    g_active_net_work.fetch_sub(1);
                    
                    data->promise->state = PromiseValue::State::REJECTED;
                    data->promise->result = Value{std::string("Failed to start DNS resolution")};
                    
                    for (auto& cb : data->promise->catch_callbacks) {
                        try { cb(data->promise->result); } catch(...) {}
                    }
                    
                    delete data;
                }
            });
        }
        
        return Value{promise}; }, env);

        obj->properties["ping"] = PropertyDescriptor{fn_ping, false, false, true, tok};
    }

    return obj;
}