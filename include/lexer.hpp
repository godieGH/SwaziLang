#pragma once

#include "token.hpp"
#include <string>
#include <vector>

class Lexer {
public:
    Lexer(const std::string& source, const std::string& filename = "");
    std::vector<Token> tokenize();

private:
    const std::string src;
    const std::string filename;
    size_t i = 0;
    int line = 1;
    int col = 1;

    // indentation stack (base 0)
    std::vector<int> indent_stack;

    // parentheses level (suppress NEWLINE inside parentheses)
    int paren_level = 0;

    // helpers
    bool eof() const;
    char peek(size_t offset = 0) const;
    char peek_next() const;
    char advance();

    // Add token: optional explicit length (if -1, length is value.size()).
    void add_token(std::vector<Token>& out, TokenType type, const std::string& value, int tok_line, int tok_col, int tok_length = -1);

    void scan_token(std::vector<Token>& out);
    void scan_number(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);
    void scan_identifier_or_keyword(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);
    void scan_string(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);
    void skip_line_comment();
    void handle_newline(std::vector<Token>& out);
    void emit_remaining_dedents(std::vector<Token>& out);
};