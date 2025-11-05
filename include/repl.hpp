#pragma once
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "linenoise.h"
#include "parser.hpp"
namespace fs = std::filesystem;

void run_repl_mode();