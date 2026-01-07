#include "uv.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "./uv.hpp"
#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"

// Helper to coerce Value -> string
static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Helper: create native function
template <typename F>
static FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    return std::make_shared<FunctionValue>(name, native_impl, env, Token());
}

// Track active handles for cleanup
static std::mutex g_uv_handles_mutex;
static std::unordered_map<void*, std::shared_ptr<void>> g_uv_handles;

std::shared_ptr<ObjectValue> make_uv_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // uv.version() -> string
    {
        auto fn = make_native_fn("uv.version", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{std::string(uv_version_string())}; }, env);
        obj->properties["version"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.now() -> number (milliseconds)
    {
        auto fn = make_native_fn("uv.now", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            return Value{static_cast<double>(uv_now(loop))}; }, env);
        obj->properties["now"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.hrtime() -> number (nanoseconds)
    {
        auto fn = make_native_fn("uv.hrtime", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{static_cast<double>(uv_hrtime())}; }, env);
        obj->properties["hrtime"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.sleep(ms) -> undefined (blocks current thread)
    {
        auto fn = make_native_fn("uv.sleep", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<double>(args[0])) {
                throw SwaziError("TypeError", "uv.sleep requires milliseconds argument", token.loc);
            }
            unsigned int ms = static_cast<unsigned int>(std::get<double>(args[0]));
            uv_sleep(ms);
            return std::monostate{}; }, env);
        obj->properties["sleep"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.cpuInfo() -> array of cpu info objects
    {
        auto fn = make_native_fn("uv.cpuInfo", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            uv_cpu_info_t* cpu_infos;
            int count;
            int r = uv_cpu_info(&cpu_infos, &count);
            
            auto arr = std::make_shared<ArrayValue>();
            if (r == 0) {
                for (int i = 0; i < count; i++) {
                    auto cpu = std::make_shared<ObjectValue>();
                    Token tok;
                    
                    cpu->properties["model"] = PropertyDescriptor{
                        Value{std::string(cpu_infos[i].model)}, false, false, true, tok};
                    cpu->properties["speed"] = PropertyDescriptor{
                        Value{static_cast<double>(cpu_infos[i].speed)}, false, false, true, tok};
                    
                    auto times = std::make_shared<ObjectValue>();
                    times->properties["user"] = PropertyDescriptor{
                        Value{static_cast<double>(cpu_infos[i].cpu_times.user)}, false, false, true, tok};
                    times->properties["nice"] = PropertyDescriptor{
                        Value{static_cast<double>(cpu_infos[i].cpu_times.nice)}, false, false, true, tok};
                    times->properties["sys"] = PropertyDescriptor{
                        Value{static_cast<double>(cpu_infos[i].cpu_times.sys)}, false, false, true, tok};
                    times->properties["idle"] = PropertyDescriptor{
                        Value{static_cast<double>(cpu_infos[i].cpu_times.idle)}, false, false, true, tok};
                    times->properties["irq"] = PropertyDescriptor{
                        Value{static_cast<double>(cpu_infos[i].cpu_times.irq)}, false, false, true, tok};
                    
                    cpu->properties["times"] = PropertyDescriptor{Value{times}, false, false, true, tok};
                    arr->elements.push_back(Value{cpu});
                }
                uv_free_cpu_info(cpu_infos, count);
            }
            return Value{arr}; }, env);
        obj->properties["cpuInfo"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.interfaceAddresses() -> array of network interface objects
    {
        auto fn = make_native_fn("uv.interfaceAddresses", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            uv_interface_address_t* addresses;
            int count;
            int r = uv_interface_addresses(&addresses, &count);
            
            auto arr = std::make_shared<ArrayValue>();
            if (r == 0) {
                for (int i = 0; i < count; i++) {
                    auto iface = std::make_shared<ObjectValue>();
                    Token tok;
                    
                    iface->properties["name"] = PropertyDescriptor{
                        Value{std::string(addresses[i].name)}, false, false, true, tok};
                    iface->properties["internal"] = PropertyDescriptor{
                        Value{addresses[i].is_internal != 0}, false, false, true, tok};
                    
                    // Physical address (MAC)
                    char phys[18];
                    snprintf(phys, sizeof(phys), "%02x:%02x:%02x:%02x:%02x:%02x",
                        (unsigned char)addresses[i].phys_addr[0],
                        (unsigned char)addresses[i].phys_addr[1],
                        (unsigned char)addresses[i].phys_addr[2],
                        (unsigned char)addresses[i].phys_addr[3],
                        (unsigned char)addresses[i].phys_addr[4],
                        (unsigned char)addresses[i].phys_addr[5]);
                    iface->properties["mac"] = PropertyDescriptor{Value{std::string(phys)}, false, false, true, tok};
                    
                    // IP address
                    char ip[INET6_ADDRSTRLEN];
                    if (addresses[i].address.address4.sin_family == AF_INET) {
                        uv_ip4_name(&addresses[i].address.address4, ip, sizeof(ip));
                        iface->properties["family"] = PropertyDescriptor{Value{std::string("IPv4")}, false, false, true, tok};
                    } else if (addresses[i].address.address4.sin_family == AF_INET6) {
                        uv_ip6_name(&addresses[i].address.address6, ip, sizeof(ip));
                        iface->properties["family"] = PropertyDescriptor{Value{std::string("IPv6")}, false, false, true, tok};
                    }
                    iface->properties["address"] = PropertyDescriptor{Value{std::string(ip)}, false, false, true, tok};
                    
                    // Netmask
                    char netmask[INET6_ADDRSTRLEN];
                    if (addresses[i].netmask.netmask4.sin_family == AF_INET) {
                        uv_ip4_name(&addresses[i].netmask.netmask4, netmask, sizeof(netmask));
                    } else if (addresses[i].netmask.netmask4.sin_family == AF_INET6) {
                        uv_ip6_name(&addresses[i].netmask.netmask6, netmask, sizeof(netmask));
                    }
                    iface->properties["netmask"] = PropertyDescriptor{Value{std::string(netmask)}, false, false, true, tok};
                    
                    arr->elements.push_back(Value{iface});
                }
                uv_free_interface_addresses(addresses, count);
            }
            return Value{arr}; }, env);
        obj->properties["interfaceAddresses"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.loadavg() -> array[3] of load averages
    {
        auto fn = make_native_fn("uv.loadavg", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            double avg[3];
            uv_loadavg(avg);
            auto arr = std::make_shared<ArrayValue>();
            arr->elements.push_back(Value{avg[0]});
            arr->elements.push_back(Value{avg[1]});
            arr->elements.push_back(Value{avg[2]});
            return Value{arr}; }, env);
        obj->properties["loadavg"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.getrusage() -> resource usage object
    {
        auto fn = make_native_fn("uv.getrusage", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_rusage_t rusage;
            int r = uv_getrusage(&rusage);
            if (r != 0) {
                throw SwaziError("SystemError", "Failed to get resource usage", token.loc);
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            obj->properties["utime"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_utime.tv_sec) + 
                     static_cast<double>(rusage.ru_utime.tv_usec) / 1e6}, 
                false, false, true, tok};
            obj->properties["stime"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_stime.tv_sec) + 
                     static_cast<double>(rusage.ru_stime.tv_usec) / 1e6}, 
                false, false, true, tok};
            obj->properties["maxrss"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_maxrss)}, false, false, true, tok};
            obj->properties["ixrss"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_ixrss)}, false, false, true, tok};
            obj->properties["idrss"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_idrss)}, false, false, true, tok};
            obj->properties["isrss"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_isrss)}, false, false, true, tok};
            obj->properties["minflt"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_minflt)}, false, false, true, tok};
            obj->properties["majflt"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_majflt)}, false, false, true, tok};
            obj->properties["nswap"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_nswap)}, false, false, true, tok};
            obj->properties["inblock"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_inblock)}, false, false, true, tok};
            obj->properties["oublock"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_oublock)}, false, false, true, tok};
            obj->properties["msgsnd"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_msgsnd)}, false, false, true, tok};
            obj->properties["msgrcv"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_msgrcv)}, false, false, true, tok};
            obj->properties["nsignals"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_nsignals)}, false, false, true, tok};
            obj->properties["nvcsw"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_nvcsw)}, false, false, true, tok};
            obj->properties["nivcsw"] = PropertyDescriptor{
                Value{static_cast<double>(rusage.ru_nivcsw)}, false, false, true, tok};
            
            return Value{obj}; }, env);
        obj->properties["getrusage"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.uptime() -> number (seconds)
    {
        auto fn = make_native_fn("uv.uptime", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            double uptime;
            int r = uv_uptime(&uptime);
            if (r != 0) {
                throw SwaziError("SystemError", "Failed to get uptime", token.loc);
            }
            return Value{uptime}; }, env);
        obj->properties["uptime"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.residentSetMemory() -> number (bytes)
    {
        auto fn = make_native_fn("uv.residentSetMemory", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            size_t rss;
            int r = uv_resident_set_memory(&rss);
            if (r != 0) {
                throw SwaziError("SystemError", "Failed to get RSS", token.loc);
            }
            return Value{static_cast<double>(rss)}; }, env);
        obj->properties["residentSetMemory"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.getTotalMemory() -> number (bytes)
    {
        auto fn = make_native_fn("uv.getTotalMemory", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{static_cast<double>(uv_get_total_memory())}; }, env);
        obj->properties["getTotalMemory"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.getFreeMemory() -> number (bytes)
    {
        auto fn = make_native_fn("uv.getFreeMemory", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{static_cast<double>(uv_get_free_memory())}; }, env);
        obj->properties["getFreeMemory"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.getConstrainedMemory() -> number (bytes, or 0 if unlimited)
    {
        auto fn = make_native_fn("uv.getConstrainedMemory", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value { return Value{static_cast<double>(uv_get_constrained_memory())}; }, env);
        obj->properties["getConstrainedMemory"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.getPriority(pid) -> number (priority value)
    {
        auto fn = make_native_fn("uv.getPriority", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            int pid = 0;
            if (!args.empty() && std::holds_alternative<double>(args[0])) {
                pid = static_cast<int>(std::get<double>(args[0]));
            }
            int priority;
            int r = uv_os_getpriority(pid, &priority);
            if (r != 0) {
                throw SwaziError("SystemError", std::string("Failed to get priority: ") + uv_strerror(r), token.loc);
            }
            return Value{static_cast<double>(priority)}; }, env);
        obj->properties["getPriority"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.setPriority(pid, priority) -> undefined
    {
        auto fn = make_native_fn("uv.setPriority", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "uv.setPriority requires (pid, priority)", token.loc);
            }
            int pid = static_cast<int>(std::get<double>(args[0]));
            int priority = static_cast<int>(std::get<double>(args[1]));
            
            int r = uv_os_setpriority(pid, priority);
            if (r != 0) {
                throw SwaziError("SystemError", std::string("Failed to set priority: ") + uv_strerror(r), token.loc);
            }
            return std::monostate{}; }, env);
        obj->properties["setPriority"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.random(size) -> Buffer (cryptographically secure random bytes)
    {
        auto fn = make_native_fn("uv.random", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<double>(args[0])) {
                throw SwaziError("TypeError", "uv.random requires size argument", token.loc);
            }
            
            size_t size = static_cast<size_t>(std::get<double>(args[0]));
            if (size > 65536) {
                throw SwaziError("RangeError", "uv.random size must be <= 65536", token.loc);
            }
            
            auto buf = std::make_shared<BufferValue>();
            buf->data.resize(size);
            
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                throw SwaziError("RuntimeError", "No event loop available", token.loc);
            }
            
            int r = uv_random(loop, nullptr, buf->data.data(), size, 0, nullptr);
            if (r != 0) {
                throw SwaziError("SystemError", std::string("uv.random failed: ") + uv_strerror(r), token.loc);
            }
            
            buf->encoding = "binary";
            return Value{buf}; }, env);
        obj->properties["random"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // ============================================================================
    // EVENT LOOP CONTROL
    // ============================================================================

    // uv.run(mode?) -> bool
    // Runs the event loop. Mode: "default" (0), "once" (1), "nowait" (2)
    {
        auto fn = make_native_fn("uv.run", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            uv_run_mode mode = UV_RUN_DEFAULT;
            if (!args.empty() && std::holds_alternative<std::string>(args[0])) {
                std::string mode_str = std::get<std::string>(args[0]);
                if (mode_str == "once") mode = UV_RUN_ONCE;
                else if (mode_str == "nowait") mode = UV_RUN_NOWAIT;
            } else if (!args.empty() && std::holds_alternative<double>(args[0])) {
                int m = static_cast<int>(std::get<double>(args[0]));
                if (m >= 0 && m <= 2) mode = static_cast<uv_run_mode>(m);
            }
            
            int result = uv_run(loop, mode);
            return Value{result != 0}; }, env);
        obj->properties["run"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.stop() -> undefined
    // Stops the event loop
    {
        auto fn = make_native_fn("uv.stop", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            uv_stop(loop);
            return std::monostate{}; }, env);
        obj->properties["stop"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.isAlive() -> bool
    // Returns true if there are active handles or requests
    {
        auto fn = make_native_fn("uv.isAlive", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            return Value{uv_loop_alive(loop) != 0}; }, env);
        obj->properties["isAlive"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.updateTime() -> undefined
    // Updates the event loop's concept of "now"
    {
        auto fn = make_native_fn("uv.updateTime", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            uv_update_time(loop);
            return std::monostate{}; }, env);
        obj->properties["updateTime"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.backendTimeout() -> number
    // Get timeout for next iteration (milliseconds, or -1 for indefinite)
    {
        auto fn = make_native_fn("uv.backendTimeout", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            return Value{static_cast<double>(uv_backend_timeout(loop))}; }, env);
        obj->properties["backendTimeout"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // uv.backendFd() -> number
    // Get backend file descriptor (Unix only, -1 on Windows)
    {
        auto fn = make_native_fn("uv.backendFd", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            return Value{static_cast<double>(uv_backend_fd(loop))}; }, env);
        obj->properties["backendFd"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // ============================================================================
    // TIMER HANDLE (Raw libuv timer with full control)
    // ============================================================================

    // uv.Timer() -> timer handle object
    {
        auto fn = make_native_fn("uv.Timer", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            struct TimerHandle {
                uv_timer_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
                long long id;
            };
            
            auto timer = std::make_shared<TimerHandle>();
            timer->id = g_next_worker_id.fetch_add(1);
            timer->handle = new uv_timer_t;
            timer->handle->data = timer.get();
            
            int r = uv_timer_init(loop, timer->handle);
            if (r != 0) {
                delete timer->handle;
                throw SwaziError("SystemError", std::string("uv_timer_init failed: ") + uv_strerror(r), token.loc);
            }
            
            // Store in global registry
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[timer->handle] = timer;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // timer.start(timeout, repeat, callback) -> undefined
            auto start_fn = make_native_fn("timer.start", [timer](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.size() < 3 || !std::holds_alternative<FunctionPtr>(args[2])) {
                    throw SwaziError("TypeError", "timer.start requires (timeout, repeat, callback)", token.loc);
                }
                
                uint64_t timeout = static_cast<uint64_t>(std::get<double>(args[0]));
                uint64_t repeat = static_cast<uint64_t>(std::get<double>(args[1]));
                timer->callback = std::get<FunctionPtr>(args[2]);
                
                auto timer_cb = [](uv_timer_t* handle) {
                    TimerHandle* t = static_cast<TimerHandle*>(handle->data);
                    if (t && !t->closed.load() && t->callback) {
                        CallbackPayload* p = new CallbackPayload(t->callback, {});
                        enqueue_callback_global(static_cast<void*>(p));
                    }
                };
                
                int r = uv_timer_start(timer->handle, timer_cb, timeout, repeat);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_timer_start failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["start"] = PropertyDescriptor{start_fn, false, false, false, tok};
            
            // timer.stop() -> undefined
            auto stop_fn = make_native_fn("timer.stop", [timer](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_timer_stop(timer->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_timer_stop failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["stop"] = PropertyDescriptor{stop_fn, false, false, false, tok};
            
            // timer.again() -> undefined (restart with repeat value)
            auto again_fn = make_native_fn("timer.again", [timer](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_timer_again(timer->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_timer_again failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["again"] = PropertyDescriptor{again_fn, false, false, false, tok};
            
            // timer.setRepeat(repeat) -> undefined
            auto set_repeat_fn = make_native_fn("timer.setRepeat", [timer](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty() || !std::holds_alternative<double>(args[0])) {
                    throw SwaziError("TypeError", "setRepeat requires numeric milliseconds", token.loc);
                }
                uint64_t repeat = static_cast<uint64_t>(std::get<double>(args[0]));
                uv_timer_set_repeat(timer->handle, repeat);
                return std::monostate{};
            }, nullptr);
            obj->properties["setRepeat"] = PropertyDescriptor{set_repeat_fn, false, false, false, tok};
            
            // timer.getRepeat() -> number
            auto get_repeat_fn = make_native_fn("timer.getRepeat", [timer](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                return Value{static_cast<double>(uv_timer_get_repeat(timer->handle))};
            }, nullptr);
            obj->properties["getRepeat"] = PropertyDescriptor{get_repeat_fn, false, false, false, tok};
            
            // timer.getDueIn() -> number (milliseconds until next timeout)
            auto get_due_in_fn = make_native_fn("timer.getDueIn", [timer](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                return Value{static_cast<double>(uv_timer_get_due_in(timer->handle))};
            }, nullptr);
            obj->properties["getDueIn"] = PropertyDescriptor{get_due_in_fn, false, false, false, tok};
            
            // timer.close() -> undefined
            auto close_fn = make_native_fn("timer.close", [timer](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!timer->closed.exchange(true)) {
                    uv_timer_stop(timer->handle);
                    uv_close((uv_handle_t*)timer->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_timer_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Timer"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // IDLE HANDLE (Runs callback when loop has no other work)
    // ============================================================================

    // uv.Idle() -> idle handle object
    {
        auto fn = make_native_fn("uv.Idle", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            struct IdleHandle {
                uv_idle_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
            };
            
            auto idle = std::make_shared<IdleHandle>();
            idle->handle = new uv_idle_t;
            idle->handle->data = idle.get();
            
            int r = uv_idle_init(loop, idle->handle);
            if (r != 0) {
                delete idle->handle;
                throw SwaziError("SystemError", std::string("uv_idle_init failed: ") + uv_strerror(r), token.loc);
            }
            
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[idle->handle] = idle;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // idle.start(callback) -> undefined
            auto start_fn = make_native_fn("idle.start", [idle](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                    throw SwaziError("TypeError", "idle.start requires callback", token.loc);
                }
                
                idle->callback = std::get<FunctionPtr>(args[0]);
                
                auto idle_cb = [](uv_idle_t* handle) {
                    IdleHandle* i = static_cast<IdleHandle*>(handle->data);
                    if (i && !i->closed.load() && i->callback) {
                        CallbackPayload* p = new CallbackPayload(i->callback, {});
                        enqueue_callback_global(static_cast<void*>(p));
                    }
                };
                
                int r = uv_idle_start(idle->handle, idle_cb);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_idle_start failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["start"] = PropertyDescriptor{start_fn, false, false, false, tok};
            
            // idle.stop() -> undefined
            auto stop_fn = make_native_fn("idle.stop", [idle](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_idle_stop(idle->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_idle_stop failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["stop"] = PropertyDescriptor{stop_fn, false, false, false, tok};
            
            // idle.close() -> undefined
            auto close_fn = make_native_fn("idle.close", [idle](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!idle->closed.exchange(true)) {
                    uv_idle_stop(idle->handle);
                    uv_close((uv_handle_t*)idle->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_idle_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Idle"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // PREPARE HANDLE (Runs before polling for I/O)
    // ============================================================================

    // uv.Prepare() -> prepare handle object
    {
        auto fn = make_native_fn("uv.Prepare", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            struct PrepareHandle {
                uv_prepare_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
            };
            
            auto prepare = std::make_shared<PrepareHandle>();
            prepare->handle = new uv_prepare_t;
            prepare->handle->data = prepare.get();
            
            int r = uv_prepare_init(loop, prepare->handle);
            if (r != 0) {
                delete prepare->handle;
                throw SwaziError("SystemError", std::string("uv_prepare_init failed: ") + uv_strerror(r), token.loc);
            }
            
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[prepare->handle] = prepare;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // prepare.start(callback) -> undefined
            auto start_fn = make_native_fn("prepare.start", [prepare](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                    throw SwaziError("TypeError", "prepare.start requires callback", token.loc);
                }
                
                prepare->callback = std::get<FunctionPtr>(args[0]);
                
                auto prepare_cb = [](uv_prepare_t* handle) {
                    PrepareHandle* p = static_cast<PrepareHandle*>(handle->data);
                    if (p && !p->closed.load() && p->callback) {
                        CallbackPayload* payload = new CallbackPayload(p->callback, {});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                };
                
                int r = uv_prepare_start(prepare->handle, prepare_cb);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_prepare_start failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["start"] = PropertyDescriptor{start_fn, false, false, false, tok};
            
            // prepare.stop() -> undefined
            auto stop_fn = make_native_fn("prepare.stop", [prepare](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_prepare_stop(prepare->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_prepare_stop failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["stop"] = PropertyDescriptor{stop_fn, false, false, false, tok};
            
            // prepare.close() -> undefined
            auto close_fn = make_native_fn("prepare.close", [prepare](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!prepare->closed.exchange(true)) {
                    uv_prepare_stop(prepare->handle);
                    uv_close((uv_handle_t*)prepare->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_prepare_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Prepare"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // CHECK HANDLE (Runs after polling for I/O)
    // ============================================================================

    // uv.Check() -> check handle object
    {
        auto fn = make_native_fn("uv.Check", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            struct CheckHandle {
                uv_check_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
            };
            
            auto check = std::make_shared<CheckHandle>();
            check->handle = new uv_check_t;
            check->handle->data = check.get();
            
            int r = uv_check_init(loop, check->handle);
            if (r != 0) {
                delete check->handle;
                throw SwaziError("SystemError", std::string("uv_check_init failed: ") + uv_strerror(r), token.loc);
            }
            
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[check->handle] = check;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // check.start(callback) -> undefined
            auto start_fn = make_native_fn("check.start", [check](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                    throw SwaziError("TypeError", "check.start requires callback", token.loc);
                }
                
                check->callback = std::get<FunctionPtr>(args[0]);
                
                auto check_cb = [](uv_check_t* handle) {
                    CheckHandle* c = static_cast<CheckHandle*>(handle->data);
                    if (c && !c->closed.load() && c->callback) {
                        CallbackPayload* p = new CallbackPayload(c->callback, {});
                        enqueue_callback_global(static_cast<void*>(p));
                    }
                };
                
                int r = uv_check_start(check->handle, check_cb);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_check_start failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["start"] = PropertyDescriptor{start_fn, false, false, false, tok};
            
            // check.stop() -> undefined
            auto stop_fn = make_native_fn("check.stop", [check](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_check_stop(check->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_check_stop failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["stop"] = PropertyDescriptor{stop_fn, false, false, false, tok};
            
            // check.close() -> undefined
            auto close_fn = make_native_fn("check.close", [check](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!check->closed.exchange(true)) {
                    uv_check_stop(check->handle);
                    uv_close((uv_handle_t*)check->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_check_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Check"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // ASYNC HANDLE (Wake event loop from other threads)
    // ============================================================================

    // uv.Async(callback) -> async handle object
    {
        auto fn = make_native_fn("uv.Async", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) {
                throw SwaziError("TypeError", "uv.Async requires callback", token.loc);
            }
            
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            struct AsyncHandle {
                uv_async_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
            };
            
            auto async = std::make_shared<AsyncHandle>();
            async->callback = std::get<FunctionPtr>(args[0]);
            async->handle = new uv_async_t;
            async->handle->data = async.get();
            
            auto async_cb = [](uv_async_t* handle) {
                AsyncHandle* a = static_cast<AsyncHandle*>(handle->data);
                if (a && !a->closed.load() && a->callback) {
                    CallbackPayload* p = new CallbackPayload(a->callback, {});
                    enqueue_callback_global(static_cast<void*>(p));
                }
            };
            
            int r = uv_async_init(loop, async->handle, async_cb);
            if (r != 0) {
                delete async->handle;
                throw SwaziError("SystemError", std::string("uv_async_init failed: ") + uv_strerror(r), token.loc);
            }
            
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[async->handle] = async;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // async.send() -> undefined (thread-safe wake)
            auto send_fn = make_native_fn("async.send", [async](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_async_send(async->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_async_send failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["send"] = PropertyDescriptor{send_fn, false, false, false, tok};
            
            // async.close() -> undefined
            auto close_fn = make_native_fn("async.close", [async](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!async->closed.exchange(true)) {
                    uv_close((uv_handle_t*)async->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_async_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Async"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // POLL HANDLE (Poll file descriptors for I/O events)
    // ============================================================================

    // uv.Poll(fd) -> poll handle object
    {
        auto fn = make_native_fn("uv.Poll", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty() || !std::holds_alternative<double>(args[0])) {
                throw SwaziError("TypeError", "uv.Poll requires file descriptor", token.loc);
            }
            
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            int fd = static_cast<int>(std::get<double>(args[0]));
            
            struct PollHandle {
                uv_poll_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
            };
            
            auto poll = std::make_shared<PollHandle>();
            poll->handle = new uv_poll_t;
            poll->handle->data = poll.get();
            
            int r = uv_poll_init(loop, poll->handle, fd);
            if (r != 0) {
                delete poll->handle;
                throw SwaziError("SystemError", std::string("uv_poll_init failed: ") + uv_strerror(r), token.loc);
            }
            
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[poll->handle] = poll;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // poll.start(events, callback) -> undefined
            // events: UV_READABLE (1), UV_WRITABLE (2), UV_DISCONNECT (4), UV_PRIORITIZED (8)
            auto start_fn = make_native_fn("poll.start", [poll](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.size() < 2 || !std::holds_alternative<FunctionPtr>(args[1])) {
                    throw SwaziError("TypeError", "poll.start requires (events, callback)", token.loc);
                }
                
                int events = static_cast<int>(std::get<double>(args[0]));
                poll->callback = std::get<FunctionPtr>(args[1]);
                
                auto poll_cb = [](uv_poll_t* handle, int status, int events) {
                    PollHandle* p = static_cast<PollHandle*>(handle->data);
                    if (p && !p->closed.load() && p->callback) {
                        std::vector<Value> cb_args = {
                            Value{static_cast<double>(status)},
                            Value{static_cast<double>(events)}
                        };
                        
                        CallbackPayload* payload = new CallbackPayload(p->callback, cb_args);
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                };
                
                int r = uv_poll_start(poll->handle, events, poll_cb);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_poll_start failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["start"] = PropertyDescriptor{start_fn, false, false, false, tok};
            
            // poll.stop() -> undefined
            auto stop_fn = make_native_fn("poll.stop", [poll](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_poll_stop(poll->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_poll_stop failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["stop"] = PropertyDescriptor{stop_fn, false, false, false, tok};
            
            // poll.close() -> undefined
            auto close_fn = make_native_fn("poll.close", [poll](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!poll->closed.exchange(true)) {
                    uv_poll_stop(poll->handle);
                    uv_close((uv_handle_t*)poll->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_poll_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Poll"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // SIGNAL HANDLE (Catch Unix signals)
    // ============================================================================

    // uv.Signal() -> signal handle object
    {
        auto fn = make_native_fn("uv.Signal", [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) throw SwaziError("RuntimeError", "No event loop available", token.loc);
            
            struct SignalHandle {
                uv_signal_t* handle;
                FunctionPtr callback;
                std::atomic<bool> closed{false};
            };
            
            auto signal = std::make_shared<SignalHandle>();
            signal->handle = new uv_signal_t;
            signal->handle->data = signal.get();
            
            int r = uv_signal_init(loop, signal->handle);
            if (r != 0) {
                delete signal->handle;
                throw SwaziError("SystemError", std::string("uv_signal_init failed: ") + uv_strerror(r), token.loc);
            }
            
            {
                std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                g_uv_handles[signal->handle] = signal;
            }
            
            auto obj = std::make_shared<ObjectValue>();
            Token tok;
            
            // signal.start(signum, callback) -> undefined
            auto start_fn = make_native_fn("signal.start", [signal](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.size() < 2 || !std::holds_alternative<FunctionPtr>(args[1])) {
                    throw SwaziError("TypeError", "signal.start requires (signum, callback)", token.loc);
                }
                
                int signum = static_cast<int>(std::get<double>(args[0]));
                signal->callback = std::get<FunctionPtr>(args[1]);
                
                auto signal_cb = [](uv_signal_t* handle, int signum) {
                    SignalHandle* s = static_cast<SignalHandle*>(handle->data);
                    if (s && !s->closed.load() && s->callback) {
                        std::vector<Value> cb_args = {Value{static_cast<double>(signum)}};
                        CallbackPayload* p = new CallbackPayload(s->callback, cb_args);
                        enqueue_callback_global(static_cast<void*>(p));
                    }
                };
                
                int r = uv_signal_start(signal->handle, signal_cb, signum);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_signal_start failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["start"] = PropertyDescriptor{start_fn, false, false, false, tok};
            
            // signal.startOneshot(signum, callback) -> undefined
            auto start_oneshot_fn = make_native_fn("signal.startOneshot", [signal](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.size() < 2 || !std::holds_alternative<FunctionPtr>(args[1])) {
                    throw SwaziError("TypeError", "signal.startOneshot requires (signum, callback)", token.loc);
                }
                
                int signum = static_cast<int>(std::get<double>(args[0]));
                signal->callback = std::get<FunctionPtr>(args[1]);
                
                auto signal_cb = [](uv_signal_t* handle, int signum) {
                    SignalHandle* s = static_cast<SignalHandle*>(handle->data);
                    if (s && !s->closed.load() && s->callback) {
                        std::vector<Value> cb_args = {Value{static_cast<double>(signum)}};
                        CallbackPayload* p = new CallbackPayload(s->callback, cb_args);
                        enqueue_callback_global(static_cast<void*>(p));
                    }
                };
                
                int r = uv_signal_start_oneshot(signal->handle, signal_cb, signum);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_signal_start_oneshot failed: ") + uv_strerror(r), token.loc);
                }
                
                return std::monostate{};
            }, nullptr);
            obj->properties["startOneshot"] = PropertyDescriptor{start_oneshot_fn, false, false, false, tok};
            
            // signal.stop() -> undefined
            auto stop_fn = make_native_fn("signal.stop", [signal](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
                int r = uv_signal_stop(signal->handle);
                if (r != 0) {
                    throw SwaziError("SystemError", std::string("uv_signal_stop failed: ") + uv_strerror(r), token.loc);
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["stop"] = PropertyDescriptor{stop_fn, false, false, false, tok};
            
            // signal.close() -> undefined
            auto close_fn = make_native_fn("signal.close", [signal](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                if (!signal->closed.exchange(true)) {
                    uv_signal_stop(signal->handle);
                    uv_close((uv_handle_t*)signal->handle, [](uv_handle_t* h) {
                        std::lock_guard<std::mutex> lock(g_uv_handles_mutex);
                        g_uv_handles.erase(h);
                        delete (uv_signal_t*)h;
                    });
                }
                return std::monostate{};
            }, nullptr);
            obj->properties["close"] = PropertyDescriptor{close_fn, false, false, false, tok};
            
            return Value{obj}; }, env);
        obj->properties["Signal"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============================================================================
    // CONSTANTS
    // ============================================================================

    {
        auto constants = std::make_shared<ObjectValue>();
        Token tok;

        // Run modes
        constants->properties["RUN_DEFAULT"] = PropertyDescriptor{Value{0.0}, false, false, true, tok};
        constants->properties["RUN_ONCE"] = PropertyDescriptor{Value{1.0}, false, false, true, tok};
        constants->properties["RUN_NOWAIT"] = PropertyDescriptor{Value{2.0}, false, false, true, tok};

        // Poll events
        constants->properties["READABLE"] = PropertyDescriptor{Value{1.0}, false, false, true, tok};
        constants->properties["WRITABLE"] = PropertyDescriptor{Value{2.0}, false, false, true, tok};
        constants->properties["DISCONNECT"] = PropertyDescriptor{Value{4.0}, false, false, true, tok};
        constants->properties["PRIORITIZED"] = PropertyDescriptor{Value{8.0}, false, false, true, tok};

        // Common Unix signals
        constants->properties["SIGHUP"] = PropertyDescriptor{Value{1.0}, false, false, true, tok};
        constants->properties["SIGINT"] = PropertyDescriptor{Value{2.0}, false, false, true, tok};
        constants->properties["SIGQUIT"] = PropertyDescriptor{Value{3.0}, false, false, true, tok};
        constants->properties["SIGILL"] = PropertyDescriptor{Value{4.0}, false, false, true, tok};
        constants->properties["SIGTRAP"] = PropertyDescriptor{Value{5.0}, false, false, true, tok};
        constants->properties["SIGABRT"] = PropertyDescriptor{Value{6.0}, false, false, true, tok};
        constants->properties["SIGBUS"] = PropertyDescriptor{Value{7.0}, false, false, true, tok};
        constants->properties["SIGFPE"] = PropertyDescriptor{Value{8.0}, false, false, true, tok};
        constants->properties["SIGKILL"] = PropertyDescriptor{Value{9.0}, false, false, true, tok};
        constants->properties["SIGUSR1"] = PropertyDescriptor{Value{10.0}, false, false, true, tok};
        constants->properties["SIGSEGV"] = PropertyDescriptor{Value{11.0}, false, false, true, tok};
        constants->properties["SIGUSR2"] = PropertyDescriptor{Value{12.0}, false, false, true, tok};
        constants->properties["SIGPIPE"] = PropertyDescriptor{Value{13.0}, false, false, true, tok};
        constants->properties["SIGALRM"] = PropertyDescriptor{Value{14.0}, false, false, true, tok};
        constants->properties["SIGTERM"] = PropertyDescriptor{Value{15.0}, false, false, true, tok};
        constants->properties["SIGCHLD"] = PropertyDescriptor{Value{17.0}, false, false, true, tok};
        constants->properties["SIGCONT"] = PropertyDescriptor{Value{18.0}, false, false, true, tok};
        constants->properties["SIGSTOP"] = PropertyDescriptor{Value{19.0}, false, false, true, tok};
        constants->properties["SIGTSTP"] = PropertyDescriptor{Value{20.0}, false, false, true, tok};
        constants->properties["SIGTTIN"] = PropertyDescriptor{Value{21.0}, false, false, true, tok};
        constants->properties["SIGTTOU"] = PropertyDescriptor{Value{22.0}, false, false, true, tok};
        constants->properties["SIGURG"] = PropertyDescriptor{Value{23.0}, false, false, true, tok};
        constants->properties["SIGXCPU"] = PropertyDescriptor{Value{24.0}, false, false, true, tok};
        constants->properties["SIGXFSZ"] = PropertyDescriptor{Value{25.0}, false, false, true, tok};
        constants->properties["SIGVTALRM"] = PropertyDescriptor{Value{26.0}, false, false, true, tok};
        constants->properties["SIGPROF"] = PropertyDescriptor{Value{27.0}, false, false, true, tok};
        constants->properties["SIGWINCH"] = PropertyDescriptor{Value{28.0}, false, false, true, tok};
        constants->properties["SIGIO"] = PropertyDescriptor{Value{29.0}, false, false, true, tok};
        constants->properties["SIGPWR"] = PropertyDescriptor{Value{30.0}, false, false, true, tok};
        constants->properties["SIGSYS"] = PropertyDescriptor{Value{31.0}, false, false, true, tok};

        obj->properties["constants"] = PropertyDescriptor{Value{constants}, false, false, true, Token()};
    }

    return obj;
}
