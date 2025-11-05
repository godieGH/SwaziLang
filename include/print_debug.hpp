#pragma once
#include <vector>

#include "ast.hpp"
#include "token.hpp"

void print_tokens(const std::vector<Token>& tokens);

void print_program_debug(ProgramNode* ast, int indent = 0);