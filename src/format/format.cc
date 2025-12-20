#include "format/format.hpp"

#include "SourceManager.hpp"
#include "ast.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "token.hpp"

Formatter::Formatter(std::vector<std::string> args, Flags flags) : flags(flags) {
    filename = args[0];
    if (args.size() >= 2) {
        destination = args[1];
    }

    std::string source_code = get_source_code();

    SourceManager src_mgr(filename, source_code);

    Lexer lexer(source_code, filename, &src_mgr);
    std::vector<Token> tokens = lexer.tokenize();

    Parser parser(tokens);
    std::unique_ptr<ProgramNode> ast = parser.parse();

    std::cout << format_from_ast(ast.get()) << std::endl;
}
std::string Formatter::get_source_code() {
    fs::path file(filename);

    if (!fs::exists(file)) {
        throw std::runtime_error("File not found: `" + filename + "`");
    }

    if (!fs::is_regular_file(file)) {
        throw std::runtime_error("Not a regular file: `" + filename + "`");
    }

    std::ifstream in(file, std::ios::in | std::ios::binary);
    if (!in) {
        throw std::runtime_error("Failed to open file: `" + filename + "`");
    }

    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(in.tellg());
    in.seekg(0, std::ios::beg);
    in.read(&contents[0], contents.size());

    return contents;
}

std::string Formatter::format_from_ast(ProgramNode* program) {
    std::ostringstream ss;

    for (auto& stmt_uptr : program->body) {
        ss << format_statement(stmt_uptr.get(), 0) << "\n";
    }

    return ss.str();
}