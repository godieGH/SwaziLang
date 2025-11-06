#pragma once

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

// Token types (keep in sync with your parser/lexer)
enum class TokenType {
    // -----------------------
    // Declaration / statements
    // (top-level language keywords)
    // -----------------------
    DATA,
    CHAPISHA,
    ANDIKA,
    CONSTANT,
    KAZI,
    TABIA,
    RUDISHA,
    SIMAMA,
    ENDELEA,
    TUMIA,   // 'tumia' (import)
    KUTOKA,  // 'kutoka' (from)
    RUHUSU,  // 'ruhusu' (export)
    
    //-----------------------
    // asyncronous
    //-----------------------
    ASYNC,
    AWAIT,

    // -----------------------
    // Control-flow (if / else / switches / guards)
    // -----------------------
    KAMA,        // 'kama' (if)
    VINGINEVYO,  // 'vinginevyo' (else)
    KAIDA,       // (could be 'case' / pattern guard style)
    IKIWA,       // conditional-like (keeps related keywords together)
    CHAGUA,      // (switch / choose)
    NI,          // (helper keyword often used in control constructs)

    // -----------------------
    // Loops
    // -----------------------
    FOR,
    KILA,
    KATIKA,
    WHILE,
    DOWHILE,

    // -----------------------
    // Error handling / flow modifiers
    // -----------------------
    JARIBU,  // try
    MAKOSA,  // catch / errors
    KISHA,   // finally / after

    // -----------------------
    // Functions / lambdas / functional helpers
    // -----------------------
    LAMBDA,
    BlOCkSHORTHAND,  // =>> token

    // -----------------------
    // Class / OOP related
    // -----------------------
    MUUNDO,
    RITHI,
    UNDA,
    FUTA,
    TILDE,
    SUPA,

    // -----------------------
    // Literals & identifiers
    // -----------------------
    IDENTIFIER,
    NUMBER,
    STRING,                // double-quoted string
    SINGLE_QUOTED_STRING,  // single-quoted string (')
    TEMPLATE_STRING,       // backtick whole string (simple mode, no interpolation)
    TEMPLATE_CHUNK,        // raw chunk inside a template literal (full interpolation mode)
    TEMPLATE_EXPR_START,   // "${"
    TEMPLATE_EXPR_END,     // "}" that closes interpolation
    TEMPLATE_END,          // closing backtick (optional)
    BOOLEAN,
    NULL_LITERAL,
    NAN_LITERAL,
    INF_LITERAL,

    // -----------------------
    // Punctuation & operators (single-character / structural)
    // -----------------------
    SEMICOLON,
    COMMA,
    OPENPARENTHESIS,
    CLOSEPARENTHESIS,
    OPENBRACE,
    CLOSEBRACE,
    OPENBRACKET,
    CLOSEBRACKET,
    COLON,
    QUESTIONMARK,
    DOT,
    AT_SIGN,
    AMPERSAND,
    QUESTION_DOT,
    ELLIPSIS,

    // special single-char tokens
    SELF,  // $ sign

    // -----------------------
    // Assignment / file end
    // -----------------------
    ASSIGN,
    EOF_TOKEN,

    // -----------------------
    // Arithmetic
    // -----------------------
    PLUS,
    MINUS,
    STAR,
    SLASH,
    PERCENT,
    POWER,

    // -----------------------
    // Compound arithmetic / increments
    // -----------------------
    PLUS_ASSIGN,
    MINUS_ASSIGN,
    TIMES_ASSIGN,
    INCREMENT,
    DECREMENT,

    // -----------------------
    // Logical
    // -----------------------
    AND,
    OR,
    NOT,
    AINA,
    
    // -----------------------
    // errors & throws
    // -----------------------
    THROW,

    // -----------------------
    // Comparison
    // -----------------------
    GREATERTHAN,
    GREATEROREQUALTHAN,
    LESSTHAN,
    LESSOREQUALTHAN,
    EQUALITY,
    NOTEQUAL,
    STRICT_EQUALITY,
    STRICT_NOTEQUAL,

    // -----------------------
    // Indentation-based blocks / newlines (for indent-sensitive grammars)
    // -----------------------
    NEWLINE,
    INDENT,
    DEDENT,

    // -----------------------
    // Miscellaneous
    // -----------------------
    BLOCK_DU,
    LINE_DU,
    COMMENT,
    UNKNOWN
};

// Small struct for token location / span in source
struct TokenLocation {
   public:
    std::string filename;                 // source filename (or "<repl>")
    int line = 1;                         // 1-based
    int col = 1;                          // 1-based column of token start
    int length = 0;                       // token length in characters
    std::map<int, std::string> linestrv;  // used for tracing
    std::string line_trace;               // trace line

    TokenLocation() = default;
    TokenLocation(const std::string& fn, int ln, int c, int len = 0)
        : filename(fn), line(ln), col(c), length(len) {}

    void set_map_linestr(std::map<int, std::string> line_str) {
        linestrv = line_str;
        std::string ss = line_str[line];
        line_trace = ss;
    }
    int end_col() const { return col + std::max(0, length - 1); }

    std::string to_string() const {
        return filename + ":" + std::to_string(line) + ":" + std::to_string(col);
    }
    std::string get_line_trace() const {
        std::stringstream ss;
        ss << " * " << line << " | ";
        std::string fss = ss.str();
        std::string lss = std::string(col + fss.size(), ' ') + "^";
        ss << line_trace << "\n"
           << lss;
        return ss.str();
    }
};

// Represents a single token with location
struct Token {
    TokenType type = TokenType::UNKNOWN;
    std::string value;  // raw text / normalized lexeme
    TokenLocation loc;  // file:line:col and length/span

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