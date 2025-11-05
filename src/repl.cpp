#include "repl.hpp"

// Improved REPL indentation/block handling
// - More robust comment detection
// - Explicit "waiting for body indent" semantics for ':' blocks (Python-like)
// - Proper nested dedent handling: pop frames only when the user actually dedents
// - Safety: limit maximum nesting depth to avoid pathological input DoS
// - Clear and conservative behavior: do not silently pop a colon-frame that hasn't received a body
//   (this avoids creating implicit empty blocks). Let the parser detect real syntax errors.

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
    // Find first non-space character and decide if it's a comment starter.
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i == s.size()) return false;
    if (s[i] == '#') return true;
    if (s[i] == '/') {
        if (i + 1 < s.size()) {
            if (s[i + 1] == '/' || s[i + 1] == '*') return true;
        }
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
//
// Behavior implemented here aims to follow Python's interactive semantics for ':' blocks:
//
// Frames (IndentFrame) track:
//  - base_indent: indentation of the line that ended with ':'
//  - body_indent: indentation used by the first non-blank non-comment line after the ':' (unknown initially)
//  - waiting_for_body: true until a non-blank non-comment line establishes the block body indent
//
// Rules:
//  - A new colon line pushes a frame with waiting_for_body = true.
//  - While waiting_for_body:
//      - ignore blank lines and comments
//      - the first real line with indent > base_indent establishes body_indent and waiting_for_body = false
//      - a non-indented real line (indent <= base_indent) will be allowed to fall through to parsing
//        (the parser will detect missing block/body if that's invalid). We intentionally do not
//        implicitly pop the waiting frame to avoid creating an implicit empty block that hides errors.
//  - While not waiting_for_body:
//      - A line with indent >= current frame.body_indent is considered inside the current block (cont.)
 //    - A line with indent < current frame.body_indent dedents out: pop frames until the
//        indent is within the enclosing frame (or no frames remain).
//
// The implementation also enforces a maximum nesting depth to prevent pathological input from
// exhausting memory/CPU (simple DoS protection).

struct IndentFrame {
    int base_indent;          // indent of the line that ended with ':'
    int body_indent;          // indent of first body line (set after first real body line)
    bool waiting_for_body;    // true until body_indent is set
};

static const size_t MAX_REPL_INDENT_NESTING = 1024;

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
    const char* homeshare = std::getenv("HOMESHARE");
    if (homeshare && homeshare[0] != '\0') return fs::path(homeshare);
    return std::nullopt;
#else
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') return fs::path(home);
    return std::nullopt;
#endif
}

static fs::path history_file_in_home() {
    auto home = get_home_dir();
    if (home.has_value()) {
        return home.value() / ".swazi_history";
    }
    return fs::current_path() / ".swazi_history";
}

void run_repl_mode() {
    std::string buffer;
    Evaluator evaluator;
    evaluator.set_entry_point("");  // REPL: sets __name__ to "<repl>" and __main__ true
    std::vector<IndentFrame> indent_stack;

    std::cout << "swazi v" << SWAZI_VERSION << " | built on " << __DATE__ << "\n";
    std::cout << "Swazi REPL — type 'exit' or 'quit' or Ctrl-D to quit\n";

    fs::path history_path = history_file_in_home();
    try {
        fs::path parent = history_path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            fs::create_directories(parent);
        }
    } catch (...) {
        // ignore; creating history directory is best-effort
    }

    linenoiseHistoryLoad(history_path.string().c_str());

    std::string last_added_history;

#ifdef LINENOISE_MULTILINE
    linenoiseSetMultiLine(1);
#endif

    while (true) {
        std::string prompt = buffer.empty() ? ">>> " : "... ";
        char* raw = linenoise(prompt.c_str());
        if (!raw) {  // EOF (Ctrl-D) or error
            std::cout << "\n";
            break;
        }

        std::string line(raw);
        linenoiseFree(raw);

        // Normalise tabs early so all indent calculations are consistent.
        line = expand_tabs(line, 4);

        if (line == "exit" || line == "quit") break;

        if (!line.empty()) {
            if (line != last_added_history) {
                linenoiseHistoryAdd(line.c_str());
                last_added_history = line;
            }
        }

        // Append line to buffer; we'll attempt parse when continuation conditions are not met.
        buffer += line;
        buffer.push_back('\n');

        // ---- continuation checks ----

        // If the line ends with ':' we begin a waiting colon frame (like Python).
        if (ends_with_colon(line)) {
            if (indent_stack.size() >= MAX_REPL_INDENT_NESTING) {
                std::cerr << "Error: too many nested blocks (limit " << MAX_REPL_INDENT_NESTING << ")\n";
                // Clear buffer and stack to recover to a clean state.
                buffer.clear();
                indent_stack.clear();
                continue;
            }
            int base_indent = count_leading_indent_spaces(line);
            IndentFrame frame;
            frame.base_indent = base_indent;
            frame.body_indent = -1;
            frame.waiting_for_body = true;
            indent_stack.push_back(frame);
            continue;  // definitely continue reading for the block body
        }

        // If the line ends with '{' (brace-block), keep reading until matching '}'.
        if (ends_with_open_brace(line)) {
            continue;
        }

        // If there are unclosed bracket tokens, continue reading (user likely typed a multiline list/expr).
        if (unclosed_brackets_depth(buffer) > 0) {
            continue;
        }

        // If we're inside colon-driven blocks, decide whether to continue or dedent/pop frames.
        if (!indent_stack.empty()) {
            // ignore comments/blank lines for continuation/dedent decisions in many cases
            if (is_comment_line(line)) {
                // Do not treat a pure-comment line as ending a waiting-for-body frame.
                // Example:
                // if cond:
                //    # comment
                //    <body>
                // So we continue reading without modifying frames.
                continue;
            }

            int leading = count_leading_indent_spaces(line);

            // If this line has only spaces (and some indent), keep waiting for a real body line.
            if (leading > 0 && is_blank_or_spaces(line)) {
                continue;
            }

            // Now examine the topmost frame and decide behavior.
            // We will pop frames when the current leading indent dedents out of them.
            // BUT if the top frame is waiting_for_body and the current line has indent > base_indent,
            // that establishes the frame's body_indent (we stay in block).
            IndentFrame &top = indent_stack.back();

            if (top.waiting_for_body) {
                // If the user provides a real body line with indent > base_indent, set the body's indent.
                if (leading > top.base_indent) {
                    top.body_indent = leading;
                    top.waiting_for_body = false;
                    // We're inside the body now, so continue reading (more block content may follow).
                    continue;
                }
                // If leading <= base_indent then user did not provide a body; allow parse attempt.
                // We intentionally do NOT pop the waiting frame here — let the parser diagnose missing block
                // (so we don't mask real syntax errors). Fall through to parsing attempt.
            } else {
                // Not waiting_for_body: determine if this line is inside this frame or dedents out.
                if (leading >= top.body_indent) {
                    // still inside the current block (includes same indentation level for sibling statements)
                    continue;
                } else {
                    // Dedent: pop frames while the leading indent is less than the effective indent of the frame.
                    while (!indent_stack.empty()) {
                        IndentFrame &cur = indent_stack.back();
                        int effective = cur.waiting_for_body ? cur.base_indent + 1 : cur.body_indent;
                        // For waiting frames, treat effective as base_indent+1 so that a dedent to base_indent
                        // is considered outside; but do not pop waiting frames blindly if leading is <= base_indent.
                        if (cur.waiting_for_body) {
                            if (leading <= cur.base_indent) {
                                // We dedented out of a waiting frame without establishing a body:
                                // popping it is safe here because the user explicitly dedented outside of it.
                                indent_stack.pop_back();
                                continue;
                            } else {
                                // leading > base_indent: this should have been captured earlier (waiting->body)
                                // but to be safe, set body_indent and stay in block.
                                cur.body_indent = leading;
                                cur.waiting_for_body = false;
                                break;
                            }
                        } else {
                            if (leading < cur.body_indent) {
                                indent_stack.pop_back();
                                continue;
                            } else {
                                // leading >= cur.body_indent => we are inside this frame
                                break;
                            }
                        }
                    } // end while
                    // After popping, we fall through to attempt parse+evaluate if no conditions require further continuation.
                }
            }
        } // end indent_stack handling

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

            // success: clear buffer and indentation stack
            buffer.clear();
            indent_stack.clear();
        } catch (const std::exception& e) {
            std::string msg = e.what();
            if (is_likely_incomplete_input(msg)) {
                // incomplete -> keep buffer and continue reading
                continue;
            }
            std::cerr << "Error: " << msg << std::endl;
            buffer.clear();
            indent_stack.clear();
        } catch (...) {
            std::cerr << "Unknown fatal error\n";
            buffer.clear();
            indent_stack.clear();
        }
    }

    linenoiseHistorySave(history_path.string().c_str());
}