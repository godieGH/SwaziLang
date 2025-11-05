#pragma once

#include <ctime>
#include <string>

#include "evaluator.hpp"

// Basic helpers used by the runtime (UTC-based)
double epoch_ms_now();
std::tm tm_from_ms(double ms);

// Format and parse helpers. zone accepts "UTC" or offsets like "+03:00".
// These are intentionally small, self-contained helpers (no tz db dependency).
std::string format_time_from_ms(double ms, const std::string& fmt, const std::string& zone = "UTC");
double parse_time_to_ms(const std::string& s, const std::string& fmt = "", const std::string& zone = "");

// Register the free-function `Muda(...)` into the given environment.
void init_time(EnvPtr env);