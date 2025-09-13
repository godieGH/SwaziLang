#include "lexer.hpp"
#include <map>
#include <cctype>
#include <algorithm>

Lexer::Lexer(const std::string& source, const std::string& filename)
    : source(source), filename(filename) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    std::map<std::string, TokenType> keywords = {
        {"data", TokenType::DATA},
        {"thabiti", TokenType::CONSTANT},
        {"chapisha", TokenType::CHAPISHA},
        {"andika", TokenType::ANDIKA},
        {"kweli", TokenType::BOOLEAN},
        {"sikweli", TokenType::BOOLEAN},
        // keyword-operators
        {"na", TokenType::AND},
        {"au", TokenType::OR},
        {"si", TokenType::NOT},
        {"sawa", TokenType::EQUALITY},
        {"sisawa", TokenType::NOTEQUAL},
    };

    auto peek_char = [&](int offset = 1) -> char {
        size_t idx = position + offset;
        return (idx < source.size()) ? source[idx] : '\0';
    };

    while (position < source.size()) {
        char current = source[position];

        // handle whitespace (and update line/col)
        if (std::isspace(static_cast<unsigned char>(current))) {
            if (current == '\n') {
                position++;
                this->line++;
                this->col = 1;
            } else {
                // space, tab, carriage return etc.
                position++;
                this->col++;
            }
            continue;
        }

        // comments
        if (current == '/' && peek_char() == '/') {
            // single-line comment
            // skip "//"
            position += 2;
            this->col += 2;
            while (position < source.size() && source[position] != '\n') {
                position++;
                this->col++;
            }
            // newline will be handled at top of loop (or afterwards)
            continue;
        }
        if (current == '/' && peek_char() == '*') {
            // block comment
            position += 2;
            this->col += 2;
            while (position + 1 < source.size() && !(source[position] == '*' && peek_char() == '/')) {
                if (source[position] == '\n') {
                    position++;
                    this->line++;
                    this->col = 1;
                } else {
                    position++;
                    this->col++;
                }
            }
            if (position + 1 < source.size()) {
                // skip closing "*/"
                position += 2;
                this->col += 2;
            }
            continue;
        }

        // two-char operators
        if (current == '*' && peek_char() == '*') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::POWER, "**", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }
        if (current == '>' && peek_char() == '=') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::GREATEROREQUALTHAN, ">=", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }
        if (current == '<' && peek_char() == '=') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::LESSOREQUALTHAN, "<=", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }
        if (current == '=' && peek_char() == '=') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::EQUALITY, "==", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }
        if (current == '!' && peek_char() == '=') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::NOTEQUAL, "!=", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }
        if (current == '&' && peek_char() == '&') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::AND, "&&", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }
        if (current == '|' && peek_char() == '|') {
            int token_line = this->line;
            int token_col = this->col;
            tokens.push_back(Token{TokenType::OR, "||", this->filename, token_line, token_col});
            position += 2;
            this->col += 2;
            continue;
        }

        // single-char tokens
        switch (current) {
            case '+': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::PLUS, "+", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '-': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::MINUS, "-", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '*': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::STAR, "*", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '/': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::SLASH, "/", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '%': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::PERCENT, "%", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '=': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::ASSIGN, "=", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '!': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::NOT, "!", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '>': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::GREATERTHAN, ">", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '<': {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::LESSTHAN, "<", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case '(' : {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::OPENPARENTHESIS, "(", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case ')' : {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::CLOSEPARENTHESIS, ")", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case ',' : {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::COMMA, ",", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
            case ';' : {
                int token_line = this->line, token_col = this->col;
                tokens.push_back(Token{TokenType::SEMICOLON, ";", this->filename, token_line, token_col});
                position++; this->col++;
                continue;
            }
        }

        // numbers (integer or decimal)
        if (std::isdigit(static_cast<unsigned char>(current))) {
            int token_line = this->line;
            int token_col = this->col;
            std::string num;
            bool seen_dot = false;
            while (position < source.size() &&
                   (std::isdigit(static_cast<unsigned char>(source[position])) ||
                    (!seen_dot && source[position] == '.'))) {
                if (source[position] == '.') seen_dot = true;
                num.push_back(source[position]);
                position++;
                this->col++;
            }
            tokens.push_back(Token{TokenType::NUMBER, num, this->filename, token_line, token_col});
            continue;
        }

        // strings with basic escapes
        if (current == '"') {
            int token_line = this->line;
            int token_col = this->col;
            // consume opening quote
            position++;
            this->col++;
            std::string str;
            while (position < source.size()) {
                char ch = source[position];
                if (ch == '\\' && position + 1 < source.size()) {
                    char next = source[position + 1];
                    if (next == '"' || next == '\\') {
                        str.push_back(next);
                        position += 2;
                        this->col += 2;
                        continue;
                    } else if (next == 'n') {
                        str.push_back('\n');
                        position += 2;
                        this->col += 2;
                        continue;
                    } else if (next == 't') {
                        str.push_back('\t');
                        position += 2;
                        this->col += 2;
                        continue;
                    } else {
                        // unknown escape, just take the escaped char
                        str.push_back(next);
                        position += 2;
                        this->col += 2;
                        continue;
                    }
                }
                if (ch == '\n') {
                    // unterminated multiline string or allow it; track lines
                    position++;
                    this->line++;
                    this->col = 1;
                    str.push_back('\n');
                    continue;
                }
                if (ch == '"') {
                    position++; // skip closing quote
                    this->col++;
                    break;
                }
                str.push_back(ch);
                position++;
                this->col++;
            }
            tokens.push_back(Token{TokenType::STRING, str, this->filename, token_line, token_col});
            continue;
        }

        // identifiers and keywords (allow underscore)
        if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
            int token_line = this->line;
            int token_col = this->col;
            std::string id;
            while (position < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[position])) || source[position] == '_')) {
                id.push_back(source[position]); // keep original case
                position++;
                this->col++;
            }
            // lowercase copy for keyword matching
            std::string id_lower = id;
            std::transform(id_lower.begin(), id_lower.end(), id_lower.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            auto it = keywords.find(id_lower);
            if (it != keywords.end()) {
                // store keyword lexeme as lowercase to keep consistent
                tokens.push_back(Token{it->second, id_lower, this->filename, token_line, token_col});
            } else {
                tokens.push_back(Token{TokenType::IDENTIFIER, id, this->filename, token_line, token_col});
            }
            continue;
        }

        // unknown single character -> emit UNKNOWN
        {
            int token_line = this->line;
            int token_col = this->col;
            std::string unk(1, current);
            tokens.push_back(Token{TokenType::UNKNOWN, unk, this->filename, token_line, token_col});
            position++;
            this->col++;
            continue;
        }
    }

    // EOF token (record current position)
    tokens.push_back(Token{TokenType::EOF_TOKEN, "", this->filename, this->line, this->col});
    return tokens;
}