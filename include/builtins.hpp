#pragma once
#include "evaluator.hpp"
#include "uv.h"

// Factories producing the ObjectPtr exports for each builtin module.
// The EnvPtr is supplied so native functions can capture a module environment if needed.
std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_http_exports(EnvPtr env);

std::shared_ptr<ObjectValue> make_json_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_path_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_os_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_process_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_timers_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_subprocess_exports(EnvPtr env, Evaluator* evaluator);

std::shared_ptr<ObjectValue> make_base64_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_buffer_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_file_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_streams_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_net_exports(EnvPtr env, Evaluator* evaluator);

// Forward declaration: native createServer implementation (defined in HttpAPI.cpp)
Value native_createServer(const std::vector<Value>& args, EnvPtr env, const Token& token);

// NEW: Network stream helpers (defined in streams.cc, used by HttpAPI.cpp)
ObjectPtr create_network_readable_stream_object(uv_tcp_t* socket);
ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket);

bool tcp_has_active_work();
bool net_has_active_work();
bool udp_has_active_work();
bool streams_have_active_work();