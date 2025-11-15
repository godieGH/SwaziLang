#pragma once

#include "evaluator.hpp"

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
std::shared_ptr<ObjectValue> make_child_process_exports(EnvPtr env, Evaluator* evaluator);

std::shared_ptr<ObjectValue> make_base64_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_buffer_exports(EnvPtr env);

// Forward declaration: native createServer implementation (defined in HttpAPI.cpp)
Value native_createServer(const std::vector<Value>& args, EnvPtr env, const Token& token);
