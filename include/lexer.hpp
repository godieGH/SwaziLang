#pragma once
#include <string>
#include <vector>
#include "token.hpp"

class Lexer {
public:
    Lexer(std::string source);
    std::vector<Token> tokenize();

private:
    std::string source;
    size_t position = 0;
};
