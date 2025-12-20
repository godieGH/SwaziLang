#pragma once
#include <filesystem>
#include <fstream>
#include <sstream>

#include "SourceManager.hpp"
#include "ast.hpp"
#include "cli_commands.hpp"
#include "lexer.hpp"
#include "token.hpp"

namespace fs = std::filesystem;

struct Formatter {
    struct Flags {
        bool recursive;
        bool print;
        Flags(bool r, bool p) : recursive(r), print(p) {};
    };

    std::string filename;
    std::string destination;
    Flags flags;

    Formatter(std::vector<std::string> args, Flags flags);

   private:
    std::string get_source_code();

    std::string format_from_ast(ProgramNode* program);
};

std::string format_statement(StatementNode* stmt, int depth = 0);
std::string format_expression(ExpressionNode* expr);