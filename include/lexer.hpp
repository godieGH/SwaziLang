#pragma once

#include <map>
#include <string>
#include <vector>

#include "token.hpp"

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

    std::string linechunk;
    std::map<int, std::string> linestr;

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
    bool handle_non_decimal_number(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);
    void scan_number(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);
    void scan_identifier_or_keyword(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);

    // scan quoted (single/double) string (handles escapes)
    void scan_quoted_string(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index, char quote);

    // scan template literal (backticks) with support for full interpolation:
    // Emits TEMPLATE_CHUNK, TEMPLATE_EXPR_START, TEMPLATE_EXPR_END and TEMPLATE_END tokens.
    void scan_template(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index);

    void skip_line_comment();
    void handle_newline(std::vector<Token>& out);
    void emit_remaining_dedents(std::vector<Token>& out);
};