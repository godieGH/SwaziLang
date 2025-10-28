#pragma once
#include <string>
#include <optional>

// Query whether an embedded module exists (key variants: "std", "std.sl", "swazi:std", "pkg/foo", ...)
bool has_embedded_module(const std::string &spec);

// Get embedded source for a module spec (returns nullopt if not found)
std::optional<std::string> get_embedded_module_source(const std::string &spec);