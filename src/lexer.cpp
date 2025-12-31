#include "lexer.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

// Constructor
Lexer::Lexer(const std::string& source, const std::string& filename, const SourceManager* mgr)
    : src(source), filename(filename), i(0), line(1), col(1), src_mgr(mgr), regex_allowed(true) {
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
        line++;
        col = 1;
    } else {
        col++;
    }
    return c;
}

void Lexer::add_token(std::vector<Token>& out, TokenType type, const std::string& value, int tok_line, int tok_col, int tok_length) {
    int len = tok_length >= 0 ? tok_length : static_cast<int>(value.size());
    TokenLocation loc(filename.empty() ? "<repl>" : filename, tok_line, tok_col, len, src_mgr);
    Token t{type, value, loc};
    out.push_back(std::move(t));
    update_regex_allowed(type);
}

void Lexer::skip_line_comment() {
    while (!eof() && peek() != '\n') advance();
}

void Lexer::update_regex_allowed(TokenType type) {
    switch (type) {
        // Tokens that END an expression -> next '/' is division
        case TokenType::IDENTIFIER:
        case TokenType::NUMBER:
        case TokenType::STRING:
        case TokenType::SINGLE_QUOTED_STRING:
        case TokenType::TEMPLATE_END:
        case TokenType::CLOSEPARENTHESIS:
        case TokenType::CLOSEBRACKET:
        case TokenType::CLOSEBRACE:
        case TokenType::BOOLEAN:
        case TokenType::NULL_LITERAL:
        case TokenType::NAN_LITERAL:
        case TokenType::INF_LITERAL:
        case TokenType::SELF:
        case TokenType::DATETIME_LITERAL:
        case TokenType::INCREMENT:  // treating as postfix for now
        case TokenType::DECREMENT:  // treating as postfix for now
            regex_allowed = false;
            break;

        // Tokens that EXPECT an expression -> next '/' starts regex
        case TokenType::ASSIGN:
        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::PERCENT:
        case TokenType::POWER:
        case TokenType::EQUALITY:
        case TokenType::NOTEQUAL:
        case TokenType::STRICT_EQUALITY:
        case TokenType::STRICT_NOTEQUAL:
        case TokenType::LESSTHAN:
        case TokenType::GREATERTHAN:
        case TokenType::LESSOREQUALTHAN:
        case TokenType::GREATEROREQUALTHAN:
        case TokenType::AND:
        case TokenType::OR:
        case TokenType::NOT:
        case TokenType::COMMA:
        case TokenType::COLON:
        case TokenType::SEMICOLON:
        case TokenType::OPENPARENTHESIS:
        case TokenType::OPENBRACKET:
        case TokenType::OPENBRACE:
        case TokenType::PLUS_ASSIGN:
        case TokenType::MINUS_ASSIGN:
        case TokenType::TIMES_ASSIGN:
        case TokenType::SLASH_ASSIGN:
        case TokenType::PERCENT_ASSIGN:
        case TokenType::RUDISHA:  // return
        case TokenType::KAMA:     // if
        case TokenType::IKIWA:
        case TokenType::LAMBDA:
        case TokenType::QUESTIONMARK:
        case TokenType::NULLISH:
        case TokenType::BIT_OR:
        case TokenType::BIT_XOR:
        case TokenType::AMPERSAND:
        case TokenType::BIT_SHIFT_LEFT:
        case TokenType::BIT_SHIFT_RIGHT:
        case TokenType::BIT_TRIPLE_RSHIFT:
            regex_allowed = true;
            break;

        // Layout tokens don't change the state
        case TokenType::NEWLINE:
        case TokenType::INDENT:
        case TokenType::DEDENT:
            // keep current state
            break;

        default:
            // Default to allowing regex (safer)
            regex_allowed = true;
            break;
    }
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
    throw std::runtime_error(ss.str());
}

bool Lexer::try_scan_datetime(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    // Quick validation: must start with 4 digits followed by '-'
    if (!std::isdigit((unsigned char)peek(0)) ||
        !std::isdigit((unsigned char)peek(1)) ||
        !std::isdigit((unsigned char)peek(2)) ||
        !std::isdigit((unsigned char)peek(3)) ||
        peek(4) != '-') {
        return false;
    }

    // Lookahead to check if this matches datetime pattern YYYY-MM-DDTHH:MM
    // We need to see at least: YYYY-MM-DDTHH:MM (16 chars minimum)
    size_t lookahead = 0;
    auto check_digit = [&]() -> bool {
        return lookahead < 50 && (i + lookahead) < src.size() &&
            std::isdigit((unsigned char)src[i + lookahead++]);
    };
    auto check_char = [&](char expected) -> bool {
        return lookahead < 50 && (i + lookahead) < src.size() &&
            src[i + lookahead++] == expected;
    };

    // Check pattern: YYYY-MM-DDTHH:MM
    if (!(check_digit() && check_digit() && check_digit() && check_digit() &&  // YYYY
            check_char('-') &&
            check_digit() && check_digit() &&  // MM
            check_char('-') &&
            check_digit() && check_digit() &&  // DD
            check_char('T') &&
            check_digit() && check_digit() &&  // HH
            check_char(':') &&
            check_digit() && check_digit())) {  // MM
        return false;                           // Not a datetime pattern
    }

    // Pattern matches - commit to scanning datetime
    auto throw_error = [&](const std::string& msg) -> void {
        std::ostringstream ss;
        ss << msg << " in datetime literal at "
           << (filename.empty() ? "<repl>" : filename)
           << " line " << tok_line << ", col " << tok_col;
        throw std::runtime_error(ss.str());
    };

    auto scan_digits = [&](int count, const std::string& component) -> int {
        int value = 0;
        for (int j = 0; j < count; j++) {
            if (eof() || !std::isdigit((unsigned char)peek())) {
                throw_error("Expected " + std::to_string(count) + " digits for " + component);
            }
            value = value * 10 + (peek() - '0');
            advance();
        }
        return value;
    };

    auto expect_char = [&](char expected, const std::string& context) -> void {
        if (eof() || peek() != expected) {
            throw_error("Expected '" + std::string(1, expected) + "' " + context);
        }
        advance();
    };

    // Scan YYYY
    int year = scan_digits(4, "year");
    expect_char('-', "after year");

    // Scan MM
    int month = scan_digits(2, "month");
    if (month < 1 || month > 12) {
        throw_error("Invalid month '" + std::to_string(month) + "' (must be 01-12)");
    }
    expect_char('-', "after month");

    // Scan DD
    int day = scan_digits(2, "day");
    if (day < 1 || day > 31) {
        throw_error("Invalid day '" + std::to_string(day) + "' (must be 01-31)");
    }

    // Validate day against month/year (calendar validation)
    auto is_leap_year = [](int y) -> bool {
        return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
    };

    int max_day = 31;
    if (month == 2) {
        max_day = is_leap_year(year) ? 29 : 28;
    } else if (month == 4 || month == 6 || month == 9 || month == 11) {
        max_day = 30;
    }

    if (day > max_day) {
        std::ostringstream msg;
        msg << "Invalid day '" << day << "' for "
            << (month == 2 ? (is_leap_year(year) ? "February (leap year)" : "February")
                           : "month " + std::to_string(month))
            << " " << year << " (max " << max_day << ")";
        throw_error(msg.str());
    }

    expect_char('T', "after date (required separator)");

    // Scan HH
    int hour = scan_digits(2, "hour");
    if (hour > 23) {
        throw_error("Invalid hour '" + std::to_string(hour) + "' (must be 00-23)");
    }
    expect_char(':', "after hour");

    // Scan MM (minutes)
    int minute = scan_digits(2, "minute");
    if (minute > 59) {
        throw_error("Invalid minute '" + std::to_string(minute) + "' (must be 00-59)");
    }

    // Optional: seconds
    int second = 0;
    std::string fraction;
    if (!eof() && peek() == ':') {
        advance();  // consume ':'
        second = scan_digits(2, "second");
        if (second > 59) {
            throw_error("Invalid second '" + std::to_string(second) + "' (must be 00-59)");
        }

        // Optional: fractional seconds
        if (!eof() && peek() == '.') {
            advance();  // consume '.'
            if (eof() || !std::isdigit((unsigned char)peek())) {
                throw_error("Expected digits after decimal point in fractional seconds");
            }
            while (!eof() && std::isdigit((unsigned char)peek())) {
                fraction.push_back(peek());
                advance();
            }
        }
    }

    // Mandatory timezone
    if (eof()) {
        throw_error("Incomplete datetime: missing required timezone (Z or ±HH:MM)");
    }
    int32_t tz_offset_total_seconds = 0;
    char tz_char = peek();
    if (tz_char == 'Z') {
        advance();
        tz_offset_total_seconds = 0;
    } else if (tz_char == '+' || tz_char == '-') {
        char sign = tz_char;
        advance();

        // Scan timezone offset hours
        if (eof() || !std::isdigit((unsigned char)peek())) {
            throw_error("Expected digits for timezone offset hours");
        }
        int tz_hour = scan_digits(2, "timezone hour");

        int tz_minute = 0;

        // Check for colon or 2 more digits (supports +HH:MM and +HHMM)
        if (!eof() && peek() == ':') {
            advance();  // consume ':'
            tz_minute = scan_digits(2, "timezone minute");
        } else if (!eof() && std::isdigit((unsigned char)peek())) {
            // Compact form: +HHMM
            tz_minute = scan_digits(2, "timezone minute");
        }
        // else: +HH format (tz_minute stays 0)

        // Validate timezone offset range (-12:00 to +14:00)
        int total_offset = tz_hour * 60 + tz_minute;
        if (tz_hour > 14 || (tz_hour == 14 && tz_minute > 0)) {
            throw_error("Timezone offset too large (max +14:00)");
        }
        if (tz_minute > 59) {
            throw_error("Invalid timezone minute '" + std::to_string(tz_minute) + "' (must be 00-59)");
        }
        // Note: negative offsets are validated implicitly by the sign
        tz_offset_total_seconds = (sign == '+' ? 1 : -1) * (tz_hour * 3600 + tz_minute * 60);
    } else {
        throw_error("Invalid or missing timezone (expected 'Z' or '±HH:MM')");
    }

    int tok_length = static_cast<int>(i - start_index);

    auto days_since_epoch = [](int y, int m, int d) -> int64_t {
        // Algorithm: compute days for Gregorian calendar
        int64_t year_adj = y;
        if (m <= 2) {
            year_adj--;
            m += 12;
        }
        int64_t era = (year_adj >= 0 ? year_adj : year_adj - 399) / 400;
        int64_t yoe = year_adj - era * 400;
        int64_t doy = (153 * (m - 3) + 2) / 5 + d - 1;
        int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;  // 719468 = days from 0000-03-01 to 1970-01-01
    };

    int64_t days = days_since_epoch(year, month, day);
    int64_t seconds_in_day = (int64_t)hour * 3600 + minute * 60 + second;
    int64_t total_seconds = days * 86400 + seconds_in_day - tz_offset_total_seconds;

    // Parse fractional seconds
    uint32_t frac_nanos = 0;
    DateTimePrecision prec = DateTimePrecision::SECOND;
    if (!fraction.empty()) {
        // Pad or truncate to 9 digits
        std::string frac_str = fraction;
        if (frac_str.size() > 9) frac_str = frac_str.substr(0, 9);
        while (frac_str.size() < 9) frac_str += '0';
        frac_nanos = static_cast<uint32_t>(std::stoul(frac_str));

        // Determine precision
        if (fraction.size() <= 3)
            prec = DateTimePrecision::MILLISECOND;
        else if (fraction.size() <= 6)
            prec = DateTimePrecision::MICROSECOND;
        else
            prec = DateTimePrecision::NANOSECOND;
    }

    uint64_t epoch_nanos = static_cast<uint64_t>(total_seconds) * 1000000000ULL + frac_nanos;

    // Store metadata in token value as structured format (we'll parse in parser)
    // Format: "ISO_STRING|EPOCH_NANOS|FRAC_NANOS|PRECISION|TZ_OFFSET|IS_UTC"
    std::string datetime_value = src.substr(start_index, tok_length);
    std::ostringstream meta;
    meta << datetime_value << "|"
         << epoch_nanos << "|"
         << frac_nanos << "|"
         << static_cast<int>(prec) << "|"
         << tz_offset_total_seconds << "|"
         << (tz_char == 'Z' ? "1" : "0");

    add_token(out, TokenType::DATETIME_LITERAL, meta.str(), tok_line, tok_col, tok_length);

    return true;
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
        {"ktk", TokenType::KATIKA},
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

        {"yield", TokenType::YIELD},

        {"step", TokenType::STEP},
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
void Lexer::scan_regex_literal(std::vector<Token>& out, int tok_line, int tok_col, size_t start_index) {
    // consume opening '/'
    advance();

    std::string pattern;
    bool escaped = false;
    bool in_char_class = false;

    while (!eof()) {
        char c = peek();

        // Handle escape sequences
        if (escaped) {
            pattern.push_back(c);
            advance();
            escaped = false;
            continue;
        }

        if (c == '\\') {
            pattern.push_back(advance());
            escaped = true;
            continue;
        }

        // Track character classes [...]
        if (c == '[' && !in_char_class) {
            in_char_class = true;
            pattern.push_back(advance());
            continue;
        }

        if (c == ']' && in_char_class) {
            in_char_class = false;
            pattern.push_back(advance());
            continue;
        }

        // End of regex (not inside character class)
        if (c == '/' && !in_char_class) {
            advance();  // consume closing '/'

            // Scan optional flags (g, i, m, s, u, y)
            std::string flags;
            while (!eof() && std::isalpha((unsigned char)peek())) {
                char flag = peek();
                if (flag == 'g' || flag == 'i' || flag == 'm' ||
                    flag == 's' || flag == 'u' || flag == 'y') {
                    flags.push_back(advance());
                } else {
                    break;
                }
            }

            // Store pattern and flags in token value
            std::string value = pattern;
            if (!flags.empty()) {
                value += "|" + flags;
            }

            int tok_length = static_cast<int>(i - start_index);
            add_token(out, TokenType::REGEX_LITERAL, value, tok_line, tok_col, tok_length);
            return;
        }

        // Newline in regex (without escape) is an error
        if (c == '\n' && !in_char_class) {
            std::ostringstream ss;
            ss << "Unterminated regex literal at "
               << (filename.empty() ? "<repl>" : filename)
               << " line " << tok_line << ", col " << tok_col;
            throw std::runtime_error(ss.str());
        }

        pattern.push_back(advance());
    }

    // Reached EOF without closing /
    std::ostringstream ss;
    ss << "Unterminated regex literal at "
       << (filename.empty() ? "<repl>" : filename)
       << " line " << tok_line << ", col " << tok_col;
    throw std::runtime_error(ss.str());
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

    if (c == '*' && peek_next() == '*' && peek(2) == '=') {
        add_token(out, TokenType::DOUBLESTAR_ASSIGN, "**=", line, col, 3);
        advance();
        advance();
        advance();
        return;
    }
    if (c == '&' && peek_next() == '&' && peek(2) == '=') {
        add_token(out, TokenType::AND_ASSIGN, "&&=", line, col, 3);
        advance();
        advance();
        advance();
        return;
    }
    if (c == '|' && peek_next() == '|' && peek(2) == '=') {
        add_token(out, TokenType::OR_ASSIGN, "||=", line, col, 3);
        advance();
        advance();
        advance();
        return;
    }
    if (c == '?' && peek_next() == '?' && peek(2) == '=') {
        add_token(out, TokenType::NULLISH_ASSIGN, "??=", line, col, 3);
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
    if (c == ':' && peek_next() == '=') {
        add_token(out, TokenType::WALRUS, ":=", line, col, 2);
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
    if (c == '/' && peek_next() == '=') {
        add_token(out, TokenType::SLASH_ASSIGN, "/=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '%' && peek_next() == '=') {
        add_token(out, TokenType::PERCENT_ASSIGN, "%=", line, col, 2);
        advance();
        advance();
        return;
    }

    if (c == '&' && peek_next() == '=') {
        add_token(out, TokenType::BIT_AND_ASSIGN, "&=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '|' && peek_next() == '=') {
        add_token(out, TokenType::BIT_OR_ASSIGN, "|=", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '^' && peek_next() == '=') {
        add_token(out, TokenType::BIT_XOR_ASSIGN, "|=", line, col, 2);
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
    if (c == '?' && peek_next() == '?') {
        add_token(out, TokenType::NULLISH, "??", line, col, 2);
        advance();
        advance();
        return;
    }
    if (c == '.' && peek_next() == '.') {
        add_token(out, TokenType::DOUBLEDOTS, "..", line, col, 2);
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
        case '/': {
            if (regex_allowed) {
                size_t start_index = i;
                int start_line = line;
                int start_col = col;
                scan_regex_literal(out, start_line, start_col, start_index);
                return;
            }
            add_token(out, TokenType::SLASH, "/", line, col, 1);
            advance();
            return;
        }
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

    // number (decimal or prefixed non-decimal) or datetime literal
    if (std::isdigit((unsigned char)c)) {
        size_t start_index = i;
        int tok_line = line;
        int tok_col = col;

        // First, try datetime literal (must be 4-digit year starting pattern)
        if (c >= '0' && c <= '9' && try_scan_datetime(out, tok_line, tok_col, start_index)) {
            return;
        }

        // Not datetime, try 0b/0o/0x prefix handling
        if (c == '0' && handle_non_decimal_number(out, tok_line, tok_col, start_index)) {
            return;
        }

        // Otherwise fall back to decimal/floating scanner
        scan_number(out, tok_line, tok_col, start_index);
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

    return out;
}