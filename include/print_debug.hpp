#pragma once
#include <vector>
#include "token.hpp"
#include "ast.hpp"

void print_tokens(const std::vector<Token>& tokens);

void print_program_debug(ProgramNode* ast, int indent = 0);