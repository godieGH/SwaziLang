#pragma once
#include <string>

// Enum for all possible token types
enum class TokenType {
    // keywords
    DATA,
    CHAPISHA,   // print with newline
    ANDIKA,     // print without newline
    CONSTANT,   // thabiti
    KAZI,       // function definition
    RUDISHA,    // return

    // literals & identifiers
    IDENTIFIER,
    NUMBER,     // numeric literal
    STRING,     // string literal
    BOOLEAN,    // boolean literal (kweli / sikweli)

    // punctuation
    SEMICOLON,
    COMMA,
    OPENPARENTHESIS,   // (
    CLOSEPARENTHESIS,  // )
    OPENBRACE,         // {
    CLOSEBRACE,        // }
    COLON,             // : (before indented block)

    // assignment / file end
    ASSIGN,            // =
    EOF_TOKEN,

    // arithmetic
    PLUS,              // +
    MINUS,             // -
    STAR,              // *
    SLASH,             // /
    PERCENT,           // %
    POWER,             // **
    
    PLUS_ASSIGN,       // +=
    MINUS_ASSIGN,      // -=
    INCREMENT,         // ++
    DECREMENT,         // --

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
    
    // indentation-based blocks
    NEWLINE,             // newline separator
    INDENT,              // indent increase
    DEDENT,              // indent decrease

    // other
    COMMENT,             // optional: if you want to emit comments
    UNKNOWN
};

// Represents a single token
struct Token {
    TokenType type;
    std::string value;    // raw text or normalized value
    std::string filename;
    int line;
    int col;
};