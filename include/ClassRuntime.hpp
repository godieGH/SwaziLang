#pragma once
#include "ast.hpp"
#include "evaluator.hpp"

// A minimal runtime representation for classes
struct ClassValue {
    std::string name;
    ClassPtr super;  // parent class (if any)
    // clone of the AST body so we can materialize instance fields/methods on instantiation
    std::unique_ptr<ClassBodyNode> body;
    // static table: properties and static methods go into an ObjectValue so they can be looked up similarly to objects
    ObjectPtr static_table = std::make_shared<ObjectValue>();
    // token for diagnostics
    Token token;

    // DEFINING ENVIRONMENT:
    // The environment in which the class declaration was evaluated (module/class lexical scope).
    // Instance initializers and instance method closures must resolve free identifiers
    // against this environment, not the environment where an object is later instantiated.
    EnvPtr defining_env = nullptr;
};