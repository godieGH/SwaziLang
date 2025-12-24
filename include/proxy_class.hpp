
#pragma once
#include "evaluator.hpp"

void init_proxy_class(EnvPtr env, Evaluator* evaluator);

FunctionPtr get_handler_method(ObjectPtr handler, const std::string& method_name, const Token& tok);