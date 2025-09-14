#include "lexer.hpp"
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <sstream>

Lexer::Lexer(const std::string& source, const std::string& filename)
    : src(source), filename(filename), i(0), line(1), col(1)
{
    indent_stack.push_back(0);
}

bool Lexer::eof() const { return i >= src.size(); }
char Lexer::peek(size_t offset) const {
    size_t idx = i + offset;
    if (idx >= src.size()) return '\0';
    return src[idx];
}
char Lexer::peek_next() const { return peek(1); }

char Lexer::advance() {
    if (eof()) return '\0';
    char c = src[i++];
    if (c == '\n') { line++; col = 1; }
    else col++;
    return c;
}

void Lexer::add_token(std::vector<Token>& out, TokenType type, const std::string& value, int tok_line, int tok_col, int tok_length) {
    int len = tok_length >= 0 ? tok_length : static_cast<int>(value.size());
    TokenLocation loc(filename.empty() ? "<repl>" : filename, tok_line, tok_col, len);
    Token t{ type, value, loc };
    out.push_back(std::move(t));
}

void Lexer::skip_line_comment() {
    while (!eof() && peek() != '\n') advance();
}

// scan double-quoted string with basic escapes
void Lexer::scan_string(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    // start_index points to the opening quote position in src
    advance(); // skip opening '"'
    std::string val;
    while (!eof()) {
        char c = peek();
        if (c == '"') { advance(); break; }
        if (c == '\\') {
            advance(); // backslash
            char nxt = peek();
            if (nxt == 'n') { val.push_back('\n'); advance(); }
            else if (nxt == 't') { val.push_back('\t'); advance(); }
            else if (nxt == '"' ) { val.push_back('"'); advance(); }
            else if (nxt == '\\') { val.push_back('\\'); advance(); }
            else { val.push_back(nxt); advance(); }
            continue;
        }
        // allow multiline strings: advance updates line/col
        val.push_back(advance());
    }
    int tok_length = static_cast<int>(i - start_index); // raw source length including quotes and escapes
    add_token(out, TokenType::STRING, val, tok_line, tok_col, tok_length);
}

void Lexer::scan_number(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    std::string val;
    bool seen_dot = false;
    while (!eof()) {
        char c = peek();
        if (std::isdigit((unsigned char)c)) { val.push_back(advance()); }
        else if (c == '.' && !seen_dot && std::isdigit((unsigned char)peek_next())) { seen_dot = true; val.push_back(advance()); }
        else break;
    }
    int tok_length = static_cast<int>(i - start_index);
    add_token(out, TokenType::NUMBER, val, tok_line, tok_col, tok_length);
}

void Lexer::scan_identifier_or_keyword(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    std::string id;
    while (!eof()) {
        char c = peek();
        if (std::isalnum((unsigned char)c) || c == '_') id.push_back(advance());
        else break;
    }

    std::string id_lower = id;
    for (auto &ch : id_lower) ch = static_cast<char>(std::tolower((unsigned char)ch));

    static const std::unordered_map<std::string, TokenType> keywords = {
        {"data", TokenType::DATA},
        {"chapisha", TokenType::CHAPISHA},
        {"andika", TokenType::ANDIKA},
        {"thabiti", TokenType::CONSTANT},
        {"kazi", TokenType::KAZI},
        {"rudisha", TokenType::RUDISHA},
        {"kweli", TokenType::BOOLEAN},
        {"sikweli", TokenType::BOOLEAN},
        {"na", TokenType::AND},
        {"au", TokenType::OR},
        {"si", TokenType::NOT},
        {"sawa", TokenType::EQUALITY},
        {"sisawa", TokenType::NOTEQUAL},

        // control-flow keywords
        {"kama", TokenType::KAMA},             // if
        {"vinginevyo", TokenType::VINGINEVYO}, // else

        // loop-related keywords
        {"kwa", TokenType::FOR},               // for-like loop with parentheses: kwa(a=0; a<6; a++) { ... } or kwa(a=0; a<6; a++): <INDENT> ...
        {"wakati", TokenType::WHILE},          // while style: wakati a < 6: ...  or wakati a < 8 { ... }
        {"fanya", TokenType::DOWHILE}          // do-while: fanya: <INDENT> ... wakati cond
    };

    auto it = keywords.find(id_lower);
    int tok_length = static_cast<int>(i - start_index);
    if (it != keywords.end()) {
        if (it->second == TokenType::BOOLEAN) add_token(out, TokenType::BOOLEAN, id_lower, tok_line, tok_col, tok_length);
        else add_token(out, it->second, id_lower, tok_line, tok_col, tok_length);
    } else {
        add_token(out, TokenType::IDENTIFIER, id, tok_line, tok_col, tok_length);
    }
}

void Lexer::handle_newline(std::vector<Token>& out) {
    // consume newline (supports CRLF)
    if (peek() == '\r' && peek_next() == '\n') { advance(); advance(); }
    else advance();

    int newline_line = line - 1; // token should point to the line that ended
    int newline_col = 1;
    if (paren_level == 0) {
        add_token(out, TokenType::NEWLINE, "", newline_line, newline_col, 1);

        // compute indentation of next non-blank line
        size_t scan = i;
        int newIndent = 0;
        bool only_blank = true;
        while (scan < src.size()) {
            char c = src[scan];
            if (c == ' ') { newIndent++; scan++; continue; }
            if (c == '\t') { newIndent += 4; scan++; continue; }
            if (c == '\r') { scan++; continue; }
            if (c == '\n') { scan++; continue; } // blank line: skip it
            only_blank = false;
            break;
        }

        if (!only_blank) {
            int curIndent = indent_stack.back();
            if (newIndent > curIndent) {
                indent_stack.push_back(newIndent);
                add_token(out, TokenType::INDENT, "", line, 1, 0);
            } else {
                while (newIndent < indent_stack.back()) {
                    indent_stack.pop_back();
                    add_token(out, TokenType::DEDENT, "", line, 1, 0);
                }
                if (newIndent != indent_stack.back()) {
                    std::ostringstream ss;
                    ss << "Indentation error in file '" << filename << "' at line " << line;
                    throw std::runtime_error(ss.str());
                }
            }
        }

        // advance i to skip those leading spaces/tabs so rest of lexing continues at content
        while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\r')) advance();
    }
}

void Lexer::emit_remaining_dedents(std::vector<Token>& out) {
    while (indent_stack.size() > 1) {
        indent_stack.pop_back();
        add_token(out, TokenType::DEDENT, "", line, col, 0);
    }
}

void Lexer::scan_token(std::vector<Token>& out) {
    char c = peek();
    // whitespace except newline
    if (c == ' ' || c == '\t' || c == '\r') { advance(); return; }

    if (c == '\n') { handle_newline(out); return; }

    // comments: '#' or '//' style
    if (c == '#') { skip_line_comment(); return; }
    if (c == '/' && peek_next() == '/') { advance(); advance(); skip_line_comment(); return; }
    if (c == '/' && peek_next() == '*') {
        advance(); advance();
        while (!eof()) {
            if (peek() == '*' && peek_next() == '/') { advance(); advance(); break; }
            advance();
        }
        return;
    }

    // two-char operators
    if (c == '*' && peek_next() == '*') { add_token(out, TokenType::POWER, "**", line, col, 2); advance(); advance(); return; }
    if (c == '=' && peek_next() == '=') { add_token(out, TokenType::EQUALITY, "==", line, col, 2); advance(); advance(); return; }
    if (c == '!' && peek_next() == '=') { add_token(out, TokenType::NOTEQUAL, "!=", line, col, 2); advance(); advance(); return; }
    if (c == '>' && peek_next() == '=') { add_token(out, TokenType::GREATEROREQUALTHAN, ">=", line, col, 2); advance(); advance(); return; }
    if (c == '<' && peek_next() == '=') { add_token(out, TokenType::LESSOREQUALTHAN, "<=", line, col, 2); advance(); advance(); return; }
    if (c == '+' && peek_next() == '+') { add_token(out, TokenType::INCREMENT, "++", line, col, 2); advance(); advance(); return; }
    if (c == '-' && peek_next() == '-') { add_token(out, TokenType::DECREMENT, "--", line, col, 2); advance(); advance(); return; }
    if (c == '+' && peek_next() == '=') { add_token(out, TokenType::PLUS_ASSIGN, "+=", line, col, 2); advance(); advance(); return; }
    if (c == '-' && peek_next() == '=') { add_token(out, TokenType::MINUS_ASSIGN, "-=", line, col, 2); advance(); advance(); return; }
    if (c == '&' && peek_next() == '&') { add_token(out, TokenType::AND, "&&", line, col, 2); advance(); advance(); return; }
    if (c == '|' && peek_next() == '|') { add_token(out, TokenType::OR, "||", line, col, 2); advance(); advance(); return; }

    // single-char tokens & bookkeeping
    switch (c) {
        case ';': add_token(out, TokenType::SEMICOLON, ";", line, col, 1); advance(); return;
        case ',': add_token(out, TokenType::COMMA, ",", line, col, 1); advance(); return;
        case '(': paren_level++; add_token(out, TokenType::OPENPARENTHESIS, "(", line, col, 1); advance(); return;
        case ')': if (paren_level > 0) paren_level--; add_token(out, TokenType::CLOSEPARENTHESIS, ")", line, col, 1); advance(); return;
        case '{': add_token(out, TokenType::OPENBRACE, "{", line, col, 1); advance(); return;
        case '}': add_token(out, TokenType::CLOSEBRACE, "}", line, col, 1); advance(); return;
        case ':': add_token(out, TokenType::COLON, ":", line, col, 1); advance(); return;
        case '=': add_token(out, TokenType::ASSIGN, "=", line, col, 1); advance(); return;
        case '+': add_token(out, TokenType::PLUS, "+", line, col, 1); advance(); return;
        case '-': add_token(out, TokenType::MINUS, "-", line, col, 1); advance(); return;
        case '*': add_token(out, TokenType::STAR, "*", line, col, 1); advance(); return;
        case '/': add_token(out, TokenType::SLASH, "/", line, col, 1); advance(); return;
        case '%': add_token(out, TokenType::PERCENT, "%", line, col, 1); advance(); return;
        case '>': add_token(out, TokenType::GREATERTHAN, ">", line, col, 1); advance(); return;
        case '<': add_token(out, TokenType::LESSTHAN, "<", line, col, 1); advance(); return;
        case '!': add_token(out, TokenType::NOT, "!", line, col, 1); advance(); return;
        case '"': {
            size_t start_index = i;
            scan_string(out, line, col, start_index);
            return;
        }
        default: break;
    }

    // number
    if (std::isdigit((unsigned char)c)) { size_t start_index = i; scan_number(out, line, col, start_index); return; }

    // identifier or keyword
    if (std::isalpha((unsigned char)c) || c == '_') { size_t start_index = i; scan_identifier_or_keyword(out, line, col, start_index); return; }

    // unknown char
    {
        std::string s(1, c);
        add_token(out, TokenType::UNKNOWN, s, line, col, 1);
        advance();
        return;
    }
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;

    // skip UTF-8 BOM if present
    if (src.size() >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF) {
        i = 3;
        col = 4;
    }

    while (!eof()) scan_token(out);

    // ensure last line closed: emit a trailing NEWLINE if previous token not NEWLINE/DEDENT
    if (out.empty() || (out.back().type != TokenType::NEWLINE && out.back().type != TokenType::DEDENT)) {
        add_token(out, TokenType::NEWLINE, "", line, col, 1);
    }

    emit_remaining_dedents(out);

    // final EOF token
    add_token(out, TokenType::EOF_TOKEN, "", line, col, 0);
    return out;
}