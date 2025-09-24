#pragma once

#include <string>
#include <algorithm>

// Token types (keep in sync with your parser/lexer)
enum class TokenType {
    // keywords
    DATA,
    CHAPISHA,
    ANDIKA,
    CONSTANT,
    KAZI,
    RUDISHA,

    // control-flow keywords (if / else)
    KAMA,         // 'kama' (if)
    VINGINEVYO,   // 'vinginevyo' (else)
    FOR,
    WHILE,
    DOWHILE,

    // literals & identifiers
    IDENTIFIER,
    NUMBER,
    STRING,                // double-quoted string
    SINGLE_QUOTED_STRING,  // single-quoted string (')
    TEMPLATE_STRING,       // backtick whole string (simple mode, no interpolation)
    TEMPLATE_CHUNK,        // raw chunk inside a template literal (full interpolation mode)
    TEMPLATE_EXPR_START,   // "${"
    TEMPLATE_EXPR_END,     // "}" that closes interpolation
    TEMPLATE_END,          // closing backtick (optional: some lexers emit it)
    BOOLEAN,

    // punctuation
    SEMICOLON,
    COMMA,
    OPENPARENTHESIS,
    CLOSEPARENTHESIS,
    OPENBRACE,
    CLOSEBRACE,
    COLON,
    QUESTIONMARK,

    // assignment / file end
    ASSIGN,
    EOF_TOKEN,

    // arithmetic
    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    POWER,

    PLUS_ASSIGN,
    MINUS_ASSIGN,
    INCREMENT,
    DECREMENT,

    // logical
    AND,
    OR,
    NOT,

    // comparison
    GREATERTHAN,
    GREATEROREQUALTHAN,
    LESSTHAN,
    LESSOREQUALTHAN,
    EQUALITY,
    NOTEQUAL,

    // indentation-based blocks
    NEWLINE,
    INDENT,
    DEDENT,

    COMMENT,
    UNKNOWN
};

// Small struct for token location / span in source
struct TokenLocation {
    std::string filename; // source filename (or "<repl>")
    int line = 1;         // 1-based
    int col  = 1;         // 1-based column of token start
    int length = 0;       // token length in characters

    TokenLocation() = default;
    TokenLocation(const std::string& fn, int ln, int c, int len = 0)
        : filename(fn), line(ln), col(c), length(len) {}

    int end_col() const { return col + std::max(0, length - 1); }

    std::string to_string() const {
        return filename + ":" + std::to_string(line) + ":" + std::to_string(col);
    }
};

// Represents a single token with location
struct Token {
    TokenType type = TokenType::UNKNOWN;
    std::string value;      // raw text / normalized lexeme
    TokenLocation loc;      // file:line:col and length/span

    Token() = default;
    Token(TokenType t, const std::string& v, const TokenLocation& l)
        : type(t), value(v), loc(l) {}

    const std::string& filename() const { return loc.filename; }
    int line() const { return loc.line; }
    int col() const { return loc.col; }
    int length() const { return loc.length; }

    std::string debug_string() const {
        return loc.to_string() + " [" + value + "]";
    }
};