#pragma once
#include "evaluator.hpp"

std::shared_ptr<ObjectValue> make_serialization_exports(EnvPtr env, Evaluator* evaluator);