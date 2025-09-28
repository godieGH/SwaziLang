#include "lexer.hpp"
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <sstream>
#include <algorithm>

// Constructor
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

// scan quoted string with basic escapes; supports single and double quotes
// start_index points to the opening quote position in src
void Lexer::scan_quoted_string(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index, char quote) {
    // skip opening quote
    advance();
    std::string val;
    while (!eof()) {
        char c = peek();
        if (c == quote) { advance(); break; }
        if (c == '\\') {
            advance(); // backslash
            char nxt = peek();
            if (nxt == 'n') { val.push_back('\n'); advance(); }
            else if (nxt == 't') { val.push_back('\t'); advance(); }
            else if (nxt == '\'' ) { val.push_back('\''); advance(); }
            else if (nxt == '"' ) { val.push_back('"'); advance(); }
            else if (nxt == '\\') { val.push_back('\\'); advance(); }
            else { val.push_back(nxt); advance(); }
            continue;
        }
        // allow multiline strings: advance updates line/col
        val.push_back(advance());
    }
    int tok_length = static_cast<int>(i - start_index); // raw source length including quotes and escapes

    TokenType tt = TokenType::STRING;
    if (quote == '\'') tt = TokenType::SINGLE_QUOTED_STRING;

    add_token(out, tt, val, tok_line, tok_col, tok_length);
}

// scan template literal with full interpolation support.
// Emits TEMPLATE_CHUNK tokens (may be empty), TEMPLATE_EXPR_START ("${"),
// then emits normal tokens for the expression, and when the top-level '}' of the
// interpolation is encountered emits TEMPLATE_EXPR_END and resumes chunking.
// Finally emits TEMPLATE_END when the closing backtick is found.
void Lexer::scan_template(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    // consume opening backtick
    advance();

    std::string chunk;
    // initial chunk start position: after the opening backtick
    int chunk_start_line = line;
    int chunk_start_col = col;

    auto emit_chunk = [&]() {
        // Always emit a chunk token (may be empty)
        add_token(out, TokenType::TEMPLATE_CHUNK, chunk, chunk_start_line, chunk_start_col, -1);
        chunk.clear();
    };

    while (!eof()) {
        char c = peek();

        // closing backtick: emit final chunk then TEMPLATE_END token and return
        if (c == '`') {
            advance();
            emit_chunk();
            add_token(out, TokenType::TEMPLATE_END, "`", line, col, 1);
            return;
        }

        // escape sequences inside template text
        if (c == '\\') {
            advance();
            char nxt = peek();
            if (nxt == 'n') { chunk.push_back('\n'); advance(); }
            else if (nxt == 't') { chunk.push_back('\t'); advance(); }
            else if (nxt == '`') { chunk.push_back('`'); advance(); }
            else if (nxt == '\\') { chunk.push_back('\\'); advance(); }
            else { chunk.push_back(nxt); advance(); }
            continue;
        }

        // interpolation start: "${"
        if (c == '$' && peek_next() == '{') {
            // emit current chunk (may be empty)
            emit_chunk();

            int expr_line = line;
            int expr_col  = col;
            // consume '${'
            advance(); // $
            advance(); // {
            add_token(out, TokenType::TEMPLATE_EXPR_START, "${", expr_line, expr_col, 2);

            // Now lex tokens for the embedded expression until the matching top-level '}'.
            int brace_depth = 0;
            while (!eof()) {
                // If we are at a '}' and brace_depth == 0 it's the interpolation end.
                if (peek() == '}' && brace_depth == 0) {
                    int end_line = line;
                    int end_col = col;
                    advance(); // consume '}'
                    add_token(out, TokenType::TEMPLATE_EXPR_END, "}", end_line, end_col, 1);

                    // next chunk starts at current line/col
                    chunk_start_line = line;
                    chunk_start_col = col;
                    break;
                }

                // If next is '{' inside expression increase nesting and let scan_token emit tokens
                if (peek() == '{') {
                    brace_depth++;
                    scan_token(out); // will emit OPENBRACE token etc
                    continue;
                }
                if (peek() == '}') {
                    // nested close brace inside expression: let scan_token emit token and decrement depth
                    scan_token(out);
                    brace_depth = std::max(0, brace_depth - 1);
                    continue;
                }

                // Otherwise lex normally the expression content
                scan_token(out);
            }
            // continue scanning template chunks
            continue;
        }

        // default: add character to chunk (advance updates line/col)
        chunk.push_back(advance());
    }

    // Unterminated template (reached EOF)
    std::ostringstream ss;
    ss << "Unterminated template literal in file '" << (filename.empty() ? "<repl>" : filename)
       << "' starting at line " << tok_line << ", col " << tok_col;
    throw std::runtime_error(ss.str());
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

    static const std::unordered_map<std::string, TokenType> keywords = {
        {"data", TokenType::DATA},
        {"chapisha", TokenType::CHAPISHA},
        {"andika", TokenType::ANDIKA},
        {"thabiti", TokenType::CONSTANT},
        {"kazi", TokenType::KAZI},
        {"tabia", TokenType::TABIA},
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
        {"kwa", TokenType::FOR},              // for-like loop with parentheses: kwa(a=0; a<6; a++) { ... } or kwa(a=0; a<6; a++): <INDENT> ...
        {"kila", TokenType::KILA},              // for-like loop with parentheses: kwa(a=0; a<6; a++) { ... } or kwa(a=0; a<6; a++): <INDENT> ...
        {"katika", TokenType::KATIKA},              // for-like loop with parentheses: kwa(a=0; a<6; a++) { ... } or kwa(a=0; a<6; a++): <INDENT> ...
        {"wakati", TokenType::WHILE},          // while style: wakati a < 6: ...  or wakati a < 8 { ... }
        {"fanya", TokenType::DOWHILE}          // do-while: fanya: <INDENT> ... wakati cond
    };

    auto it = keywords.find(id);
    int tok_length = static_cast<int>(i - start_index);
    if (it != keywords.end()) {
        if (it->second == TokenType::BOOLEAN) add_token(out, TokenType::BOOLEAN, id, tok_line, tok_col, tok_length);
        else add_token(out, it->second, id, tok_line, tok_col, tok_length);
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

    // Only do indentation tracking when not inside parens/brackets (same as before)
    if (paren_level == 0) {
        // Emit a NEWLINE token for the logical line break
        add_token(out, TokenType::NEWLINE, "", newline_line, newline_col, 1);

        // Scan forward to find the first non-blank, non-comment line.
        // When found, compute its indentation (spaces/tabs) as newIndent.
        size_t scan = i;
        bool only_blank = true;
        int newIndent = 0;
        size_t first_code_pos = scan; // will hold index of first non-space character on the code line

        while (scan < src.size()) {
            char c = src[scan];

            // Skip CR and bare newlines (completely blank lines)
            if (c == '\r') { scan++; continue; }
            if (c == '\n')  { scan++; continue; }

            // Skip whole line comments: '#' style
            if (c == '#') {
                // skip until end of this line
                while (scan < src.size() && src[scan] != '\n') scan++;
                continue;
            }

            // Skip '//' style comments
            if (c == '/' && (scan + 1) < src.size() && src[scan + 1] == '/') {
                scan += 2;
                while (scan < src.size() && src[scan] != '\n') scan++;
                continue;
            }

            // Found a non-blank, non-comment line. Compute its indent (count leading spaces/tabs)
            only_blank = false;
            size_t j = scan;
            newIndent = 0;
            while (j < src.size()) {
                if (src[j] == ' ') { newIndent += 1; j++; }
                else if (src[j] == '\t') { newIndent += 4; j++; } // tab == 4 spaces
                else break;
            }
            first_code_pos = j; // first non-space char index on that code line
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
                    ss << "Indentation error in file '" << (filename.empty() ? "<repl>" : filename)
                       << "' at line " << line;
                    throw std::runtime_error(ss.str());
                }
            }

            // Advance the lexer index to the first non-space of that next code line
            // (use advance() so line/col counters stay correct)
            while (!eof() && i < first_code_pos) advance();
        }

        // If only_blank == true we do nothing (blank/comment-only rest of file) and
        // leave i where it is. The next scan_token() call will handle further newlines.
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

   if(c == '.' && peek_next() == '.' && peek(2) == '.') {
      add_token(out, TokenType::ELLIPSIS, "...", line, col, 3);
      advance(); advance(); advance();
      return;
   }

    // two-char operators
    if (c == '*' && peek_next() == '*') { add_token(out, TokenType::POWER, "**", line, col, 2); advance(); advance(); return; }
    if (c == '=' && peek_next() == '=') { add_token(out, TokenType::EQUALITY, "==", line, col, 2); advance(); advance(); return; }
    if (c == '=' && peek_next() == '>') { add_token(out, TokenType::LAMBDA, "=>", line, col, 2); advance(); advance(); return; }
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
        case '[': add_token(out, TokenType::OPENBRACKET, "[", line, col, 1); advance(); return;
        case ']': add_token(out, TokenType::CLOSEBRACKET, "]", line, col, 1); advance(); return;
        case '$': add_token(out, TokenType::SELF, "$", line, col, 1); advance(); return;
        case '.': add_token(out, TokenType::DOT, ".", line, col, 1); advance(); return;
        case ':': add_token(out, TokenType::COLON, ":", line, col, 1); advance(); return;
        case '?': add_token(out, TokenType::QUESTIONMARK, "?", line, col, 1); advance(); return;
        case '@': add_token(out, TokenType::AT_SIGN, "@", line, col, 1); advance(); return;
        case '&': add_token(out, TokenType::AMPERSAND, "&", line, col, 1); advance(); return;
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
            scan_quoted_string(out, line, col, start_index, '"');
            return;
        }
        case '\'': {
            size_t start_index = i;
            scan_quoted_string(out, line, col, start_index, '\'');
            return;
        }
        case '`': {
            size_t start_index = i;
            scan_template(out, line, col, start_index);
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