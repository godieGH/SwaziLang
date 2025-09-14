// small debug snippet to paste into your REPL main after parsing
for (auto &stmt : ast->body) {
    if (auto f = dynamic_cast<FunctionDeclarationNode*>(stmt.get())) {
        std::cerr << "DEBUG: parsed function '" << f->name << "' with "
                  << f->parameters.size() << " params\n";
    } else {
        std::cerr << "DEBUG: parsed statement of type (not function)\n";
    }
}