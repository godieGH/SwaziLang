#include "print_debug.hpp"
#include <iostream>
#include <unordered_map>
#include <string>
#include <iomanip>

static std::string token_name(TokenType t) {
    static const std::unordered_map<TokenType, std::string> names = {
        {TokenType::DATA,"DATA"}, {TokenType::CHAPISHA,"CHAPISHA"}, {TokenType::ANDIKA,"ANDIKA"},
        {TokenType::CONSTANT,"CONSTANT"}, {TokenType::KAZI,"KAZI"}, {TokenType::RUDISHA,"RUDISHA"},
        {TokenType::IDENTIFIER,"IDENTIFIER"}, {TokenType::NUMBER,"NUMBER"}, {TokenType::STRING,"STRING"},
        {TokenType::BOOLEAN,"BOOLEAN"}, {TokenType::SEMICOLON,"SEMICOLON"}, {TokenType::COMMA,"COMMA"},
        {TokenType::OPENPARENTHESIS,"OPENPARENTHESIS"}, {TokenType::CLOSEPARENTHESIS,"CLOSEPARENTHESIS"},
        {TokenType::OPENBRACE,"OPENBRACE"}, {TokenType::CLOSEBRACE,"CLOSEBRACE"},
        {TokenType::OPENBRACKET,"OPENBRACKET"}, {TokenType::CLOSEBRACKET,"CLOSEBRACKET"},
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
    return it != names.end() ? it->second : "TOKEN(?)";
}

static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void print_tokens(const std::vector<Token>& tokens) {
    std::cout << "[\n";
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto& tok = tokens[i];
        std::cout << "  {\n";
        std::cout << "    \"type\": \"" << token_name(tok.type) << "\",\n";
        std::cout << "    \"value\": \"" << escape(tok.value) << "\",\n";
        std::cout << "    \"loc\": \"" << tok.loc.to_string() << "\",\n";
        std::cout << "    \"length\": " << tok.loc.length << "\n";
        std::cout << "  }" << (i + 1 < tokens.size() ? "," : "") << "\n";
    }
    std::cout << "]\n";
}

void print_program_debug(ProgramNode* ast, int indent) {
    if (!ast) {
        std::cout << "{}\n";
        return;
    }

    std::string ind(indent, ' ');
    std::cout << ind << "{\n";
    std::cout << ind << "  \"type\": \"Program\",\n";
    std::cout << ind << "  \"body\": [\n";

    for (size_t i = 0; i < ast->body.size(); ++i) {
        auto* stmt = ast->body[i].get();
        if (!stmt) {
            std::cout << ind << "    null";
        } else {
            std::cout << ind << "    {\n";
            std::cout << ind << "      \"nodeType\": \"" << typeid(*stmt).name() << "\",\n";
            std::cout << ind << "      \"token\": \"" << escape(stmt->token.loc.to_string()) << "\"\n";
            std::cout << ind << "    }";
        }
        if (i + 1 < ast->body.size()) std::cout << ",";
        std::cout << "\n";
    }

    std::cout << ind << "  ]\n";
    std::cout << ind << "}\n";
}
