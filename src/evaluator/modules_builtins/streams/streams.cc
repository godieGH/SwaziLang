#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "./streams.h"
#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// ============================================================================
// ACTIVE OPERATIONS TRACKING
// ============================================================================

std::atomic<size_t> g_active_stream_operations{0};

bool streams_have_active_work() {
    return g_active_stream_operations.load() > 0;
}

std::atomic<long long> g_next_stream_id{1};

// ============================================================================
// UTILITY HELPERS
// ============================================================================

std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) {
        return std::get<std::string>(v);
    }
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) {
        return std::get<bool>(v) ? "true" : "false";
    }
    return "";
}

void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args) {
    if (!cb) return;
    CallbackPayload* p = new CallbackPayload(cb, args);
    enqueue_callback_global(static_cast<void*>(p));
}

// ============================================================================
// ENCODING HELPERS
// ============================================================================

Value encode_buffer_for_emission(const BufferPtr& buf, const std::string& encoding) {
    if (!buf) return std::monostate{};

    if (encoding == "utf8" || encoding == "utf-8") {
        return Value{std::string(buf->data.begin(), buf->data.end())};
    } else {
        // binary (default) - return raw buffer
        return Value{buf};
    }
}

// ============================================================================
// EXPORTS
// ============================================================================

std::shared_ptr<ObjectValue> make_streams_exports(EnvPtr env, Evaluator* evaluator) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<streams>", 0, 0, 0);

    auto createReadable = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_createReadStream(args, env, evaluator, token);
    };
    obj->properties["createReadable"] = {
        Value{std::make_shared<FunctionValue>("streams.createReadable", createReadable, env, tok)},
        false, false, true, tok};

    auto createWritable = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_createWriteStream(args, env, evaluator, token);
    };
    obj->properties["createWritable"] = {
        Value{std::make_shared<FunctionValue>("streams.createWritable", createWritable, env, tok)},
        false, false, true, tok};

    auto createDuplex = [evaluator](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        return native_createDuplexStream(args, env, evaluator, token);
    };
    obj->properties["createDuplex"] = {
        Value{std::make_shared<FunctionValue>("streams.createDuplex", createDuplex, env, tok)},
        false, false, true, tok};

    return obj;
}

// ============================================================================
// NETWORK STREAM STUBS
// ============================================================================

ObjectPtr create_network_readable_stream_object(uv_tcp_t* socket) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<net-readable>", 0, 0, 0);

    uv_stream_t* sock = (uv_stream_t*)socket;

    // Minimal on() - just collect listeners, don't use them yet
    auto on_impl = [sock](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2 || !std::holds_alternative<std::string>(args[0])) {
            return std::monostate{};
        }
        // Accept but ignore for now - HttpAPI doesn't push data here yet
        return std::monostate{};
    };
    obj->properties["on"] = {Value{std::make_shared<FunctionValue>("net.on", on_impl, nullptr, tok)}, false, false, true, tok};

    auto pause_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return std::monostate{};
    };
    obj->properties["pause"] = {Value{std::make_shared<FunctionValue>("net.pause", pause_impl, nullptr, tok)}, false, false, true, tok};

    auto resume_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return std::monostate{};
    };
    obj->properties["resume"] = {Value{std::make_shared<FunctionValue>("net.resume", resume_impl, nullptr, tok)}, false, false, true, tok};

    return obj;
}

ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<net-writable>", 0, 0, 0);

    uv_stream_t* sock = (uv_stream_t*)socket;

    // CRITICAL: write() must exist for wrap_stream_with_http to work
    auto write_impl = [sock](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (!sock || args.empty()) return Value{true};

        BufferPtr bufptr;
        if (std::holds_alternative<BufferPtr>(args[0])) {
            bufptr = std::get<BufferPtr>(args[0]);
        } else if (std::holds_alternative<std::string>(args[0])) {
            bufptr = std::make_shared<BufferValue>();
            std::string s = std::get<std::string>(args[0]);
            bufptr->data.assign(s.begin(), s.end());
        } else {
            return Value{true};
        }

        if (bufptr->data.empty()) return Value{true};

        char* mem = static_cast<char*>(malloc(bufptr->data.size()));
        if (!mem) throw SwaziError("Error", "Out of memory", token.loc);
        memcpy(mem, bufptr->data.data(), bufptr->data.size());

        uv_buf_t uvbuf = uv_buf_init(mem, (unsigned int)bufptr->data.size());
        uv_write_t* req = new uv_write_t;
        req->data = mem;

        int r = uv_write(req, sock, &uvbuf, 1, [](uv_write_t* req, int) {
            if (req->data) free(req->data);
            delete req;
        });

        if (r < 0) {
            if (mem) free(mem);
            delete req;
            throw SwaziError("IOError", std::string("write failed: ") + uv_strerror(r), token.loc);
        }

        return Value{true};
    };
    obj->properties["write"] = {Value{std::make_shared<FunctionValue>("net.write", write_impl, nullptr, tok)}, false, false, false, tok};

    // CRITICAL: end() must exist for wrap_stream_with_http to work
    auto end_impl = [sock](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (!sock) return std::monostate{};

        // If data provided, write it first
        if (!args.empty() && !std::holds_alternative<std::monostate>(args[0])) {
            BufferPtr bufptr;
            if (std::holds_alternative<BufferPtr>(args[0])) {
                bufptr = std::get<BufferPtr>(args[0]);
            } else if (std::holds_alternative<std::string>(args[0])) {
                bufptr = std::make_shared<BufferValue>();
                std::string s = std::get<std::string>(args[0]);
                bufptr->data.assign(s.begin(), s.end());
            }

            if (bufptr && !bufptr->data.empty()) {
                char* mem = static_cast<char*>(malloc(bufptr->data.size()));
                if (!mem) throw SwaziError("Error", "Out of memory", token.loc);
                memcpy(mem, bufptr->data.data(), bufptr->data.size());

                uv_buf_t uvbuf = uv_buf_init(mem, (unsigned int)bufptr->data.size());
                uv_write_t* req = new uv_write_t;
                req->data = mem;

                // Write then close
                uv_write(req, sock, &uvbuf, 1, [](uv_write_t* req, int) {
                    if (req->data) free(req->data);
                    uv_stream_t* s = req->handle;
                    uv_close((uv_handle_t*)s, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
                    delete req;
                });
                return std::monostate{};
            }
        }

        // No data, just close
        uv_close((uv_handle_t*)sock, [](uv_handle_t* h) { delete (uv_tcp_t*)h; });
        return std::monostate{};
    };
    obj->properties["end"] = {Value{std::make_shared<FunctionValue>("net.end", end_impl, nullptr, tok)}, false, false, false, tok};

    return obj;
}