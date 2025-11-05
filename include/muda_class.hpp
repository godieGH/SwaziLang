#pragma once
#include <ctime>
#include <iomanip>
#include <sstream>

#include "evaluator.hpp"

// Register the Muda class runtime (so `unda Muda()` works) and supporting native helpers.
void init_muda_class(EnvPtr env);