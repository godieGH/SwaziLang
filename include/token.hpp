#pragma once
#include <string>

// Enum for all possible token types
enum class TokenType {
    DATA,
    CHAPISHA,
    IDENTIFIER,
    NAMBA,
    NENO,
    BOOLEAN,
    SAWA,
    SEMICOLON,
    EOF_TOKEN,
    UNKNOWN
};

// Represents a single token
struct Token {
    TokenType type;
    std::string value;
};
