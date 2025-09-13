#pragma once
#include <string>
#include <vector>
#include "token.hpp"

class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename);

    std::vector<Token> tokenize();

private:
    std::string source;
    std::string filename;
    size_t position = 0;
    int line = 1;           // track line numbers
    int col = 1;            // track column numbers
};