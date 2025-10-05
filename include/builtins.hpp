#pragma once

#include "evaluator.hpp"

// Factories producing the ObjectPtr exports for each builtin module.
// The EnvPtr is supplied so native functions can capture a module environment if needed.
std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_http_exports(EnvPtr env);