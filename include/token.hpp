#pragma once
#include <string>

// Enum for all possible token types
enum class TokenType {
    // keywords
    DATA,
    CHAPISHA,   // print with newline
    ANDIKA,     // print without newline

    // literals & identifiers
    IDENTIFIER,
    NUMBER,     // numeric literal (formerly NAMBA)
    STRING,     // string literal (formerly NENO)
    BOOLEAN,    // boolean literal (kweli / sikweli)

    // punctuation
    SEMICOLON,
    COMMA,
    OPENPARENTHESIS,  // (
    CLOSEPARENTHESIS, // )

    // assignment / file end
    ASSIGN,     // =
    EOF_TOKEN,

    // arithmetic
    PLUS,       // +
    MINUS,      // -
    STAR,       // *
    SLASH,      // /
    PERCENT,    // % (remainder)
    POWER,      // ** (exponent)
    
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    INCREMENT,
    DECREMENT,

    // logical
    AND,        // && or 'na'
    OR,         // || or 'au'
    NOT,        // ! or 'si'

    // comparison
    GREATERTHAN,         // >
    GREATEROREQUALTHAN,  // >=
    LESSTHAN,            // <
    LESSOREQUALTHAN,     // <=
    EQUALITY,            // == or 'sawa'
    NOTEQUAL,            // != or 'sisawa'
    
    CONSTANT,
    
    // other (comments etc.)
    COMMENT,     // optional: if you want to emit or capture comments
    UNKNOWN
};

// Represents a single token
struct Token {
    TokenType type;
    std::string value; // raw text or normalized value (e.g., "123", "\"hello\"", "kweli")
    std::string filename;
    int line;
    int col;
};