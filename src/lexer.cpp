#include "lexer.hpp"
#include <map>
#include <cctype>

Lexer::Lexer(std::string source) : source(source) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    std::map<std::string, TokenType> keywords = {
        {"data", TokenType::DATA},
        {"chapisha", TokenType::CHAPISHA},
        {"kweli", TokenType::BOOLEAN},
        {"sikweli", TokenType::BOOLEAN}
    };

    while (position < source.length()) {
        char current_char = source[position];

        if (isspace(current_char)) {
            position++;
            continue;
        }

        if (current_char == '=') {
            tokens.push_back({TokenType::SAWA, "="});
            position++;
            continue;
        }

        if (current_char == ';') {
            tokens.push_back({TokenType::SEMICOLON, ";"});
            position++;
            continue;
        }

        if (isdigit(current_char)) {
            std::string number_str;
            while (position < source.length() && isdigit(source[position])) {
                number_str += source[position];
                position++;
            }
            tokens.push_back({TokenType::NAMBA, number_str});
            continue;
        }
        
        if (current_char == '"') {
            std::string str_val;
            position++; // Skip the opening quote
            while (position < source.length() && source[position] != '"') {
                str_val += source[position];
                position++;
            }
            position++; // Skip the closing quote
            tokens.push_back({TokenType::NENO, str_val});
            continue;
        }

        if (isalpha(current_char)) {
            std::string identifier;
            while (position < source.length() && isalnum(source[position])) {
                identifier += source[position];
                position++;
            }

            if (keywords.count(identifier)) {
                tokens.push_back({keywords[identifier], identifier});
            } else {
                tokens.push_back({TokenType::IDENTIFIER, identifier});
            }
            continue;
        }

        tokens.push_back({TokenType::UNKNOWN, std::string(1, current_char)});
        position++;
    }

    tokens.push_back({TokenType::EOF_TOKEN, ""});
    return tokens;
}
