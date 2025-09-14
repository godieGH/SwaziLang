#include "print_tokens.hpp"
#include <iostream>
#include <unordered_map>
#include <string>

static std::string token_name(TokenType t) {
    static const std::unordered_map<TokenType, std::string> names = {
        {TokenType::DATA,"DATA"}, {TokenType::CHAPISHA,"CHAPISHA"}, {TokenType::ANDIKA,"ANDIKA"},
        {TokenType::CONSTANT,"CONSTANT"}, {TokenType::KAZI,"KAZI"}, {TokenType::RUDISHA,"RUDISHA"},
        {TokenType::IDENTIFIER,"IDENTIFIER"}, {TokenType::NUMBER,"NUMBER"}, {TokenType::STRING,"STRING"},
        {TokenType::BOOLEAN,"BOOLEAN"}, {TokenType::SEMICOLON,"SEMICOLON"}, {TokenType::COMMA,"COMMA"},
        {TokenType::OPENPARENTHESIS,"OPENPARENTHESIS"}, {TokenType::CLOSEPARENTHESIS,"CLOSEPARENTHESIS"},
        {TokenType::OPENBRACE,"OPENBRACE"}, {TokenType::CLOSEBRACE,"CLOSEBRACE"},
        {TokenType::COLON,"COLON"}, {TokenType::ASSIGN,"ASSIGN"}, {TokenType::EOF_TOKEN,"EOF_TOKEN"},
        {TokenType::PLUS,"PLUS"}, {TokenType::MINUS,"MINUS"}, {TokenType::STAR,"STAR"}, {TokenType::SLASH,"SLASH"},
        {TokenType::PERCENT,"PERCENT"}, {TokenType::POWER,"POWER"}, {TokenType::PLUS_ASSIGN,"PLUS_ASSIGN"},
        {TokenType::MINUS_ASSIGN,"MINUS_ASSIGN"}, {TokenType::INCREMENT,"INCREMENT"}, {TokenType::DECREMENT,"DECREMENT"},
        {TokenType::AND,"AND"}, {TokenType::OR,"OR"}, {TokenType::NOT,"NOT"},
        {TokenType::GREATERTHAN,"GREATERTHAN"}, {TokenType::GREATEROREQUALTHAN,"GREATEROREQUALTHAN"},
        {TokenType::LESSTHAN,"LESSTHAN"}, {TokenType::LESSOREQUALTHAN,"LESSOREQUALTHAN"},
        {TokenType::EQUALITY,"EQUALITY"}, {TokenType::NOTEQUAL,"NOTEQUAL"},
        {TokenType::NEWLINE,"NEWLINE"}, {TokenType::INDENT,"INDENT"}, {TokenType::DEDENT,"DEDENT"},
        {TokenType::COMMENT,"COMMENT"}, {TokenType::UNKNOWN,"UNKNOWN"}
    };
    auto it = names.find(t);
    if (it != names.end()) return it->second;
    return "TOKEN(?)";
}

void print_tokens(const std::vector<Token>& tokens) {
    for (const auto &tok : tokens) {
        std::cout
            << "type=" << static_cast<int>(tok.type)
            << " value=\"" << tok.value << "\""
            << " loc=" << tok.loc.to_string()
            << " len=" << tok.loc.length
            << "\n";
    }
}