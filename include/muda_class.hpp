#pragma once
#include "evaluator.hpp"
#include <iomanip>
#include <ctime>
#include <sstream>

// Register the Muda class runtime (so `unda Muda()` works) and supporting native helpers.
void init_muda_class(EnvPtr env);