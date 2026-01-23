#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include "Frame.hpp"
#include "SwaziError.hpp"
#include "colors.hpp"
#include "evaluator.hpp"
#include "lexer.hpp"
#include "linenoise.h"
#include "parser.hpp"

void Evaluator::enter_debugger(CallFramePtr frame, EnvPtr env_fallback, DebugEncounter* encounter) {
    // Decide env to use for evaluation
    EnvPtr eval_env = nullptr;
    if (frame && frame->env)
        eval_env = frame->env;
    else
        eval_env = env_fallback ? env_fallback : main_module_env ? main_module_env
                                                                 : global_env;

    bool use_color = Color::supports_color();

    if (encounter) {
        std::string ordinal;
        size_t count = encounter->count;

        // Generate ordinal suffix (1st, 2nd, 3rd, 4th, etc.)
        if (count % 100 >= 11 && count % 100 <= 13) {
            ordinal = std::to_string(count) + "th";
        } else {
            switch (count % 10) {
                case 1:
                    ordinal = std::to_string(count) + "st";
                    break;
                case 2:
                    ordinal = std::to_string(count) + "nd";
                    break;
                case 3:
                    ordinal = std::to_string(count) + "rd";
                    break;
                default:
                    ordinal = std::to_string(count) + "th";
                    break;
            }
        }

        if (use_color) {
            std::cout << Color::bright_cyan << "Debugger paused (" << ordinal << " encounter)" << Color::reset << "\n";
        } else {
            std::cout << "Debugger paused (" << ordinal << " encounter)\n";
        }

        // Display location information
        const TokenLocation& loc = encounter->location;
        if (!loc.filename.empty()) {
            if (use_color) {
                std::cout << Color::bright_black << "  at " << Color::cyan << loc.filename
                          << Color::bright_black << ":" << Color::yellow << loc.line
                          << Color::bright_black << ":" << Color::yellow << loc.col << Color::reset << "\n";
            } else {
                std::cout << "  at " << loc.filename << ":" << loc.line << ":" << loc.col << "\n";
            }

            // Show source context if available
            // std::cout << "\n" << loc.get_line_trace() << "\n";
        }
    } else {
        if (use_color) {
            std::cout << Color::bright_cyan << "Debugger paused" << Color::reset << "\n";
        } else {
            std::cout << "Debugger paused\n";
        }
    }

    if (use_color) {
        std::cout << Color::bright_black << "Type 'help' for commands, 'c' to continue" << Color::reset << "\n";
        std::cout << Color::bright_black << "__________________________________________" << Color::reset << "\n\n";
    } else {
        std::cout << "Type 'help' for commands, 'c' to continue\n";
        std::cout << "__________________________________________\n\n";
    }

    // Setup history for debugger session
    linenoiseHistorySetMaxLen(100);

    bool running = true;
    while (running) {
        std::string prompt_str = use_color ? (Color::bright_magenta + "dbg> " + Color::reset) : "dbg> ";

        char* raw = linenoise(prompt_str.c_str());
        if (!raw) {
            std::cout << "\n";  // Ctrl-D
            break;
        }
        std::string line(raw);

        // Add to history if non-empty and different from last
        if (!line.empty()) {
            linenoiseHistoryAdd(line.c_str());
        }

        linenoiseFree(raw);

        // Trim whitespace
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            if (a == std::string::npos) return std::string();
            size_t b = s.find_last_not_of(" \t\r\n");
            return s.substr(a, b - a + 1);
        };
        std::string cmd = trim(line);
        if (cmd.empty()) continue;

        // Parse command and arguments
        std::string verb = cmd;
        std::string args;
        size_t space_pos = cmd.find(' ');
        if (space_pos != std::string::npos) {
            verb = cmd.substr(0, space_pos);
            args = trim(cmd.substr(space_pos + 1));
        }

        if (verb == "c" || verb == "cont" || verb == "continue") {
            running = false;
            break;
        } else if (verb == "help" || verb == "h" || verb == "?") {
            std::cout << "Debugger commands:\n"
                      << "  c|cont|continue    - resume program execution\n"
                      << "  l|locals           - list local variables\n"
                      << "  p|print <expr>     - evaluate expression in current frame\n"
                      << "  bt|stack           - show call stack\n"
                      << "  w|where            - show current location\n"
                      << "  q|quit|abort       - abort program (throws)\n"
                      << "  help|h|?           - show this help message\n";
            continue;
        } else if (verb == "l" || verb == "locals") {
            if (!eval_env) {
                std::cout << "(no environment)\n";
            } else {
                try {
                    std::vector<std::string> names;
                    names.reserve(eval_env->values.size());
                    for (const auto& kv : eval_env->values) names.push_back(kv.first);
                    std::sort(names.begin(), names.end());

                    if (names.empty()) {
                        std::cout << "(no local variables)\n";
                    } else {
                        for (const auto& n : names) {
                            try {
                                const auto& var = eval_env->values.at(n);
                                std::cout << n << " = " << value_to_string(var.value) << "\n";
                            } catch (...) {
                                std::cout << n << " = <unprintable>\n";
                            }
                        }
                    }
                } catch (...) {
                    std::cout << "(unable to enumerate locals)\n";
                }
            }
            continue;
        } else if (verb == "bt" || verb == "stack") {
            auto stack = get_call_stack_snapshot();
            if (stack.empty()) {
                std::cout << "(call stack empty)\n";
            } else {
                std::cout << "Call stack (most recent first):\n";
                for (int i = (int)stack.size() - 1; i >= 0; --i) {
                    auto f = stack[i];
                    std::string label = f && !f->label.empty() ? f->label : "<lambda>";

                    std::cout << "  #" << i << " " << label;

                    if (f && !f->call_token.loc.filename.empty()) {
                        std::cout << " at " << f->call_token.loc.to_string();
                    }
                    std::cout << "\n";
                }
            }
            continue;
        } else if (verb == "w" || verb == "where") {
            if (encounter && !encounter->location.filename.empty()) {
                const TokenLocation& loc = encounter->location;
                std::cout << "Current location:\n"
                          << "  " << loc.filename << ":" << loc.line << ":" << loc.col << "\n";
            } else {
                std::cout << "(location information not available)\n";
            }
            continue;
        } else if (verb == "q" || verb == "quit" || verb == "abort") {
            throw SwaziError("RuntimeError", "Aborted through debugger",
                encounter ? encounter->location : TokenLocation("<debugger>", 0, 0, 0));
        } else if (verb == "p" || verb == "print") {
            if (args.empty()) {
                std::cout << "Usage: print <expression>\n";
                continue;
            }
            try {
                Lexer lx(args, "<debugger>");
                auto tokens = lx.tokenize();
                Parser p(tokens);
                auto e = p.parse_expression_public();
                Value v = evaluate_expression(e.get(), eval_env);
                std::cout << value_to_string(v) << "\n";
            } catch (const std::exception& ex) {
                std::cout << "Error: " << ex.what() << "\n";
            } catch (...) {
                std::cout << "Unknown error evaluating expression\n";
            }
            continue;
        } else {
            std::cout << "Unknown command: '" << verb << "'. Type 'help' for commands.\n";
        }
    }
}