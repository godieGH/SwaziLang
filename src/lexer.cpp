#include "lexer.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
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
            advance();  // consume backslash
            char nxt = peek();

            // Standard single-character escapes
            if (nxt == 'n') {
                val.push_back('\n');
                advance();
            } else if (nxt == 't') {
                val.push_back('\t');
                advance();
            } else if (nxt == 'r') {
                val.push_back('\r');
                advance();
            } else if (nxt == 'b') {
                val.push_back('\b');
                advance();
            } else if (nxt == 'f') {
                val.push_back('\f');
                advance();
            } else if (nxt == 'v') {
                val.push_back('\v');
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
            }
            // Octal escape: \0, \00, \000, \033, \177, etc.
            else if (nxt >= '0' && nxt <= '7') {
                int octal_value = 0;
                int digits = 0;

                // Parse up to 3 octal digits
                while (!eof() && digits < 3 && peek() >= '0' && peek() <= '7') {
                    octal_value = octal_value * 8 + (peek() - '0');
                    advance();
                    digits++;
                }

                // Clamp to valid byte range [0, 255]
                if (octal_value > 255) {
                    std::ostringstream ss;
                    ss << "Octal escape sequence out of range (> 255) at "
                       << (filename.empty() ? "<repl>" : filename)
                       << " line " << tok_line << ", col " << tok_col;
                    throw std::runtime_error(ss.str());
                }

                val.push_back(static_cast<char>(octal_value));
            }
            // Hex escape: \x1b, \xff, \x00
            else if (nxt == 'x') {
                advance();  // consume 'x'

                if (!eof() && std::isxdigit((unsigned char)peek())) {
                    int hex_value = 0;
                    int digits = 0;

                    // Parse exactly 2 hex digits
                    while (!eof() && digits < 2 && std::isxdigit((unsigned char)peek())) {
                        char hc = peek();
                        int digit;
                        if (hc >= '0' && hc <= '9')
                            digit = hc - '0';
                        else if (hc >= 'a' && hc <= 'f')
                            digit = 10 + (hc - 'a');
                        else if (hc >= 'A' && hc <= 'F')
                            digit = 10 + (hc - 'A');
                        else
                            break;

                        hex_value = hex_value * 16 + digit;
                        advance();
                        digits++;
                    }

                    if (digits > 0) {
                        val.push_back(static_cast<char>(hex_value));
                    } else {
                        // Invalid hex escape - keep literal \x
                        val.push_back('\\');
                        val.push_back('x');
                    }
                } else {
                    // No hex digits after \x - keep literal
                    val.push_back('\\');
                    val.push_back('x');
                }
            }
            // Unicode escape: \u2713, \u00e9 (4 hex digits)
            else if (nxt == 'u') {
                advance();  // consume 'u'

                // Must have exactly 4 hex digits
                bool valid = true;
                uint32_t codepoint = 0;

                for (int j = 0; j < 4; j++) {
                    if (eof() || !std::isxdigit((unsigned char)peek())) {
                        valid = false;
                        break;
                    }

                    char hc = peek();
                    int digit;
                    if (hc >= '0' && hc <= '9')
                        digit = hc - '0';
                    else if (hc >= 'a' && hc <= 'f')
                        digit = 10 + (hc - 'a');
                    else if (hc >= 'A' && hc <= 'F')
                        digit = 10 + (hc - 'A');
                    else {
                        valid = false;
                        break;
                    }

                    codepoint = (codepoint << 4) | digit;
                    advance();
                }

                if (valid) {
                    // Convert Unicode codepoint to UTF-8
                    if (codepoint <= 0x7F) {
                        // 1-byte UTF-8
                        val.push_back(static_cast<char>(codepoint));
                    } else if (codepoint <= 0x7FF) {
                        // 2-byte UTF-8
                        val.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                        val.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    } else if (codepoint <= 0xFFFF) {
                        // 3-byte UTF-8
                        val.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                        val.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        val.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    } else {
                        // 4-byte UTF-8 (for completeness, though \u only goes to 0xFFFF)
                        val.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                        val.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                        val.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        val.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                } else {
                    // Invalid unicode escape - keep literal \u
                    val.push_back('\\');
                    val.push_back('u');
                }
            }
            // Unknown escape - keep the character as-is (e.g., \a remains as 'a')
            else {
                val.push_back(nxt);
                advance();
            }
            continue;
        }

        // Regular character - allow multiline strings
        val.push_back(advance());
    }

    int tok_length = static_cast<int>(i - start_index);
    TokenType tt = (quote == '\'') ? TokenType::SINGLE_QUOTED_STRING : TokenType::STRING;
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
            advance();  // consume backslash
            char nxt = peek();

            // Standard single-character escapes
            if (nxt == 'n') {
                chunk.push_back('\n');
                advance();
            } else if (nxt == 't') {
                chunk.push_back('\t');
                advance();
            } else if (nxt == 'r') {
                chunk.push_back('\r');
                advance();
            } else if (nxt == 'b') {
                chunk.push_back('\b');
                advance();
            } else if (nxt == 'f') {
                chunk.push_back('\f');
                advance();
            } else if (nxt == 'v') {
                chunk.push_back('\v');
                advance();
            } else if (nxt == '`') {
                chunk.push_back('`');
                advance();
            } else if (nxt == '\\') {
                chunk.push_back('\\');
                advance();
            } else if (nxt == '$') {
                // Allow escaping $ to prevent interpolation: \${ -> literal ${
                chunk.push_back('$');
                advance();
            }
            // Octal escape: \0, \00, \000, \033, \177, etc.
            else if (nxt >= '0' && nxt <= '7') {
                int octal_value = 0;
                int digits = 0;

                // Parse up to 3 octal digits
                while (!eof() && digits < 3 && peek() >= '0' && peek() <= '7') {
                    octal_value = octal_value * 8 + (peek() - '0');
                    advance();
                    digits++;
                }

                // Clamp to valid byte range [0, 255]
                if (octal_value > 255) {
                    std::ostringstream ss;
                    ss << "Octal escape sequence out of range (> 255) in template at "
                       << (filename.empty() ? "<repl>" : filename)
                       << " line " << tok_line << ", col " << tok_col;
                    throw std::runtime_error(ss.str());
                }

                chunk.push_back(static_cast<char>(octal_value));
            }
            // Hex escape: \x1b, \xff, \x00
            else if (nxt == 'x') {
                advance();  // consume 'x'

                if (!eof() && std::isxdigit((unsigned char)peek())) {
                    int hex_value = 0;
                    int digits = 0;

                    // Parse exactly 2 hex digits
                    while (!eof() && digits < 2 && std::isxdigit((unsigned char)peek())) {
                        char hc = peek();
                        int digit;
                        if (hc >= '0' && hc <= '9')
                            digit = hc - '0';
                        else if (hc >= 'a' && hc <= 'f')
                            digit = 10 + (hc - 'a');
                        else if (hc >= 'A' && hc <= 'F')
                            digit = 10 + (hc - 'A');
                        else
                            break;

                        hex_value = hex_value * 16 + digit;
                        advance();
                        digits++;
                    }

                    if (digits > 0) {
                        chunk.push_back(static_cast<char>(hex_value));
                    } else {
                        // Invalid hex escape - keep literal \x
                        chunk.push_back('\\');
                        chunk.push_back('x');
                    }
                } else {
                    // No hex digits after \x - keep literal
                    chunk.push_back('\\');
                    chunk.push_back('x');
                }
            }
            // Unicode escape: \u2713, \u00e9 (4 hex digits)
            else if (nxt == 'u') {
                advance();  // consume 'u'

                // Must have exactly 4 hex digits
                bool valid = true;
                uint32_t codepoint = 0;

                for (int j = 0; j < 4; j++) {
                    if (eof() || !std::isxdigit((unsigned char)peek())) {
                        valid = false;
                        break;
                    }

                    char hc = peek();
                    int digit;
                    if (hc >= '0' && hc <= '9')
                        digit = hc - '0';
                    else if (hc >= 'a' && hc <= 'f')
                        digit = 10 + (hc - 'a');
                    else if (hc >= 'A' && hc <= 'F')
                        digit = 10 + (hc - 'A');
                    else {
                        valid = false;
                        break;
                    }

                    codepoint = (codepoint << 4) | digit;
                    advance();
                }

                if (valid) {
                    // Convert Unicode codepoint to UTF-8
                    if (codepoint <= 0x7F) {
                        // 1-byte UTF-8
                        chunk.push_back(static_cast<char>(codepoint));
                    } else if (codepoint <= 0x7FF) {
                        // 2-byte UTF-8
                        chunk.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
                        chunk.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    } else if (codepoint <= 0xFFFF) {
                        // 3-byte UTF-8
                        chunk.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
                        chunk.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        chunk.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    } else {
                        // 4-byte UTF-8 (for completeness)
                        chunk.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
                        chunk.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
                        chunk.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                        chunk.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                    }
                } else {
                    // Invalid unicode escape - keep literal \u
                    chunk.push_back('\\');
                    chunk.push_back('u');
                }
            }
            // Unknown escape - keep the character as-is
            else {
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
    sd << " * " << tok_line << " | ";
    ss << "\n --> Traced at: \n"
       << sd.str() << linestr[tok_line] << "\n"
       << std::string(sd.str().size() + (tok_col - 6), ' ') << "~~~^~~~";
    throw std::runtime_error(ss.str());
}

// Returns true when it recognized & emitted a prefixed non-decimal literal; false otherwise.
// On malformed literals it throws std::runtime_error with a helpful message.
bool Lexer::handle_non_decimal_number(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    // must start with '0' followed by base indicator
    if (peek() != '0') return false;
    char p = peek_next();
    int base = 0;
    if (p == 'b' || p == 'B')
        base = 2;
    else if (p == 'o' || p == 'O')
        base = 8;
    else if (p == 'x' || p == 'X')
        base = 16;
    else
        return false;

    // consume '0' and the prefix letter
    advance();  // '0'
    advance();  // prefix

    // collect raw digits (may include underscores)
    std::string raw_digits;
    while (!eof()) {
        char c = peek();
        // digits and underscores allowed; stop on first char that isn't digit/underscore/alpha(for hex)
        if (c == '_') {
            raw_digits.push_back(advance());
            continue;
        }
        if (base == 2) {
            if (c == '0' || c == '1') {
                raw_digits.push_back(advance());
                continue;
            }
            break;
        } else if (base == 8) {
            if (c >= '0' && c <= '7') {
                raw_digits.push_back(advance());
                continue;
            }
            break;
        } else {  // base == 16
            if (std::isxdigit((unsigned char)c)) {
                raw_digits.push_back(advance());
                continue;
            }
            break;
        }
    }

    int tok_length = static_cast<int>(i - start_index);  // raw source length (prefix + digits + underscores)

    // helper to throw diagnostic similar to template error style (with trace line + caret/tilde)
    auto throw_diag = [&](const std::string& headline) -> void {
        std::ostringstream ss;
        ss << headline << " in file '" << (filename.empty() ? "<repl>" : filename)
           << "' starting at line " << tok_line << ", col " << tok_col;
        std::stringstream sd;
        sd << " * " << tok_line << " | ";
        ss << "\n --> Traced at: \n"
           << sd.str() << linestr[tok_line] << "\n"
           // mark the full raw token span with tildes and a caret at the end for visibility
           << std::string(sd.str().size() + std::max(0, tok_col - 1), ' ')
           << std::string(std::max(1, tok_length), '~') << "^";
        throw std::runtime_error(ss.str());
    };

    // validation: must have at least one digit (ignore underscores)
    std::string digits_no_unders;
    for (char ch : raw_digits)
        if (ch != '_') digits_no_unders.push_back(ch);
    if (digits_no_unders.empty()) {
        std::ostringstream head;
        head << "Invalid numeric literal (missing digits after '0" << (char)std::toupper((unsigned char)p) << "')";
        throw_diag(head.str());
    }

    // validate underscore usage: not leading, not trailing, no consecutive underscores
    if (!raw_digits.empty() && (raw_digits.front() == '_' || raw_digits.back() == '_')) {
        std::ostringstream head;
        head << "Invalid numeric separator placement in literal (leading/trailing underscore not allowed)";
        throw_diag(head.str());
    }
    for (size_t k = 1; k < raw_digits.size(); ++k) {
        if (raw_digits[k] == '_' && raw_digits[k - 1] == '_') {
            std::ostringstream head;
            head << "Invalid numeric separator (consecutive underscores) in literal";
            throw_diag(head.str());
        }
    }

    // validate characters against base (defensive check)
    for (char ch : digits_no_unders) {
        bool ok = false;
        if (base == 2)
            ok = (ch == '0' || ch == '1');
        else if (base == 8)
            ok = (ch >= '0' && ch <= '7');
        else
            ok = std::isxdigit((unsigned char)ch);
        if (!ok) {
            std::ostringstream head;
            head << "Invalid digit '" << ch << "' for base-" << base << " literal";
            throw_diag(head.str());
        }
    }

    // Convert base-N digit string to decimal string using string-based big-integer multiply-add:
    auto mul_decimal_by_small = [](std::string& dec, int m) {
        // dec is decimal ASCII digits, most-significant-first.
        int carry = 0;
        for (int i = (int)dec.size() - 1; i >= 0; --i) {
            int d = dec[i] - '0';
            long long prod = 1LL * d * m + carry;
            dec[i] = char('0' + (prod % 10));
            carry = (int)(prod / 10);
        }
        while (carry > 0) {
            char digit = char('0' + (carry % 10));
            dec.insert(dec.begin(), digit);
            carry /= 10;
        }
    };
    auto add_small_to_decimal = [](std::string& dec, int add) {
        int carry = add;
        for (int i = (int)dec.size() - 1; i >= 0 && carry > 0; --i) {
            int d = dec[i] - '0';
            int sum = d + carry;
            dec[i] = char('0' + (sum % 10));
            carry = sum / 10;
        }
        while (carry > 0) {
            char digit = char('0' + (carry % 10));
            dec.insert(dec.begin(), digit);
            carry /= 10;
        }
    };

    std::string decimal = "0";
    for (char ch : digits_no_unders) {
        int digit_val = 0;
        if (ch >= '0' && ch <= '9')
            digit_val = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            digit_val = 10 + (ch - 'a');
        else if (ch >= 'A' && ch <= 'F')
            digit_val = 10 + (ch - 'A');
        // multiply decimal by base then add digit_val
        mul_decimal_by_small(decimal, base);
        add_small_to_decimal(decimal, digit_val);
    }

    add_token(out, TokenType::NUMBER, decimal, tok_line, tok_col, tok_length);
    return true;
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
        } else if (c == '_' && std::isdigit((unsigned char)peek_next()) && !seen_dot) {
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
        {"sivyo", TokenType::VINGINEVYO},       // else alias

        {"jaribu", TokenType::JARIBU},
        {"makosa", TokenType::MAKOSA},
        {"kisha", TokenType::KISHA},
        {"tupa", TokenType::THROW},
        {"throw", TokenType::THROW},

        // loop-related keywords
        {"kwa", TokenType::FOR},  // for-like loop
        {"kila", TokenType::KILA},
        {"katika", TokenType::KATIKA},
        {"wakati", TokenType::WHILE},
        {"fanya", TokenType::DOWHILE},
        {"simama", TokenType::SIMAMA},
        {"endelea", TokenType::ENDELEA},

        // keyword literals and constants
        {"null", TokenType::NULL_LITERAL},  // null token
        {"nan", TokenType::NAN_LITERAL},
        {"inf", TokenType::INF_LITERAL},

        {"__block__", TokenType::BLOCK_DU},
        {"__line__", TokenType::LINE_DU},

        // asyncronous async/await
        {"ASYNC", TokenType::ASYNC},
        {"async", TokenType::ASYNC},
        {"await", TokenType::AWAIT},
        {"subiri", TokenType::AWAIT},

        {"yield", TokenType::YIELD}

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

    if (!continuation) {
        size_t p = i;
        // skip only spaces/tabs on the following line (we're already positioned at the start of the next line)
        while (p < src.size() && (src[p] == ' ' || src[p] == '\t' || src[p] == '\r')) p++;
        if (p < src.size()) {
            // simple checks on the raw source: '.' (member), '[' (index), or '?.' (optional member)
            if (src[p] == '.' || src[p] == '[' || (src[p] == '?' && p + 1 < src.size() && src[p + 1] == '.')) {
                // treat as continuation: don't emit NEWLINE
                return;
            }
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

                ss << sd.str() << linestr[line] << "\n"
                   << std::string(sd.str().size() + col, ' ') << "^";

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

    if (c == '=' && peek_next() == '>' && peek(2) == '>') {
        advance();
        advance();
        advance();
        add_token(out, TokenType::OPENBRACE, "{", line, col, 1);
        while (peek() != '\n') {
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
    if (c == '>' && peek_next() == '>' && peek(2) == '>') {
        add_token(out, TokenType::BIT_TRIPLE_RSHIFT, ">>>", line, col, 3);
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

    if (c == '<' && peek_next() == '<') {
        add_token(out, TokenType::BIT_SHIFT_LEFT, "<<", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '>' && peek_next() == '>') {
        add_token(out, TokenType::BIT_SHIFT_RIGHT, ">>", line, col, 2);
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
        case '|':
            add_token(out, TokenType::BIT_OR, "|", line, col, 1);
            advance();
            return;
        case '^':
            add_token(out, TokenType::BIT_XOR, "^", line, col, 1);
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

    // number (decimal or prefixed non-decimal)
    if (std::isdigit((unsigned char)c)) {
        size_t start_index = i;
        // Attempt 0b/0o/0x prefix handling (lexer-level). If handled, it already emitted a NUMBER.
        if (c == '0' && handle_non_decimal_number(out, line, col, start_index)) {
            return;
        }
        // otherwise fall back to existing decimal/floating scanner
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

    for (auto& tok : out) {
        tok.loc.set_map_linestr(linestr);
    }

    return out;
}