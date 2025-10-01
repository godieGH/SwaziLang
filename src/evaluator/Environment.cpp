//src/evaluator/Environment.cpp
#include "evaluator.hpp"
#include "ClassRuntime.hpp"
#include <iostream>
#include <cmath>
#include <stdexcept>
#include <sstream>

// ----------------- Environment methods -----------------

bool Environment::has(const std::string& name) const {
   auto it = values.find(name);
   if (it != values.end()) return true;
   if (parent) return parent->has(name);
   return false;
}

Environment::Variable& Environment::get(const std::string& name) {
   auto it = values.find(name);
   if (it != values.end()) return it->second;
   if (parent) return parent->get(name);
   throw std::runtime_error("Undefined variable '" + name + "'");
}

void Environment::set(const std::string& name, const Variable& var) {
   // If variable exists in this environment, replace it here.
   // Otherwise create in current environment (no automatic up-chain assignment).
   values[name] = var;
}
