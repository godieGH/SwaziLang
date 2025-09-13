#include "lexer.hpp"
#include <map>
#include <cctype>

Lexer::Lexer(std::string source) : source(source) {}


std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;

    std::map<std::string, TokenType> keywords = {
        {"data", TokenType::DATA},
        {"thabiti", TokenType::CONSTANT},
        {"chapisha", TokenType::CHAPISHA},
        {"andika", TokenType::ANDIKA},
        {"kweli", TokenType::BOOLEAN},
        {"sikweli", TokenType::BOOLEAN},
        // word-operators
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

        // skip whitespace
        if (std::isspace(static_cast<unsigned char>(current))) {
            position++;
            continue;
        }

        // comments
        if (current == '/' && peek_char() == '/') {
            // single-line comment
            position += 2;
            while (position < source.size() && source[position] != '\n') position++;
            continue;
        }
        if (current == '/' && peek_char() == '*') {
            // block comment
            position += 2;
            while (position + 1 < source.size() && !(source[position] == '*' && peek_char() == '/')) {
                position++;
            }
            if (position + 1 < source.size()) position += 2;
            continue;
        }

        // two-char operators: check first
        if (current == '*' && peek_char() == '*') {
            tokens.push_back({TokenType::POWER, "**"});
            position += 2;
            continue;
        }
        if (current == '>' && peek_char() == '=') {
            tokens.push_back({TokenType::GREATEROREQUALTHAN, ">="});
            position += 2;
            continue;
        }
        if (current == '<' && peek_char() == '=') {
            tokens.push_back({TokenType::LESSOREQUALTHAN, "<="});
            position += 2;
            continue;
        }
        if (current == '=' && peek_char() == '=') {
            tokens.push_back({TokenType::EQUALITY, "=="});
            position += 2;
            continue;
        }
        if (current == '!' && peek_char() == '=') {
            tokens.push_back({TokenType::NOTEQUAL, "!="});
            position += 2;
            continue;
        }
        if (current == '&' && peek_char() == '&') {
            tokens.push_back({TokenType::AND, "&&"});
            position += 2;
            continue;
        }
        if (current == '|' && peek_char() == '|') {
            tokens.push_back({TokenType::OR, "||"});
            position += 2;
            continue;
        }

        // single-char tokens
        switch (current) {
            case '+':
                tokens.push_back({TokenType::PLUS, "+"});
                position++;
                continue;
            case '-':
                tokens.push_back({TokenType::MINUS, "-"});
                position++;
                continue;
            case '*':
                tokens.push_back({TokenType::STAR, "*"});
                position++;
                continue;
            case '/':
                tokens.push_back({TokenType::SLASH, "/"});
                position++;
                continue;
            case '%':
                tokens.push_back({TokenType::PERCENT, "%"});
                position++;
                continue;
            case '=':
                tokens.push_back({TokenType::ASSIGN, "="});
                position++;
                continue;
            case '!':
                tokens.push_back({TokenType::NOT, "!"});
                position++;
                continue;
            case '>':
                tokens.push_back({TokenType::GREATERTHAN, ">"});
                position++;
                continue;
            case '<':
                tokens.push_back({TokenType::LESSTHAN, "<"});
                position++;
                continue;
            case '(':
                tokens.push_back({TokenType::OPENPARENTHESIS, "("});
                position++;
                continue;
            case ')':
                tokens.push_back({TokenType::CLOSEPARENTHESIS, ")"});
                position++;
                continue;
            case ',':
                tokens.push_back({TokenType::COMMA, ","});
                position++;
                continue;
            case ';':
                tokens.push_back({TokenType::SEMICOLON, ";"});
                position++;
                continue;
        }

        // numbers (integer or decimal)
        if (std::isdigit(static_cast<unsigned char>(current))) {
            std::string num;
            bool seen_dot = false;
            while (position < source.size() &&
                   (std::isdigit(static_cast<unsigned char>(source[position])) ||
                    (!seen_dot && source[position] == '.'))) {
                if (source[position] == '.') seen_dot = true;
                num.push_back(source[position]);
                position++;
            }
            tokens.push_back({TokenType::NUMBER, num});
            continue;
        }

        // strings with basic escapes
        if (current == '"') {
            position++; // skip opening quote
            std::string str;
            while (position < source.size()) {
                char ch = source[position];
                if (ch == '\\' && position + 1 < source.size()) {
                    char next = source[position + 1];
                    if (next == '"' || next == '\\') {
                        str.push_back(next);
                        position += 2;
                        continue;
                    } else if (next == 'n') {
                        str.push_back('\n');
                        position += 2;
                        continue;
                    } else if (next == 't') {
                        str.push_back('\t');
                        position += 2;
                        continue;
                    }
                }
                if (ch == '"') {
                    position++; // skip closing quote
                    break;
                }
                str.push_back(ch);
                position++;
            }
            tokens.push_back({TokenType::STRING, str});
            continue;
        }

        // identifiers and keywords (allow underscore)
        if (std::isalpha(static_cast<unsigned char>(current)) || current == '_') {
            std::string id;
            while (position < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[position])) || source[position] == '_')) {
                id.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(source[position]))));
                position++;
            }
            auto it = keywords.find(id);
            if (it != keywords.end()) {
                tokens.push_back({it->second, id});
            } else {
                // keep original case? we lowercased for keyword matching; store identifier as-is
                tokens.push_back({TokenType::IDENTIFIER, id});
            }
            continue;
        }

        // unknown single character -> emit UNKNOWN
        tokens.push_back({TokenType::UNKNOWN, std::string(1, current)});
        position++;
    }

    tokens.push_back({TokenType::EOF_TOKEN, ""});
    return tokens;
}