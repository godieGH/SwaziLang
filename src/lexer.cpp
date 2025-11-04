#include "lexer.hpp"

#include <algorithm>
#include <iostream>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// Constructor
Lexer::Lexer(const std::string& source, const std::string& filename)
    : src(source), filename(filename), i(0), line(1), col(1) {
    indent_stack.push_back(0);
}

bool Lexer::eof() const {
    return i >= src.size();
}
char Lexer::peek(size_t offset) const {
    size_t idx = i + offset;
    if (idx >= src.size()) return '\0';
    return src[idx];
}
char Lexer::peek_next() const {
    return peek(1);
}

char Lexer::advance() {
    if (eof()) return '\0';
    char c = src[i++];
    if (c == '\n') {
        linestr[line] = linechunk;
        linechunk = "";
        line++;
        col = 1;
    } else {
        col++;
        linechunk += c;
    }
    return c;
}


void Lexer::add_token(std::vector<Token>& out, TokenType type, const std::string& value, int tok_line, int tok_col, int tok_length) {
    int len = tok_length >= 0 ? tok_length : static_cast<int>(value.size());
    TokenLocation loc(filename.empty() ? "<repl>" : filename, tok_line, tok_col, len);
    Token t{type, value, loc};
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
        if (c == quote) {
            advance();
            break;
        }
        if (c == '\\') {
            advance();  // backslash
            char nxt = peek();
            if (nxt == 'n') {
                val.push_back('\n');
                advance();
            } else if (nxt == 't') {
                val.push_back('\t');
                advance();
            } else if (nxt == '\'') {
                val.push_back('\'');
                advance();
            } else if (nxt == '"') {
                val.push_back('"');
                advance();
            } else if (nxt == '\\') {
                val.push_back('\\');
                advance();
            } else {
                val.push_back(nxt);
                advance();
            }
            continue;
        }
        // allow multiline strings: advance updates line/col
        val.push_back(advance());
    }
    int tok_length = static_cast<int>(i - start_index);  // raw source length including quotes and escapes

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
            if (nxt == 'n') {
                chunk.push_back('\n');
                advance();
            } else if (nxt == 't') {
                chunk.push_back('\t');
                advance();
            } else if (nxt == '`') {
                chunk.push_back('`');
                advance();
            } else if (nxt == '\\') {
                chunk.push_back('\\');
                advance();
            } else {
                chunk.push_back(nxt);
                advance();
            }
            continue;
        }

        // interpolation start: "${"
        if (c == '$' && peek_next() == '{') {
            // emit current chunk (may be empty)
            emit_chunk();

            int expr_line = line;
            int expr_col = col;
            // consume '${'
            advance();  // $
            advance();  // {
            add_token(out, TokenType::TEMPLATE_EXPR_START, "${", expr_line, expr_col, 2);

            // Now lex tokens for the embedded expression until the matching top-level '}'.
            int brace_depth = 0;
            while (!eof()) {
                // If we are at a '}' and brace_depth == 0 it's the interpolation end.
                if (peek() == '}' && brace_depth == 0) {
                    int end_line = line;
                    int end_col = col;
                    advance();  // consume '}'
                    add_token(out, TokenType::TEMPLATE_EXPR_END, "}", end_line, end_col, 1);

                    // next chunk starts at current line/col
                    chunk_start_line = line;
                    chunk_start_col = col;
                    break;
                }

                // If next is '{' inside expression increase nesting and let scan_token emit tokens
                if (peek() == '{') {
                    brace_depth++;
                    scan_token(out);  // will emit OPENBRACE token etc
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
       std::stringstream sd;
       sd  << " * " << tok_line << " | ";
       ss << "\n --> Traced at: \n" << sd.str() << linestr[tok_line] << "\n" << std::string(sd.str().size() + (tok_col - 6), ' ') << "~~~^~~~";
    throw std::runtime_error(ss.str());
}

void Lexer::scan_number(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    std::string val;
    bool seen_dot = false;

    while (!eof()) {
        char c = peek();

        // digits
        if (std::isdigit((unsigned char)c)) {
            val.push_back(advance());
        }
        // decimal point (only one allowed, and must be followed by a digit)
        else if (c == '.' && !seen_dot && std::isdigit((unsigned char)peek_next())) {
            seen_dot = true;
            val.push_back(advance());
        }
        // exponent part: 'e' or 'E'
        else if ((c == 'e' || c == 'E')) {
            // Save state in case the exponent is invalid (e.g. "1e" or "1e+")
            size_t save_i = i;
            size_t saved_len = val.size();

            // consume 'e' or 'E'
            val.push_back(advance());

            // optional sign after exponent
            if (!eof() && (peek() == '+' || peek() == '-')) {
                val.push_back(advance());
            }

            // require at least one digit after exponent
            bool has_exp_digits = false;
            while (!eof() && std::isdigit((unsigned char)peek())) {
                has_exp_digits = true;
                val.push_back(advance());
            }

            if (!has_exp_digits) {
                // rollback to before 'e' if no digits followed -- leave 'e' for caller
                i = save_i;
                val.resize(saved_len);
                break;  // stop scanning number here
            }
        } 
        else if(c == '_' && std::isdigit((unsigned char)peek_next()) && !seen_dot) {
          advance();
        } else {
            break;
        }
    }

    int tok_length = static_cast<int>(i - start_index);
    add_token(out, TokenType::NUMBER, val, tok_line, tok_col, tok_length);
}
void Lexer::scan_identifier_or_keyword(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    std::string id;
    while (!eof()) {
        char c = peek();
        if (std::isalnum((unsigned char)c) || c == '_')
            id.push_back(advance());
        else
            break;
    }

    static const std::unordered_map<std::string, TokenType> keywords = {
        {"data", TokenType::DATA},
        {"chapisha", TokenType::CHAPISHA},
        {"andika", TokenType::ANDIKA},
        {"thabiti", TokenType::CONSTANT},
        {"kazi", TokenType::KAZI},
        {"tabia", TokenType::TABIA},
        {"chagua", TokenType::CHAGUA},
        {"ikiwa", TokenType::IKIWA},
        {"kaida", TokenType::KAIDA},
        {"rudisha", TokenType::RUDISHA},
        {"kweli", TokenType::BOOLEAN},
        {"sikweli", TokenType::BOOLEAN},
        {"na", TokenType::AND},
        {"ni", TokenType::NI},
        {"au", TokenType::OR},
        {"si", TokenType::NOT},
        {"sawa", TokenType::EQUALITY},
        {"sisawa", TokenType::NOTEQUAL},

        {"ainaya", TokenType::AINA},

        // module / import / export
        {"tumia", TokenType::TUMIA},    // import
        {"kutoka", TokenType::KUTOKA},  // from
        {"ruhusu", TokenType::RUHUSU},  // export

        // calsses keywords
        {"muundo", TokenType::MUUNDO},
        {"rithi", TokenType::RITHI},
        {"unda", TokenType::UNDA},
        {"supa", TokenType::SUPA},
        {"super", TokenType::SUPA},
        {"futa", TokenType::FUTA},
        {"this", TokenType::SELF},
        {"self", TokenType::SELF},

        // control-flow keywords
        {"kama", TokenType::KAMA},              // if
        {"vinginevyo", TokenType::VINGINEVYO},  // else

        {"jaribu", TokenType::JARIBU},
        {"makosa", TokenType::MAKOSA},
        {"kisha", TokenType::KISHA},

        // loop-related keywords
        {"kwa", TokenType::FOR},  // for-like loop
        {"kila", TokenType::KILA},
        {"katika", TokenType::KATIKA},
        {"wakati", TokenType::WHILE},
        {"fanya", TokenType::DOWHILE},
        {"simama", TokenType::SIMAMA},
        {"endelea", TokenType::ENDELEA},

        {"null", TokenType::NULL_LITERAL},  // null token
        {"nan", TokenType::NAN_LITERAL},
        {"inf", TokenType::INF_LITERAL},
        
        {"__block__", TokenType::BLOCK_DU},
        {"__line__", TokenType::LINE_DU},
      
    };

    auto it = keywords.find(id);
    int tok_length = static_cast<int>(i - start_index);
    if (it != keywords.end()) {
        if (it->second == TokenType::BOOLEAN)
            add_token(out, TokenType::BOOLEAN, id, tok_line, tok_col, tok_length);
        else
            add_token(out, it->second, id, tok_line, tok_col, tok_length);
    } else {
        add_token(out, TokenType::IDENTIFIER, id, tok_line, tok_col, tok_length);
    }
}

void Lexer::handle_newline(std::vector<Token>& out) {
    // consume newline (supports CRLF)
    if (peek() == '\r' && peek_next() == '\n') {
        advance();
        advance();
    } else
        advance();

    int newline_line = line - 1;  // token should point to the line that ended
    int newline_col = 1;

    // --- CONTINUATION CHECK ---
    bool continuation = false;
    if (!out.empty()) {
        TokenType prev = out.back().type;
        switch (prev) {
            case TokenType::ASSIGN:        // =
            case TokenType::PLUS:          // +
            case TokenType::MINUS:         // -
            case TokenType::STAR:          // *
            case TokenType::SLASH:         // /
            case TokenType::COMMA:         // ,
            case TokenType::DOT:           // .
            case TokenType::LAMBDA:        // =>
            case TokenType::ELLIPSIS:      // ...
            case TokenType::PLUS_ASSIGN:   // +=
            case TokenType::MINUS_ASSIGN:  // -=
                continuation = true;
                break;
            default:
                break;
        }
    }

    // inside parentheses/brackets OR after a continuation operator: skip NEWLINE
    if (paren_level > 0 || continuation) {
        return;
    }

    // --- NORMAL INDENTATION LOGIC ---
    add_token(out, TokenType::NEWLINE, "", newline_line, newline_col, 1);

    // Scan forward to find the first non-blank, non-comment line.
    // We must treat lines that contain only whitespace or only comments (even if
    // the comment is preceded by indentation) as blank and keep scanning.
    size_t scan = i;
    bool only_blank = true;
    int newIndent = 0;
    size_t first_code_pos = scan;

    while (scan < src.size()) {
        // handle CRLF / LF boundaries: if the candidate line is empty, skip it
        if (src[scan] == '\r') {
            scan++;
            continue;
        }
        if (src[scan] == '\n') {
            scan++;
            continue;
        }

        // For this candidate line, skip its leading spaces/tabs and compute indent.
        size_t pos = scan;
        int indentCount = 0;
        while (pos < src.size()) {
            if (src[pos] == ' ') {
                indentCount++;
                pos++;
            } else if (src[pos] == '\t') {
                indentCount += 4;
                pos++;
            } else
                break;
        }

        // If the rest of the line is empty (newline or EOF) -> blank line: skip it
        if (pos >= src.size()) {
            scan = pos;
            continue;
        }
        if (src[pos] == '\r' || src[pos] == '\n') {
            // a line of only whitespace
            scan = pos;
            continue;
        }

        // If after indentation we have a line comment start '#', or '//' -> skip entire line
        if (src[pos] == '#') {
            // skip to end of line
            while (scan < src.size() && src[scan] != '\n') scan++;
            continue;
        }
        if (src[pos] == '/' && (pos + 1) < src.size() && src[pos + 1] == '/') {
            // skip to end of line
            while (scan < src.size() && src[scan] != '\n') scan++;
            continue;
        }

        // If after indentation we have a block comment '/*', skip until closing '*/'
        if (src[pos] == '/' && (pos + 1) < src.size() && src[pos + 1] == '*') {
            // find closing '*/' — if not found, advance to EOF and stop
            size_t b = pos + 2;
            bool closed = false;
            while (b + 1 < src.size()) {
                if (src[b] == '*' && src[b + 1] == '/') {
                    b += 2;
                    closed = true;
                    break;
                }
                b++;
            }
            // set scan to after the block comment; if closed, continue scanning from there,
            // otherwise set scan to EOF and break the loop.
            if (closed) {
                scan = b;
                continue;
            } else {
                scan = src.size();
                break;
            }
        }

        // Otherwise this line contains code: record its indentation (the number of
        // spaces/tabs we skipped) and the position of the first non-space char.
        only_blank = false;
        newIndent = indentCount;
        first_code_pos = pos;
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
                ss << "Indentation error in file '"
                   << (filename.empty() ? "<repl>" : filename)
                   << "' at line " << line;
                   
                   ss << "\n --> Traced at: \n";
                   std::stringstream sd;
                   sd << " * " << line << " | ";
                   
                   ss << sd.str() << linestr[line] << "\n" << std::string(sd.str().size() + col, ' ') << "^";
                   
                throw std::runtime_error(ss.str());
            }
        }

        // Advance the lexer's current index to the first non-space code char we found.
        while (!eof() && i < first_code_pos) advance();
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
    if (c == ' ' || c == '\t' || c == '\r') {
        advance();
        return;
    }
    
    if(c == '=' && peek_next() == '>' && peek(2) == '>') {
      advance();
      advance();
      advance();
      add_token(out, TokenType::OPENBRACE, "{", line, col, 1);
      while(peek() != '\n') {
        scan_token(out);
      }
      add_token(out, TokenType::CLOSEBRACE, "}", line, col, 1);
      return;
    }

    if (c == '\n') {
        handle_newline(out);
        return;
    }

    // comments: '#' or '//' style
    if (c == '#') {
        skip_line_comment();
        return;
    }
    if (c == '/' && peek_next() == '/') {
        advance();
        advance();
        skip_line_comment();
        return;
    }
    if (c == '/' && peek_next() == '*') {
        advance();
        advance();
        while (!eof()) {
            if (peek() == '*' && peek_next() == '/') {
                advance();
                advance();
                break;
            }
            advance();
        }
        return;
    }

    if (c == '.' && peek_next() == '.' && peek(2) == '.') {
        add_token(out, TokenType::ELLIPSIS, "...", line, col, 3);
        advance();
        advance();
        advance();
        return;
    }
    if (c == '?' && peek_next() == '.') {
        add_token(out, TokenType::QUESTION_DOT, "?.", line, col, 2);
        advance();  // consume '?'
        advance();  // consume '.'
        return;
    }
    if (c == '=' && peek_next() == '=' && peek(2) == '=') {
        add_token(out, TokenType::STRICT_EQUALITY, "===", line, col, 3);
        advance();
        advance();
        advance();
        return;
    }
    if (c == '!' && peek_next() == '=' && peek(2) == '=') {
        add_token(out, TokenType::STRICT_NOTEQUAL, "!==", line, col, 3);
        advance();
        advance();
        advance();
        return;
    }
    

    // two-char operators
    if (c == '*' && peek_next() == '*') {
        add_token(out, TokenType::POWER, "**", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '=' && peek_next() == '=') {
        add_token(out, TokenType::EQUALITY, "==", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '=' && peek_next() == '>') {
        add_token(out, TokenType::LAMBDA, "=>", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '!' && peek_next() == '=') {
        add_token(out, TokenType::NOTEQUAL, "!=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '>' && peek_next() == '=') {
        add_token(out, TokenType::GREATEROREQUALTHAN, ">=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '<' && peek_next() == '=') {
        add_token(out, TokenType::LESSOREQUALTHAN, "<=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '+' && peek_next() == '+') {
        add_token(out, TokenType::INCREMENT, "++", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '-' && peek_next() == '-') {
        add_token(out, TokenType::DECREMENT, "--", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '+' && peek_next() == '=') {
        add_token(out, TokenType::PLUS_ASSIGN, "+=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '-' && peek_next() == '=') {
        add_token(out, TokenType::MINUS_ASSIGN, "-=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '*' && peek_next() == '=') {
        add_token(out, TokenType::TIMES_ASSIGN, "*=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '&' && peek_next() == '&') {
        add_token(out, TokenType::AND, "&&", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '|' && peek_next() == '|') {
        add_token(out, TokenType::OR, "||", line, col, 2);
        advance();
        advance();
        return;
    }

    // single-char tokens & bookkeeping
    switch (c) {
        case ';':
            add_token(out, TokenType::SEMICOLON, ";", line, col, 1);
            advance();
            return;
        case ',':
            add_token(out, TokenType::COMMA, ",", line, col, 1);
            advance();
            return;
        case '(':
            paren_level++;
            add_token(out, TokenType::OPENPARENTHESIS, "(", line, col, 1);
            advance();
            return;
        case ')':
            if (paren_level > 0) paren_level--;
            add_token(out, TokenType::CLOSEPARENTHESIS, ")", line, col, 1);
            advance();
            return;
        case '{':
            add_token(out, TokenType::OPENBRACE, "{", line, col, 1);
            advance();
            return;
        case '}':
            add_token(out, TokenType::CLOSEBRACE, "}", line, col, 1);
            advance();
            return;
        case '[':
            add_token(out, TokenType::OPENBRACKET, "[", line, col, 1);
            advance();
            return;
        case ']':
            add_token(out, TokenType::CLOSEBRACKET, "]", line, col, 1);
            advance();
            return;
        case '$':
            add_token(out, TokenType::SELF, "$", line, col, 1);
            advance();
            return;
        case '.':
            add_token(out, TokenType::DOT, ".", line, col, 1);
            advance();
            return;
        case ':':
            add_token(out, TokenType::COLON, ":", line, col, 1);
            advance();
            return;
        case '?':
            add_token(out, TokenType::QUESTIONMARK, "?", line, col, 1);
            advance();
            return;
        case '@':
            add_token(out, TokenType::AT_SIGN, "@", line, col, 1);
            advance();
            return;
        case '&':
            add_token(out, TokenType::AMPERSAND, "&", line, col, 1);
            advance();
            return;
        case '~':
            add_token(out, TokenType::TILDE, "~", line, col, 1);
            advance();
            return;
        case '=':
            add_token(out, TokenType::ASSIGN, "=", line, col, 1);
            advance();
            return;
        case '+':
            add_token(out, TokenType::PLUS, "+", line, col, 1);
            advance();
            return;
        case '-':
            add_token(out, TokenType::MINUS, "-", line, col, 1);
            advance();
            return;
        case '*':
            add_token(out, TokenType::STAR, "*", line, col, 1);
            advance();
            return;
        case '/':
            add_token(out, TokenType::SLASH, "/", line, col, 1);
            advance();
            return;
        case '%':
            add_token(out, TokenType::PERCENT, "%", line, col, 1);
            advance();
            return;
        case '>':
            add_token(out, TokenType::GREATERTHAN, ">", line, col, 1);
            advance();
            return;
        case '<':
            add_token(out, TokenType::LESSTHAN, "<", line, col, 1);
            advance();
            return;
        case '!':
            add_token(out, TokenType::NOT, "!", line, col, 1);
            advance();
            return;
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
        default:
            break;
    }

    // number
    if (std::isdigit((unsigned char)c)) {
        size_t start_index = i;
        scan_number(out, line, col, start_index);
        return;
    }

    // identifier or keyword
    if (std::isalpha((unsigned char)c) || c == '_') {
        size_t start_index = i;
        scan_identifier_or_keyword(out, line, col, start_index);
        return;
    }

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
    
    for(auto& tok: out) {
      tok.loc.set_map_linestr(linestr);
    }
    
    return out;
}