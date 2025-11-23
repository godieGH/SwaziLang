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
    return obj;
}

ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket) {
    auto obj = std::make_shared<ObjectValue>();
    return obj;
}