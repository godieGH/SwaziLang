#pragma once
#include <string>

#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "time.hpp"
#include "token.hpp"

// Only declare helpers that are unique to the Muda helper module.
// The core time functions epoch_ms_now(), tm_from_ms(), format_time_from_ms()
// are provided by the existing time.hpp/time.cpp in the repo and must not be
// redefined here to avoid duplicate symbols.

// Convert a Value to epoch milliseconds (accepts number or parsable string)
double value_to_ms_or_throw(const Value& v, Token token = {});

// Parse helpers local to this module (exposed so muda_class can call them).
double parse_date_string_with_format_local(const std::string& input, const std::string& userFmt, Token token = {});
double parse_iso_like_local(const std::string& s, Token token = {});