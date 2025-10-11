#pragma once
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <optional>
#include <cstdlib>

#include "lexer.hpp"
#include "parser.hpp"
#include "evaluator.hpp"

#include <filesystem>
#include "linenoise.h"
namespace fs = std::filesystem;


void run_repl_mode();