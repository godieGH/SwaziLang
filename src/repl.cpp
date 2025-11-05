
#include "repl.hpp"

static bool is_likely_incomplete_input(const std::string& err) {
    // Treat parser expectations that mean "unterminated input" as incomplete so REPL continues.
    // Removed "Expected ':'" because the REPL now proactively handles ':'-started blocks.
    return err.find("Expected dedent") != std::string::npos ||
        err.find("Expected '}'") != std::string::npos ||
        err.find("Expected ')'") != std::string::npos ||
        err.find("Expected ']'") != std::string::npos;
}

// --- Helpers for REPL indentation and block detection ---

static int count_leading_indent_spaces(const std::string& line) {
    // Tabs are treated as 4 spaces here. Adjust if you want different behavior.
    int count = 0;
    for (char c : line) {
        if (c == ' ')
            count += 1;
        else if (c == '\t')
            count += 4;
        else
            break;
    }
    return count;
}

static bool is_comment_line(const std::string& s) {
    bool met_char = false;
    bool met_slash = false;
    bool comment = false;
    for (unsigned char c : s) {
        met_char = !std::isspace(c);
        if (met_char) {
            if (c == '#') {
                comment = true;
                break;
            }
            if (c == '/') {
                met_slash = true;
                continue;
            }
            if (met_slash && c == '/' || c == '*') {
                comment = true;
                break;
            }
        }
    }
    if (met_char && comment) {
        return true;
    }
    return false;
}
static bool is_blank_or_spaces(const std::string& s) {
    for (unsigned char c : s)
        if (!std::isspace(c)) return false;
    return true;
}

static char last_non_ws_char(const std::string& line) {
    for (int i = (int)line.size() - 1; i >= 0; --i) {
        unsigned char ch = static_cast<unsigned char>(line[i]);
        if (!std::isspace(ch)) return static_cast<char>(ch);
    }
    return '\0';
}

static bool ends_with_colon(const std::string& line) {
    return last_non_ws_char(line) == ':';
}

static bool ends_with_open_brace(const std::string& line) {
    return last_non_ws_char(line) == '{';
}

static std::string expand_tabs(const std::string& s, int tab_width = 4) {
    std::string out;
    out.reserve(s.size());
    int col = 0;
    for (unsigned char ch : s) {
        if (ch == '\t') {
            int spaces = tab_width - (col % tab_width);
            if (spaces <= 0) spaces = tab_width;
            out.append(spaces, ' ');
            col += spaces;
        } else {
            out.push_back((char)ch);
            // assumes ASCII/single-column characters; if you need full Unicode,
            // replace with wcwidth-based logic.
            col += 1;
        }
    }
    return out;
}

// Return the count of *unclosed* bracket-like tokens: {...}, (...), [...]
// This ignores characters inside single or double quotes and skips escaped chars.
static int unclosed_brackets_depth(const std::string& s) {
    int braces = 0, paren = 0, square = 0;
    bool in_single = false, in_double = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\\') {  // skip escaped char
            ++i;
            continue;
        }
        if (!in_double && c == '\'') {
            in_single = !in_single;
            continue;
        }
        if (!in_single && c == '"') {
            in_double = !in_double;
            continue;
        }
        if (in_single || in_double) continue;

        if (c == '{')
            ++braces;
        else if (c == '}')
            --braces;
        else if (c == '(')
            ++paren;
        else if (c == ')')
            --paren;
        else if (c == '[')
            ++square;
        else if (c == ']')
            --square;
    }

    int openOnly = 0;
    if (braces > 0) openOnly += braces;
    if (paren > 0) openOnly += paren;
    if (square > 0) openOnly += square;
    return openOnly;  // zero if all balanced (or more closes than opens)
}

// --- REPL: colon-block tracking + brace continuation ---
// Behavior:
//  - If a line ends with ':'    -> push indent level and continue reading (colon block).
//  - If a line ends with '{'    -> continue reading (brace block) but DO NOT push indent level.
//  - If inside colon blocks: typing a line with indent <= top-of-stack dedents and triggers parse+exec.
//  - Parser-driven incomplete input (unclosed paren, expected '}', etc.) still handled by exception check.

// Cross-platform helper to find a suitable user home directory.
// On Unix: $HOME
// On Windows: %USERPROFILE% or %HOMEDRIVE%%HOMEPATH%
// Returns empty optional if none found.
static std::optional<fs::path> get_home_dir() {
#ifdef _WIN32
    const char* userprofile = std::getenv("USERPROFILE");
    if (userprofile && userprofile[0] != '\0') return fs::path(userprofile);
    const char* homedrive = std::getenv("HOMEDRIVE");
    const char* homepath = std::getenv("HOMEPATH");
    if (homedrive && homepath) {
        std::string combined = std::string(homedrive) + std::string(homepath);
        if (!combined.empty()) return fs::path(combined);
    }
    // As a last resort, try HOMESHARE (rare)
    const char* homeshare = std::getenv("HOMESHARE");
    if (homeshare && homeshare[0] != '\0') return fs::path(homeshare);
    return std::nullopt;
#else
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') return fs::path(home);
    return std::nullopt;
#endif
}

// Build a cross-platform history file path in the user's home directory.
// Falls back to current directory "/.swazi_history" if no home is found.
static fs::path history_file_in_home() {
    auto home = get_home_dir();
    if (home.has_value()) {
        return home.value() / ".swazi_history";
    }
    // fallback: current directory
    return fs::current_path() / ".swazi_history";
}

// Important note:
// Many linenoise implementations (and terminals) don't handle multi-byte or
// wide characters gracefully when computing cursor positions for the prompt.
// The original REPL used fancy Unicode arrows which can make the cursor appear
// "unstable" or misplaced in some linnoise/terminal combinations. To avoid
// cursor misplacement we use a plain ASCII prompt here (no hidden ANSI codes,
// no multi-byte glyphs). If you prefer the fancier Unicode prompt, enable it
// only after verifying your linenoise/terminal combo correctly supports UTF-8
// prompt length calculations.
void run_repl_mode() {
    std::string buffer;
    Evaluator evaluator;
    evaluator.set_entry_point("");  // REPL: sets __name__ to "<repl>" and __main__ true
    std::vector<int> colon_indent_stack;

    std::cout << "swazi v" << SWAZI_VERSION << " | built on " << __DATE__ << "\n";
    std::cout << "Swazi REPL — type 'exit' or 'quit' or Ctrl-D to quit\n";

    // Initialize linenoise history and callbacks (best-effort)
    fs::path history_path = history_file_in_home();
    // Ensure parent dir exists (usually the home dir exists already).
    try {
        fs::path parent = history_path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            fs::create_directories(parent);
        }
    } catch (...) {
        // ignore; creating history directory is best-effort
    }

    // linenoise API expects C-string path
    linenoiseHistoryLoad(history_path.string().c_str());

    // Track the last history entry we added in this session to avoid immediate duplicates.
    // Some linenoise builds do not expose history iteration APIs (linenoiseHistoryLength/Get),
    // so we avoid calling those and maintain the value locally.
    std::string last_added_history;

// If your linenoise supports a multi-line editing mode, you can enable it here.
// We try calling it in a guarded way (some builds export it, some do not).
// Uncomment the following lines if your linenoise has linenoiseSetMultiLine:
//
#ifdef LINENOISE_MULTILINE
    linenoiseSetMultiLine(1);
#endif
    //
    // We keep it commented out to remain portable.

    while (true) {
        // Use plain ASCII prompts to avoid cursor misplacement bugs in some terminals.
        std::string prompt = buffer.empty() ? ">>> " : "... ";
        char* raw = linenoise(prompt.c_str());
        if (!raw) {  // EOF (Ctrl-D) or error
            std::cout << "\n";
            break;
        }

        std::string line(raw);
        linenoiseFree(raw);

        line = expand_tabs(line, 4);

        if (line == "exit" || line == "quit") break;

        // Save non-empty lines to history (avoid immediate duplicates)
        if (!line.empty()) {
            if (line != last_added_history) {
                linenoiseHistoryAdd(line.c_str());
                last_added_history = line;
            }
        }

        // Append line to buffer and continue with your continuation logic
        buffer += line;
        buffer.push_back('\n');

        // ---- continuation checks (unchanged) ----
        if (ends_with_colon(line)) {
            int base_indent = count_leading_indent_spaces(line);
            colon_indent_stack.push_back(base_indent);
            continue;  // keep reading for colon-blocks (pythonic)
        }

        if (ends_with_open_brace(line)) {
            continue;
        }

        if (unclosed_brackets_depth(buffer) > 0) {
            continue;
        }

        if (!colon_indent_stack.empty()) {
            if (is_comment_line(line)) continue;
            int leading = count_leading_indent_spaces(line);
            if (leading > 0 && is_blank_or_spaces(line)) continue;
            if (leading > colon_indent_stack.back()) {
                continue;
            } else if (!colon_indent_stack.empty() && leading == colon_indent_stack.back() && colon_indent_stack.back() != 0) {
                colon_indent_stack.pop_back();
                continue;
            } else {
                while (!colon_indent_stack.empty() && leading < colon_indent_stack.back()) {
                    // if(leading colon_indent_stack.back())
                    colon_indent_stack.pop_back();
                }

                // fall through -> attempt parse+evaluate
            }
        }

        // ---- Try lex/parse/evaluate the accumulated buffer ----
        try {
            if (buffer.empty() || buffer.back() != '\n') buffer.push_back('\n');

            Lexer lexer(buffer, "<repl>");
            std::vector<Token> tokens = lexer.tokenize();

            Parser parser(tokens);
            std::unique_ptr<ProgramNode> ast = parser.parse();

            bool did_auto_print = false;
            if (ast->body.size() == 1) {
                if (auto exprStmt = dynamic_cast<ExpressionStatementNode*>(ast->body[0].get())) {
                    Value v = evaluator.evaluate_expression(exprStmt->expression.get());
                    if (!evaluator.is_void(v)) {
                        std::cout << evaluator.value_to_string(v) << "\n";
                    }
                    did_auto_print = true;
                }
            }

            if (!did_auto_print) evaluator.evaluate(ast.get());

            // success: clear buffer and colon-indent stack
            buffer.clear();
            colon_indent_stack.clear();
        } catch (const std::exception& e) {
            std::string msg = e.what();
            if (is_likely_incomplete_input(msg)) {
                // incomplete -> keep buffer and continue reading
                continue;
            }
            std::cerr << "Error: " << msg << std::endl;
            buffer.clear();
            colon_indent_stack.clear();
        } catch (...) {
            std::cerr << "Unknown fatal error\n";
            buffer.clear();
            colon_indent_stack.clear();
        }
    }

    // Save history on exit
    linenoiseHistorySave(history_path.string().c_str());
}
