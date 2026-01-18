#include <cctype>
#include <csignal>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

#include "SwaziError.hpp"
#include "debugging/outputTry.hpp"
#include "parser.hpp"

std::unique_ptr<StatementNode> Parser::parse_sequential_declarations(bool outer_is_constant) {
    Token openTok = consume();  // consume '('

    auto skip_formatting = [&]() {
        while (peek().type == TokenType::NEWLINE ||
            peek().type == TokenType::INDENT ||
            peek().type == TokenType::DEDENT) {
            consume();
        }
    };

    skip_formatting();

    if (peek().type == TokenType::CLOSEPARENTHESIS) {
        Token tok = peek();
        throw std::runtime_error("Parse error at " + tok.loc.to_string() +
            ": Empty declaration list not allowed" +
            "\n--> Traced at:\n" + tok.loc.get_line_trace());
    }

    std::vector<std::unique_ptr<VariableDeclarationNode>> declarations;

    while (peek().type != TokenType::CLOSEPARENTHESIS && peek().type != TokenType::EOF_TOKEN) {
        skip_formatting();

        // Check for per-item constant modifier
        bool item_is_constant = outer_is_constant;

        if (peek().type == TokenType::CONSTANT || peek().type == TokenType::AMPERSAND) {
            consume();
            item_is_constant = true;
        }

        // Parse declaration target
        std::unique_ptr<ExpressionNode> pattern = nullptr;
        std::string name;
        Token idTok;

        if (peek().type == TokenType::IDENTIFIER) {
            consume();
            idTok = tokens[position - 1];
            name = idTok.value;
        } else if (peek().type == TokenType::OPENBRACKET || peek().type == TokenType::OPENBRACE) {
            pattern = parse_pattern();
            idTok = pattern->token;
        } else {
            Token tok = peek();
            throw std::runtime_error("Parse error at " + tok.loc.to_string() +
                ": Expected identifier or pattern in declaration list" +
                "\n--> Traced at:\n" + tok.loc.get_line_trace());
        }

        skip_formatting();

        // Optional initializer
        std::unique_ptr<ExpressionNode> value = nullptr;

        if (peek().type == TokenType::ASSIGN) {
            consume();
            skip_formatting();
            value = parse_expression();
        }

        // Validate: constants must be initialized
        if (item_is_constant && !value) {
            throw std::runtime_error("Parse error at " + idTok.loc.to_string() +
                ": Constant '" + (pattern ? "<pattern>" : name) +
                "' must be initialized at declaration" +
                "\n--> Traced at:\n" + idTok.loc.get_line_trace());
        }

        // Create VariableDeclarationNode
        auto declNode = std::make_unique<VariableDeclarationNode>();
        if (pattern) {
            declNode->pattern = std::move(pattern);
            declNode->identifier = "";
        } else {
            declNode->identifier = name;
            declNode->pattern = nullptr;
        }
        declNode->value = std::move(value);
        declNode->is_constant = item_is_constant;
        declNode->token = idTok;

        declarations.push_back(std::move(declNode));

        skip_formatting();

        // Separator handling
        if (peek().type == TokenType::COMMA) {
            consume();
            skip_formatting();
            if (peek().type == TokenType::CLOSEPARENTHESIS) break;
            continue;
        }

        if (peek().type == TokenType::CLOSEPARENTHESIS) break;

        Token tok = peek();
        throw std::runtime_error("Parse error at " + tok.loc.to_string() +
            ": Expected ',' or ')' in declaration list" +
            "\n--> Traced at:\n" + tok.loc.get_line_trace());
    }

    skip_formatting();
    expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after declaration list");

    if (peek().type == TokenType::SEMICOLON) consume();

    // CHANGE: Return SequentialDeclarationNode instead of DoStatementNode
    auto seqDecl = std::make_unique<SequentialDeclarationNode>();
    seqDecl->token = openTok;
    seqDecl->declarations = std::move(declarations);

    return seqDecl;
}

std::unique_ptr<StatementNode> Parser::parse_variable_declaration() {
    bool is_constant = false;

    if (peek().type == TokenType::CONSTANT || peek().type == TokenType::AMPERSAND) {
        consume();
        is_constant = true;
    }

    if (peek().type == TokenType::OPENPARENTHESIS) {
        return parse_sequential_declarations(is_constant);
    }

    // After 'data' we allow either:
    //   IDENTIFIER                    -> simple var
    //   OPENBRACKET '[' ... ']'       -> array destructuring
    //   OPENBRACE   '{' ... '}'       -> object destructuring
    std::unique_ptr<ExpressionNode> pattern = nullptr;
    std::string name;
    Token idTok;

    if (peek().type == TokenType::IDENTIFIER) {
        consume();
        idTok = tokens[position - 1];
        name = idTok.value;
    } else if (peek().type == TokenType::OPENBRACKET || peek().type == TokenType::OPENBRACE) {
        // parse a pattern and use its token as the declaration token
        pattern = parse_pattern();
        // choose token for error reporting
        idTok = pattern->token;
    } else {
        Token tok = peek();
        throw std::runtime_error("Parse error at " + tok.loc.to_string() + ": Expected identifier or destructuring pattern after 'data'" + "\n--> Traced at:\n" + tok.loc.get_line_trace());
    }

    std::unique_ptr<ExpressionNode> value = nullptr;

    if (is_constant) {
        // force an initializer for constants
        expect(TokenType::ASSIGN, "Constant '" + (pattern ? "<pattern>" : name) + "' must be initialized at declaration");
        value = parse_expression();
    } else {
        // non-constants: assignment is optional
        if (peek().type == TokenType::ASSIGN) {
            consume();
            value = parse_expression();
        } else {
            // experimental but worth it.
            if (peek().type != TokenType::SEMICOLON && peek().type != TokenType::NEWLINE) {
                throw std::runtime_error("Terminate variable declaration with a \";\" or with a newline if not initialized at" + peek().loc.to_string() + "\n--> Traced at, bad code practice:\n" + peek_next(-1).loc.get_line_trace());
            }
        }
    }

    if (peek().type == TokenType::SEMICOLON) consume();

    auto node = std::make_unique<VariableDeclarationNode>();
    if (pattern) {
        node->pattern = std::move(pattern);
        node->identifier = "";  // not a simple identifier
    } else {
        node->identifier = name;
        node->pattern = nullptr;
    }
    node->value = std::move(value);
    node->is_constant = is_constant;
    node->token = idTok;
    return node;
}

std::unique_ptr<StatementNode> Parser::parse_import_declaration() {
    // 'tumia' token already consumed by caller; tokens[position-1] is the 'tumia' token.
    Token tumiaTok = tokens[position - 1];

    auto node = std::make_unique<ImportDeclarationNode>();
    node->token = tumiaTok;

    // Helper to consume optional trailing semicolon
    auto maybe_consume_semicolon = [&]() {
        if (peek().type == TokenType::SEMICOLON) consume();
    };

    // Helper: skip formatting tokens
    auto skip_formatting = [&]() {
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
    };

    // Case A: side-effect only import: tumia "path"
    if (peek().type == TokenType::STRING || peek().type == TokenType::SINGLE_QUOTED_STRING) {
        Token pathTok = consume();
        node->side_effect_only = true;
        node->module_path = pathTok.value;
        node->module_token = pathTok;
        maybe_consume_semicolon();
        return node;
    }

    // Case B: tumia * kutoka "path"
    if (peek().type == TokenType::STAR) {
        Token star = consume();
        node->import_all = true;
        expect(TokenType::KUTOKA, "Expected 'kutoka' after '*' in `tumia` statements");
        expect(TokenType::STRING, "Expected module string after 'kutoka' in `tumia` statements");
        Token pathTok = tokens[position - 1];
        node->module_path = pathTok.value;
        node->module_token = pathTok;
        maybe_consume_semicolon();
        return node;
    }

    // Case C: tumia { a, b as c } kutoka "path"
    if (peek().type == TokenType::OPENBRACE) {
        consume();  // '{'
        skip_formatting();

        // empty import list {}
        if (peek().type == TokenType::CLOSEBRACE) {
            consume();
            expect(TokenType::KUTOKA, "Expected 'kutoka' after import specifiers");
            expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
            Token pathTok = tokens[position - 1];
            node->module_path = pathTok.value;
            node->module_token = pathTok;
            maybe_consume_semicolon();
            return node;
        }

        while (true) {
            skip_formatting();

            // allow early close when layout inserted a DEDENT/newline before '}'
            if (peek().type == TokenType::CLOSEBRACE) break;

            expect(TokenType::IDENTIFIER, "Expected identifier in import specifier");
            Token impTok = tokens[position - 1];
            std::string imported = impTok.value;
            std::string local = imported;

            // optional alias: 'kama' IDENT
            skip_formatting();
            if (peek().type == TokenType::KAMA) {
                consume();  // 'kama'
                skip_formatting();
                expect(TokenType::IDENTIFIER, "Expected identifier after 'kama' in import alias");
                Token localTok = tokens[position - 1];
                local = localTok.value;
            }

            auto spec = std::make_unique<ImportSpecifier>();
            spec->imported = imported;
            spec->local = local;
            spec->token = impTok;
            node->specifiers.push_back(std::move(spec));

            skip_formatting();

            if (peek().type == TokenType::COMMA) {
                consume();
                // allow trailing comma before close (skip formatting then allow close)
                skip_formatting();
                if (peek().type == TokenType::CLOSEBRACE) break;
                continue;
            }

            // If next is closebrace or layout that eventually leads to closebrace, continue loop to detect it
            if (peek().type == TokenType::CLOSEBRACE) break;

            // otherwise break and let expect(CLOSEBRACE) validate
            break;
        }

        // Accept closing brace possibly after layout tokens
        skip_formatting();
        expect(TokenType::CLOSEBRACE, "Expected '}' after import specifiers");
        // require 'kutoka' and string path
        skip_formatting();
        expect(TokenType::KUTOKA, "Expected 'kutoka' after import specifiers");
        skip_formatting();
        expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
        Token pathTok = tokens[position - 1];
        node->module_path = pathTok.value;
        node->module_token = pathTok;
        maybe_consume_semicolon();
        return node;
    }

    // Case D: default/module binding: tumia localName [kama alias] kutoka "path"
    if (peek().type == TokenType::IDENTIFIER) {
        // Collect a dotted identifier sequence: IDENT ('.' IDENT)*
        std::vector<Token> idParts;
        Token firstId = consume();
        idParts.push_back(firstId);

        while (peek().type == TokenType::DOT) {
            consume();  // consume '.'
            // allow layout between '.' and identifier (tolerant)
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            expect(TokenType::IDENTIFIER, "Expected identifier after '.' in import shorthand");
            idParts.push_back(tokens[position - 1]);
        }

        // By default treat as:
        // - single ident: tumia math        -> default import from module "math" into local name "math"
        // - dotted: tumia console.print    -> import 'print' from module 'console' as local 'print'
        std::string imported;
        std::string local;
        std::string implicit_module_spec;
        Token module_tok_for_diag = idParts.front();  // fallback token for module diagnostics

        if (idParts.size() == 1) {
            // tumia X  -> default import of module X into local name X, implicit module_path = X
            imported = "default";
            local = idParts[0].value;
            implicit_module_spec = idParts[0].value;
        } else {
            // tumia A.B.C  -> imported = "C", module_spec = "A.B"
            imported = idParts.back().value;
            local = imported;
            std::ostringstream ss;
            for (size_t i = 0; i + 1 < idParts.size(); ++i) {
                if (i) ss << ".";
                ss << idParts[i].value;
            }
            implicit_module_spec = ss.str();
            module_tok_for_diag = idParts.front();
        }

        // optional alias 'kama'
        skip_formatting();
        if (peek().type == TokenType::KAMA) {
            consume();  // 'kama'
            skip_formatting();
            expect(TokenType::IDENTIFIER, "Expected identifier after 'kama' for import alias");
            Token aliasTok = tokens[position - 1];
            local = aliasTok.value;
        }

        // Now check for explicit 'kutoka' string which overrides implicit module spec
        skip_formatting();
        if (peek().type == TokenType::KUTOKA) {
            consume();  // 'kutoka'
            skip_formatting();
            expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
            Token pathTok = tokens[position - 1];
            node->module_path = pathTok.value;
            node->module_token = pathTok;
        } else {
            // No explicit 'kutoka' -> use implicit module spec from the dotted identifiers
            node->module_path = implicit_module_spec;
            node->module_token = module_tok_for_diag;
        }

        // Create the specifier: if dotted then named import, otherwise default import already mapped to "default"
        auto spec = std::make_unique<ImportSpecifier>();
        spec->imported = imported;
        spec->local = local;
        // Use the token corresponding to the imported name for location/diagnostics
        spec->token = idParts.back();
        node->specifiers.push_back(std::move(spec));

        maybe_consume_semicolon();
        return node;
    }
    // Otherwise it's a syntax error
    Token bad = peek();
    throw std::runtime_error("Parse error at " + bad.loc.to_string() + ": invalid import syntax after 'tumia'" + "\n --> Traced at: \n" + bad.loc.get_line_trace());
}

std::unique_ptr<StatementNode> Parser::parse_export_declaration() {
    // 'ruhusu' token already consumed by caller
    Token ruhusuTok = tokens[position - 1];

    if (saw_export) {
        throw std::runtime_error("Parse error at " + ruhusuTok.loc.to_string() + ": multiple 'ruhusu' (export) declarations are not allowed" + "\n --> Traced at: \n" + ruhusuTok.loc.get_line_trace());
    }

    auto node = std::make_unique<ExportDeclarationNode>();
    node->token = ruhusuTok;

    // Helper to skip formatting tokens
    auto skip_formatting = [&]() {
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
    };

    // Two main forms:
    //  - ruhusu IDENT  (default export of identifier)
    //  - ruhusu { a, b, c }  (named exports list)
    skip_formatting();
    if (peek().type == TokenType::IDENTIFIER) {
        Token idTok = consume();
        node->is_default = true;
        node->single_identifier = idTok.value;
        saw_export = true;
        if (peek().type == TokenType::SEMICOLON) consume();
        return node;
    }

    if (peek().type == TokenType::OPENBRACE) {
        consume();  // '{'
        skip_formatting();

        // empty export list {}
        if (peek().type == TokenType::CLOSEBRACE) {
            consume();
            saw_export = true;
            if (peek().type == TokenType::SEMICOLON) consume();
            return node;
        }

        while (true) {
            skip_formatting();

            // allow early close when layout inserted a DEDENT/newline before '}'
            if (peek().type == TokenType::CLOSEBRACE) break;

            expect(TokenType::IDENTIFIER, "Expected identifier in export list");
            Token idTok = tokens[position - 1];
            node->names.push_back(idTok.value);

            skip_formatting();
            if (peek().type == TokenType::COMMA) {
                consume();
                // allow trailing comma before close
                skip_formatting();
                if (peek().type == TokenType::CLOSEBRACE) break;
                continue;
            } else {
                // if next is CLOSEBRACE, loop will exit; otherwise break and let expect validate
                if (peek().type == TokenType::CLOSEBRACE) break;
                // allow layout -> next iteration will handle it
                continue;
            }
        }

        skip_formatting();
        expect(TokenType::CLOSEBRACE, "Expected '}' after export list");
        saw_export = true;
        if (peek().type == TokenType::SEMICOLON) consume();
        return node;
    }

    Token bad = peek();
    throw std::runtime_error("Parse error at " + bad.loc.to_string() + ": expected identifier or '{' after 'ruhusu'" + "\n --> Traced at: \n" + bad.loc.get_line_trace());
}

std::unique_ptr<StatementNode> Parser::parse_class_declaration() {
    // muundo has already been consumed by caller
    expect(TokenType::IDENTIFIER, "Expected class name after 'muundo' keyword.");
    Token idTok = tokens[position - 1];
    auto nameNode = std::make_unique<IdentifierNode>();
    nameNode->name = idTok.value;
    nameNode->token = idTok;

    std::unique_ptr<IdentifierNode> superNode = nullptr;

    // NEW: accept either:
    //   muundo Name rithi Super
    // or
    //   muundo Name(Super)
    // The parenthesized form is a shorthand (python-style) and works with either ':' (indent) or '{' body forms.
    if (peek().type == TokenType::RITHI) {
        consume();  // consume 'rithi'
        expect(TokenType::IDENTIFIER, "Expected base class name after 'rithi'.");
        Token superTok = tokens[position - 1];
        superNode = std::make_unique<IdentifierNode>();
        superNode->name = superTok.value;
        superNode->token = superTok;
    } else if (peek().type == TokenType::OPENPARENTHESIS) {
        // parenthesized shorthand: (Super)
        consume();  // consume '('

        // allow optional formatting inside parentheses (follow other parser tolerant patterns)
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();

        expect(TokenType::IDENTIFIER, "Expected base class name inside parentheses after class name");
        Token superTok = tokens[position - 1];
        superNode = std::make_unique<IdentifierNode>();
        superNode->name = superTok.value;
        superNode->token = superTok;

        // allow optional formatting before closing ')'
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
        expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after base class name");
    }

    std::unique_ptr<ClassBodyNode> body = nullptr;

    if (match(TokenType::COLON)) {
        // pythonic / indent-based body
        expect(TokenType::NEWLINE, "Expected newline after ':' in class declaration.");
        expect(TokenType::INDENT, "Expected indented block for class body.");

        // pass the class name and indicate non-brace style
        body = parse_class_body(nameNode->name, /*braceStyle=*/false);
        if (!body) body = std::make_unique<ClassBodyNode>();

        // caller closes the indented block
        expect(TokenType::DEDENT, "Expected dedent to close class body.");

    } else if (match(TokenType::OPENBRACE)) {
        // C-style block { ... }
        // pass the class name and indicate brace style = true
        body = parse_class_body(nameNode->name, /*braceStyle=*/true);
        if (!body) body = std::make_unique<ClassBodyNode>();

        // skip stray separators, then expect the closing brace
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
            consume();
        }
        expect(TokenType::CLOSEBRACE, "Expected '}' to close class body.");
    } else {
        expect(TokenType::COLON, "Expected ':' or '{' to begin class body.");
    }

    // Construct node
    auto classNode = std::make_unique<ClassDeclarationNode>(
        std::move(nameNode),
        std::move(superNode),
        std::move(body));

    // --- IMPORTANT: set token BEFORE any validation that might use it ---
    classNode->token = idTok;

    // Validation block (constructor/destructor handled specially)
    if (classNode->body) {
        int constructorCount = 0;
        int destructorCount = 0;
        std::unordered_map<std::string,
            int>
            memberCount;  // composite key -> count

        // methods
        for (const auto& mPtr : classNode->body->methods) {
            if (!mPtr) continue;
            const ClassMethodNode& m = *mPtr;

            // token fallback (use class token if method token is missing)
            Token methodTok = (m.token.type != TokenType::EOF_TOKEN) ? m.token : classNode->token;

            // constructor
            if (m.is_constructor) {
                constructorCount++;
                if (m.name != classNode->name->name) {
                    throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
                        ": constructor name '" + m.name + "' must match class name '" + classNode->name->name + "'." + "\n --> Traced at: \n" + methodTok.loc.get_line_trace());
                }
                if (m.is_static) {
                    throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
                        ": constructor must not be static." + "\n --> Traced at: \n" + methodTok.loc.get_line_trace());
                }
                // do NOT add constructor to memberCount (avoid collision with members named same as class)
                continue;  // <-- important: skip normal member counting
            }

            // destructor
            if (m.is_destructor) {
                destructorCount++;
                if (m.name != classNode->name->name) {
                    throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
                        ": destructor name '" + m.name + "' must match class name '" + classNode->name->name + "'." + "\n --> Traced at: \n" + methodTok.loc.get_line_trace());
                }
                if (m.is_static) {
                    throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
                        ": destructor must not be static." + "\n --> Traced at: \n" + methodTok.loc.get_line_trace());
                }
                // do NOT add destructor to memberCount
                continue;  // <-- important
            }

            // getter arg rule
            if (m.is_getter && !m.params.empty()) {
                throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
                    ": getter '" + m.name + "' must not have parameters." + "\n --> Traced at: \n" + methodTok.loc.get_line_trace());
            }

            // composite key: static/instance + method + name
            std::string mkey = std::string(m.is_static ? "S:" : "I:") + "M:" + m.name;
            memberCount[mkey]++;
        }

        // properties
        for (const auto& pPtr : classNode->body->properties) {
            if (!pPtr) continue;
            const ClassPropertyNode& p = *pPtr;
            Token propTok = (p.token.type != TokenType::EOF_TOKEN) ? p.token : classNode->token;

            std::string pkey = std::string(p.is_static ? "S:" : "I:") + "P:" + p.name;
            memberCount[pkey]++;
        }

        // enforce at most one constructor/destructor
        if (constructorCount > 1) {
            throw std::runtime_error("Parse error at " + classNode->token.loc.to_string() +
                ": multiple constructors defined for class '" + classNode->name->name + "'." + "\n --> Traced at: \n" + classNode->token.loc.get_line_trace());
        }
        if (destructorCount > 1) {
            throw std::runtime_error("Parse error at " + classNode->token.loc.to_string() +
                ": multiple destructors defined for class '" + classNode->name->name + "'." + "\n --> Traced at: \n" + classNode->token.loc.get_line_trace());
        }

        // report duplicates (static/instance and kind-aware)
        for (const auto& kv : memberCount) {
            if (kv.second > 1) {
                // pretty name: substring after last ':'
                auto pos = kv.first.rfind(':');
                std::string pretty = (pos == std::string::npos) ? kv.first : kv.first.substr(pos + 1);
                throw std::runtime_error("Parse error at " + classNode->token.loc.to_string() +
                    ": duplicate member name '" + pretty + "' found " + std::to_string(kv.second) + " times." + "\n --> Traced at: \n" + classNode->token.loc.get_line_trace());
            }
        }
    }

    return std::unique_ptr<StatementNode>(std::move(classNode));
}
std::unique_ptr<StatementNode> Parser::parse_print_statement(bool newline) {
    // capture the keyword token (CHAPISHA / ANDIKA) which was consumed by caller
    Token kwTok = tokens[position - 1];

    std::vector<std::unique_ptr<ExpressionNode>> args;
    if (peek().type == TokenType::OPENPARENTHESIS) {
        consume();
        if (peek().type != TokenType::CLOSEPARENTHESIS) {
            do {
                args.push_back(parse_expression());
            } while (match(TokenType::COMMA));
        }
        expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after print arguments");
    } else {
        // single expression without parentheses
        args.push_back(parse_expression());
    }
    if (peek().type == TokenType::SEMICOLON) consume();
    auto node = std::make_unique<PrintStatementNode>();
    node->expressions = std::move(args);
    node->newline = newline;
    node->token = kwTok;
    return node;
}

// Parse an assignment or expression statement starting with an identifier (or general expression fallback).
// This version builds full postfix expressions (member, index, calls) from the initial identifier,
// so forms like arr[0] = 1 and arr.ongeza(4) parse correctly. For compound ops (+=, ++/--)
// we only support them when the target is a simple IdentifierNode (preserves previous semantics).
// Parse an assignment or expression statement (updated to avoid moving the same unique_ptr twice
// and to allow assignable L-values beyond plain identifiers: Identifier, Member, Index).
std::unique_ptr<StatementNode> Parser::parse_assignment_or_expression_statement() {
    if (peek().type == TokenType::IDENTIFIER || peek().type == TokenType::SELF) {
        Token idTok = consume();

        std::unique_ptr<ExpressionNode> nodeExpr;
        if (idTok.type == TokenType::SELF) {
            nodeExpr = std::make_unique<ThisExpressionNode>();
            static_cast<ThisExpressionNode*>(nodeExpr.get())->token = idTok;
        } else {
            auto ident = std::make_unique<IdentifierNode>();
            ident->name = idTok.value;
            ident->token = idTok;
            nodeExpr = std::move(ident);
        }
        // helper: decide if an expression can be used as an assignment target
        auto is_assignable = [](ExpressionNode* n) -> bool {
            if (!n) return false;
            if (dynamic_cast<IdentifierNode*>(n)) return true;
            if (dynamic_cast<IndexExpressionNode*>(n)) return true;
            if (dynamic_cast<MemberExpressionNode*>(n)) return true;
            return false;
        };

        // Postfix expansion: accept calls, member access, indexing
        while (true) {
            if (peek().type == TokenType::OPENPARENTHESIS) {
                // call: convert current nodeExpr into a CallExpressionNode
                nodeExpr = parse_call(std::move(nodeExpr));
                continue;
            }
            if (peek().type == TokenType::DOT) {
                Token dotTok = consume();  // consume '.'
                expect(TokenType::IDENTIFIER, "Expected identifier after '.'");
                Token propTok = tokens[position - 1];
                auto mem = std::make_unique<MemberExpressionNode>();
                mem->object = std::move(nodeExpr);
                mem->property = propTok.value;
                mem->token = dotTok;
                nodeExpr = std::move(mem);
                continue;
            }

            if (peek().type == TokenType::QUESTION_DOT) {
                Token qdotTok = consume();  // '?.'

                // optional call: ident?.(...)
                if (peek().type == TokenType::OPENPARENTHESIS) {
                    nodeExpr = parse_call(std::move(nodeExpr));
                    if (auto c = dynamic_cast<CallExpressionNode*>(nodeExpr.get())) {
                        c->is_optional = true;
                        c->token = qdotTok;
                    }
                    continue;
                }

                // optional member: ident?.prop
                if (peek().type == TokenType::IDENTIFIER) {
                    Token propTok = consume();
                    auto mem = std::make_unique<MemberExpressionNode>();
                    mem->object = std::move(nodeExpr);
                    mem->property = propTok.value;
                    mem->token = qdotTok;
                    mem->is_optional = true;

                    // if call follows immediately, make the call optional too
                    if (peek().type == TokenType::OPENPARENTHESIS) {
                        auto callNode = parse_call(std::move(mem));
                        if (auto cptr = dynamic_cast<CallExpressionNode*>(callNode.get())) {
                            cptr->is_optional = true;
                            cptr->token = qdotTok;
                        }
                        nodeExpr = std::move(callNode);
                    } else {
                        nodeExpr = std::move(mem);
                    }
                    continue;
                }

                // optional computed index: ident?.[expr]
                if (peek().type == TokenType::OPENBRACKET) {
                    Token openIdx = consume();  // '['
                    auto idxExpr = parse_expression();
                    expect(TokenType::CLOSEBRACKET, "Expected ']' after index expression");
                    auto idxNode = std::make_unique<IndexExpressionNode>();
                    idxNode->object = std::move(nodeExpr);
                    idxNode->index = std::move(idxExpr);
                    idxNode->token = qdotTok;
                    idxNode->is_optional = true;
                    nodeExpr = std::move(idxNode);
                    continue;
                }

                // otherwise syntax error
                Token bad = peek();
                throw std::runtime_error("Parse error at " + bad.loc.to_string() + ": unexpected token after '?.'" + "\n --> Traced at: \n" + bad.loc.get_line_trace());
            }

            if (peek().type == TokenType::OPENBRACKET) {
                Token openIdx = consume();  // consume '['
                auto idxExpr = parse_expression();
                expect(TokenType::CLOSEBRACKET, "Expected ']' after index expression");
                auto idxNode = std::make_unique<IndexExpressionNode>();
                idxNode->object = std::move(nodeExpr);
                idxNode->index = std::move(idxExpr);
                idxNode->token = openIdx;
                nodeExpr = std::move(idxNode);
                continue;
            }
            break;
        }

        // At this point nodeExpr is the full left-side expression (Identifier, Member, Index, Call)
        // If next token is '=', create an AssignmentNode with target = nodeExpr
        // If next token is '=', create an AssignmentNode with target = nodeExpr
        if (peek().type == TokenType::ASSIGN) {
            if (!is_assignable(nodeExpr.get())) {
                Token opTok = peek();
                throw std::runtime_error("Invalid assignment target at " + opTok.loc.to_string() + "\n --> Traced at: \n" + opTok.loc.get_line_trace());
            }
            consume();  // '='
            auto value = parse_expression();
            if (peek().type == TokenType::SEMICOLON) consume();
            auto assign = std::make_unique<AssignmentNode>();
            assign->target = std::move(nodeExpr);
            assign->value = std::move(value);
            assign->token = idTok;
            return assign;
        }

        // Support compound assignment operators (+=, -=, *=) for assignable L-values (Identifier, Member, Index)
        if (peek().type == TokenType::PLUS_ASSIGN ||
            peek().type == TokenType::MINUS_ASSIGN ||
            peek().type == TokenType::TIMES_ASSIGN ||
            peek().type == TokenType::SLASH_ASSIGN ||
            peek().type == TokenType::DOUBLESTAR_ASSIGN ||
            peek().type == TokenType::PERCENT_ASSIGN ||
            peek().type == TokenType::NULLISH_ASSIGN ||
            peek().type == TokenType::AND_ASSIGN ||
            peek().type == TokenType::OR_ASSIGN ||
            peek().type == TokenType::BIT_AND_ASSIGN ||
            peek().type == TokenType::BIT_OR_ASSIGN ||
            peek().type == TokenType::BIT_XOR_ASSIGN) {
            if (!is_assignable(nodeExpr.get())) {
                Token opTok = peek();
                throw std::runtime_error("Compound assignment is only supported for assignable targets at " + opTok.loc.to_string() + "\n --> Traced at: \n" + opTok.loc.get_line_trace());
            }

            Token opTok = consume();  // one of +=, -=, *=
            auto right = parse_expression();

            // build BinaryExpressionNode: left <op> right
            auto bin = std::make_unique<BinaryExpressionNode>();
            if (opTok.type == TokenType::PLUS_ASSIGN)
                bin->op = "+";
            else if (opTok.type == TokenType::MINUS_ASSIGN)
                bin->op = "-";
            else if (opTok.type == TokenType::TIMES_ASSIGN)
                bin->op = "*";
            else if (opTok.type == TokenType::SLASH_ASSIGN)
                bin->op = "/";
            else if (opTok.type == TokenType::DOUBLESTAR_ASSIGN)
                bin->op = "**";
            else if (opTok.type == TokenType::PERCENT_ASSIGN)
                bin->op = "%";
            else if (opTok.type == TokenType::NULLISH_ASSIGN)
                bin->op = "??";
            else if (opTok.type == TokenType::AND_ASSIGN)
                bin->op = "&&";
            else if (opTok.type == TokenType::OR_ASSIGN)
                bin->op = "||";
            else if (opTok.type == TokenType::BIT_AND_ASSIGN)
                bin->op = "&";
            else if (opTok.type == TokenType::BIT_OR_ASSIGN)
                bin->op = "|";
            else if (opTok.type == TokenType::BIT_XOR_ASSIGN)
                bin->op = "^";

            // clone the left for the computed expression so we don't lose ownership of the original
            bin->left = nodeExpr->clone();
            bin->right = std::move(right);
            bin->token = opTok;

            auto assign = std::make_unique<AssignmentNode>();
            // move the original nodeExpr into the assignment target (single owner)
            assign->target = std::move(nodeExpr);
            assign->value = std::move(bin);
            assign->token = idTok;
            if (peek().type == TokenType::SEMICOLON) consume();
            return assign;
        }

        // Support ++ / -- for assignable L-values like x++ or arr[i]++
        if (peek().type == TokenType::INCREMENT || peek().type == TokenType::DECREMENT) {
            if (!is_assignable(nodeExpr.get())) {
                Token opTok = peek();
                throw std::runtime_error("Increment/decrement is only supported for assignable targets at " + opTok.loc.to_string() + "\n --> Traced at: \n" + opTok.loc.get_line_trace());
            }
            Token opTok = consume();
            // build BinaryExpressionNode: left + 1 OR left - 1
            auto bin = std::make_unique<BinaryExpressionNode>();
            bin->op = (opTok.type == TokenType::INCREMENT) ? "+" : "-";
            // clone left for computation, keep original to move into the assignment target
            bin->left = nodeExpr->clone();
            auto one = std::make_unique<NumericLiteralNode>();
            one->value = 1;
            one->token = opTok;
            bin->right = std::move(one);
            bin->token = opTok;

            auto assign = std::make_unique<AssignmentNode>();
            // assign target is the original nodeExpr (moved from here)
            assign->target = std::move(nodeExpr);
            assign->value = std::move(bin);
            assign->token = idTok;
            if (peek().type == TokenType::SEMICOLON) consume();
            return assign;
        }

        // Otherwise it's an expression statement: return the fully-expanded expression (call/member/index)
        if (peek().type == TokenType::SEMICOLON) consume();
        auto stmt = std::make_unique<ExpressionStatementNode>();
        stmt->expression = std::move(nodeExpr);
        return stmt;
    }

    // fallback: expression statement
    auto expr = parse_expression();

    // Reject bare walrus expressions (they must be in expression context, not statement context)
    if (dynamic_cast<AssignmentExpressionNode*>(expr.get())) {
        throw SwaziError(
            "SyntaxError",
            "Assignment expression ('ni') cannot be used as a statement. Use 'data' for variable declarations.",
            expr->token.loc);
    }

    if (peek().type == TokenType::SEMICOLON) consume();
    auto stmt = std::make_unique<ExpressionStatementNode>();
    stmt->expression = std::move(expr);
    return stmt;
}

std::unique_ptr<StatementNode> Parser::parse_sequential_functions(bool outer_is_async, bool outer_is_generator) {
    Token openTok = consume();  // consume '('

    auto skip_formatting = [&]() {
        while (peek().type == TokenType::NEWLINE ||
            peek().type == TokenType::INDENT ||
            peek().type == TokenType::DEDENT) {
            consume();
        }
    };

    skip_formatting();

    if (peek().type == TokenType::CLOSEPARENTHESIS) {
        Token tok = peek();
        throw SwaziError("SyntaxError", "Empty function declaration list not allowed", tok.loc);
    }

    std::vector<std::unique_ptr<FunctionDeclarationNode>> declarations;

    while (peek().type != TokenType::CLOSEPARENTHESIS && peek().type != TokenType::EOF_TOKEN) {
        skip_formatting();

        // Check for per-function modifiers
        bool func_is_async = outer_is_async;
        bool func_is_generator = outer_is_generator;

        if (peek().type == TokenType::STAR) {
            consume();
            func_is_generator = true;
        }

        if (peek().type == TokenType::ASYNC) {
            consume();
            func_is_async = true;
        }

        // Reject async generators per existing rules
        if (func_is_async && func_is_generator) {
            throw SwaziError("SyntaxError",
                "Async functions cannot be generators (kazi* cannot be async).",
                peek().loc);
        }

        skip_formatting();

        // Parse function name
        expect(TokenType::IDENTIFIER, "Expected function name in declaration list");
        Token nameTok = tokens[position - 1];

        auto funcNode = std::make_unique<FunctionDeclarationNode>();
        funcNode->name = nameTok.value;
        funcNode->token = nameTok;
        funcNode->is_async = func_is_async;
        funcNode->is_generator = func_is_generator;

        skip_formatting();

        // Parse parameters (either parenthesized or bare identifiers)
        bool rest_seen = false;
        if (match(TokenType::OPENPARENTHESIS)) {
            skip_formatting();
            if (peek().type != TokenType::CLOSEPARENTHESIS) {
                while (true) {
                    skip_formatting();

                    // Rest param
                    if (peek().type == TokenType::ELLIPSIS) {
                        Token ellTok = consume();
                        if (rest_seen) {
                            throw SwaziError("SyntaxError", "Only one rest parameter allowed", ellTok.loc);
                        }
                        expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
                        Token id = tokens[position - 1];

                        auto p = std::make_unique<ParameterNode>();
                        p->token = ellTok;
                        p->name = id.value;
                        p->is_rest = true;
                        p->rest_required_count = 0;

                        if (peek().type == TokenType::OPENBRACKET) {
                            consume();
                            expect(TokenType::NUMBER, "Expected number in rest count");
                            Token numTok = tokens[position - 1];
                            try {
                                p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
                            } catch (...) {
                                throw SwaziError("SyntaxError", "Invalid rest count", numTok.loc);
                            }
                            expect(TokenType::CLOSEBRACKET, "Expected ']'");
                        }

                        funcNode->parameters.push_back(std::move(p));
                        rest_seen = true;

                        if (peek().type == TokenType::COMMA) {
                            if (peek_next().type != TokenType::CLOSEPARENTHESIS) {
                                throw SwaziError("SyntaxError",
                                    "Rest parameter must be last", peek_next().loc);
                            }
                        }
                        break;
                    }

                    // Normal parameter
                    expect(TokenType::IDENTIFIER, "Expected parameter name");
                    Token pTok = tokens[position - 1];
                    auto pnode = std::make_unique<ParameterNode>();
                    pnode->token = pTok;
                    pnode->name = pTok.value;
                    pnode->is_rest = false;
                    pnode->defaultValue = nullptr;

                    skip_formatting();
                    if (peek().type == TokenType::ASSIGN) {
                        consume();
                        skip_formatting();
                        pnode->defaultValue = parse_expression();
                    }

                    funcNode->parameters.push_back(std::move(pnode));

                    skip_formatting();
                    if (match(TokenType::COMMA)) {
                        skip_formatting();
                        if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                        continue;
                    }
                    break;
                }
            }
            expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after parameters");
        } else {
            // Bare identifiers (legacy form)
            while (peek().type == TokenType::IDENTIFIER || peek().type == TokenType::ELLIPSIS) {
                if (peek().type == TokenType::ELLIPSIS) {
                    Token ellTok = consume();
                    if (rest_seen) {
                        throw SwaziError("SyntaxError", "Only one rest parameter allowed", ellTok.loc);
                    }
                    expect(TokenType::IDENTIFIER, "Expected identifier");
                    Token id = tokens[position - 1];

                    auto p = std::make_unique<ParameterNode>();
                    p->token = ellTok;
                    p->name = id.value;
                    p->is_rest = true;
                    p->rest_required_count = 0;

                    if (peek().type == TokenType::OPENBRACKET) {
                        consume();
                        expect(TokenType::NUMBER, "Expected number");
                        Token numTok = tokens[position - 1];
                        try {
                            p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
                        } catch (...) {
                            throw SwaziError("SyntaxError", "Invalid rest count", numTok.loc);
                        }
                        expect(TokenType::CLOSEBRACKET, "Expected ']'");
                    }

                    funcNode->parameters.push_back(std::move(p));
                    rest_seen = true;

                    if (peek().type == TokenType::COMMA) {
                        throw SwaziError("SyntaxError",
                            "Rest parameter must be last", peek().loc);
                    }
                    break;
                }

                Token pTok = consume();
                auto pnode = std::make_unique<ParameterNode>();
                pnode->token = pTok;
                pnode->name = pTok.value;
                pnode->is_rest = false;
                pnode->defaultValue = nullptr;

                if (peek().type == TokenType::ASSIGN) {
                    consume();
                    pnode->defaultValue = parse_expression();
                }

                funcNode->parameters.push_back(std::move(pnode));

                if (!match(TokenType::COMMA)) break;
            }
        }

        skip_formatting();

        // Parse body (either colon+indent or brace)
        struct AsyncScopeGuard {
            Parser& parser;
            bool prev_async, prev_gen;
            AsyncScopeGuard(Parser& p, bool a, bool g)
                : parser(p), prev_async(p.in_async_function), prev_gen(p.in_generator_function) {
                parser.in_async_function = a;
                parser.in_generator_function = g;
            }
            ~AsyncScopeGuard() {
                parser.in_async_function = prev_async;
                parser.in_generator_function = prev_gen;
            }
        };

        AsyncScopeGuard guard(*this, func_is_async, func_is_generator);

        if (match(TokenType::COLON)) {
            expect(TokenType::NEWLINE, "Expected newline after ':'");
            expect(TokenType::INDENT, "Expected indent");
            while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
                auto stmt = parse_statement();
                if (!stmt) break;
                funcNode->body.push_back(std::move(stmt));
            }
            expect(TokenType::DEDENT, "Expected dedent");
        } else if (match(TokenType::OPENBRACE)) {
            while (peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::EOF_TOKEN) {
                while (peek().type == TokenType::NEWLINE ||
                    peek().type == TokenType::INDENT ||
                    peek().type == TokenType::DEDENT) {
                    consume();
                }
                if (peek().type == TokenType::CLOSEBRACE) break;
                auto stmt = parse_statement();
                if (!stmt) break;
                funcNode->body.push_back(std::move(stmt));
            }
            expect(TokenType::CLOSEBRACE, "Expected '}'");
        } else {
            expect(TokenType::COLON, "Expected ':' or '{' for function body");
        }

        declarations.push_back(std::move(funcNode));

        skip_formatting();
        if (peek().type == TokenType::COMMA) {
            consume();
            skip_formatting();
            if (peek().type == TokenType::CLOSEPARENTHESIS) break;
            continue;
        }

        if (peek().type == TokenType::CLOSEPARENTHESIS) break;

        throw SwaziError("SyntaxError", "Expected ',' or ')' in function list", peek().loc);
    }

    skip_formatting();
    expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after function list");

    if (peek().type == TokenType::SEMICOLON) consume();

    auto seqFunc = std::make_unique<SequentialFunctionDeclarationNode>();
    seqFunc->token = openTok;
    seqFunc->is_async = outer_is_async;
    seqFunc->is_generator = outer_is_generator;
    seqFunc->declarations = std::move(declarations);

    return seqFunc;
}
std::unique_ptr<StatementNode> Parser::parse_function_declaration() {
    struct AsyncScopeGuard {
        Parser& parser;
        bool prev;
        AsyncScopeGuard(Parser& p, bool newVal) : parser(p), prev(p.in_async_function) {
            parser.in_async_function = newVal;
        }
        ~AsyncScopeGuard() {
            parser.in_async_function = prev;
        }
    };

    struct GeneratorScopeGuard {
        Parser& parser;
        bool prev;
        GeneratorScopeGuard(Parser& p, bool newVal) : parser(p), prev(p.in_generator_function) {
            parser.in_generator_function = newVal;
        }
        ~GeneratorScopeGuard() {
            parser.in_generator_function = prev;
        }
    };

    // The 'kazi' token was already consumed by parse_statement
    auto funcNode = std::make_unique<FunctionDeclarationNode>();

    // NEW: optional generator marker immediately after 'kazi': `kazi* name ...`
    if (peek().type == TokenType::STAR) {
        consume();
        funcNode->is_generator = true;
    }

    // optional ASYNC modifier BEFORE the function name (syntax: 'kazi ASYNC name ...')
    if (peek().type == TokenType::ASYNC) {
        consume();
        funcNode->is_async = true;
    }

    expect(TokenType::IDENTIFIER, "Expected function name after 'kazi'");
    Token idTok = tokens[position - 1];
    // Reject async generators per rule
    if (funcNode->is_async && funcNode->is_generator) {
        throw SwaziError("SyntaxError", "Async functions cannot be generators (kazi* cannot be async).", idTok.loc);
    }

    funcNode->name = idTok.value;
    funcNode->token = idTok;

    // Parse parameters
    // Parse parameters: support either a parenthesized parameter list (`kazi name(...)`)
    // or the existing bare identifier form (`kazi name a b ...`). Both are accepted.
    bool rest_seen = false;
    if (match(TokenType::OPENPARENTHESIS)) {
        // Parenthesized params: ( ... )
        if (peek().type != TokenType::CLOSEPARENTHESIS) {
            while (true) {
                // rest param: ...name[<number>]
                if (peek().type == TokenType::ELLIPSIS) {
                    Token ellTok = consume();
                    if (rest_seen) {
                        throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed" + "\n --> Traced at: \n" + ellTok.loc.get_line_trace());
                    }
                    expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
                    Token nameTok = tokens[position - 1];

                    auto p = std::make_unique<ParameterNode>();
                    p->token = ellTok;
                    p->name = nameTok.value;
                    p->is_rest = true;
                    p->rest_required_count = 0;

                    if (peek().type == TokenType::OPENBRACKET) {
                        consume();  // '['
                        expect(TokenType::NUMBER, "Expected number inside rest count brackets");
                        Token numTok = tokens[position - 1];
                        try {
                            p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
                        } catch (...) {
                            throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string() + "\n --> Traced at: \n" + numTok.loc.get_line_trace());
                        }
                        expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
                    }

                    funcNode->parameters.push_back(std::move(p));
                    rest_seen = true;

                    // rest must be last (allow trailing comma only if closing paren next)
                    if (peek().type == TokenType::COMMA) {
                        if (peek_next().type != TokenType::CLOSEPARENTHESIS) {
                            Token bad = tokens[position];
                            throw std::runtime_error("Rest parameter must be the last parameter at " + bad.loc.to_string() + "\n --> Traced at: \n" + bad.loc.get_line_trace());
                        }
                        // allow trailing comma then break
                    }

                    break;
                }

                // normal identifier param (with optional default)
                expect(TokenType::IDENTIFIER, "Expected identifier in parameter list");
                Token nameTok = tokens[position - 1];
                auto pnode = std::make_unique<ParameterNode>();
                pnode->token = nameTok;
                pnode->name = nameTok.value;
                pnode->is_rest = false;
                pnode->rest_required_count = 0;
                pnode->defaultValue = nullptr;

                // new: optional '?' syntactic sugar -> defaultValue = null
                if (peek().type == TokenType::QUESTIONMARK) {
                    Token qm = consume();
                    pnode->defaultValue = std::make_unique<NullNode>(nameTok);
                } else if (peek().type == TokenType::ASSIGN) {
                    // optional default initializer: '=' expression
                    consume();  // consume '='
                    pnode->defaultValue = parse_expression();
                    if (!pnode->defaultValue) {
                        throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position - 1].loc.to_string() + "\n --> Traced at: \n" + tokens[position - 1].loc.get_line_trace());
                    }
                }

                funcNode->parameters.push_back(std::move(pnode));

                if (match(TokenType::COMMA)) {
                    // allow trailing comma before ')'
                    if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                    continue;
                }
                break;
            }
        }
        expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after parameters");

    } else {
        // Legacy / bare form: zero or more identifiers / rest without surrounding parentheses.
        // This preserves existing behaviour for scripts that use the bare form.
        while (true) {
            // rest param: ...name[<number>]
            if (peek().type == TokenType::ELLIPSIS) {
                Token ellTok = consume();
                if (rest_seen) {
                    throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed" + "\n --> Traced at: \n" + ellTok.loc.get_line_trace());
                }
                expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
                Token nameTok = tokens[position - 1];

                auto p = std::make_unique<ParameterNode>();
                p->token = ellTok;
                p->name = nameTok.value;
                p->is_rest = true;
                p->rest_required_count = 0;

                if (peek().type == TokenType::OPENBRACKET) {
                    consume();  // '['
                    expect(TokenType::NUMBER, "Expected number inside rest count brackets");
                    Token numTok = tokens[position - 1];
                    try {
                        p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
                    } catch (...) {
                        throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string() + "\n --> Traced at: \n" + numTok.loc.get_line_trace());
                    }
                    expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
                }

                funcNode->parameters.push_back(std::move(p));
                rest_seen = true;

                // rest must be last in bare form
                if (peek().type == TokenType::COMMA) {
                    Token c = tokens[position];  // lookahead
                    throw std::runtime_error("Rest parameter must be the last parameter at " + c.loc.to_string() + "\n --> Traced at: \n" + c.loc.get_line_trace());
                }
                break;
            }

            // normal identifier
            if (peek().type == TokenType::IDENTIFIER) {
                Token pTok = consume();
                auto pnode = std::make_unique<ParameterNode>();
                pnode->token = pTok;
                pnode->name = pTok.value;
                pnode->is_rest = false;
                pnode->rest_required_count = 0;
                pnode->defaultValue = nullptr;

                // new: optional '?' syntactic sugar -> defaultValue = null
                if (peek().type == TokenType::QUESTIONMARK) {
                    Token qm = consume();
                    pnode->defaultValue = std::make_unique<NullNode>(pTok);
                } else if (peek().type == TokenType::ASSIGN) {
                    // optional default initializer: '=' expression
                    consume();  // consume '='
                    pnode->defaultValue = parse_expression();
                    if (!pnode->defaultValue) {
                        throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position - 1].loc.to_string() + "\n --> Traced at: \n" + tokens[position - 1].loc.get_line_trace());
                    }
                }

                funcNode->parameters.push_back(std::move(pnode));

                if (match(TokenType::COMMA)) continue;
                // no separator -> break to body parsing
                continue;
            }

            // neither IDENTIFIER nor ELLIPSIS => end of param list (bare form)
            break;
        }
    }

    // Now, branch on the token that starts the function body
    if (match(TokenType::COLON)) {
        // --- Indentation-based body ---
        expect(TokenType::NEWLINE, "Expected newline after ':' in function declaration");
        expect(TokenType::INDENT, "Expected indented block for function body");

        AsyncScopeGuard async_guard(*this, funcNode->is_async);
        GeneratorScopeGuard gen_guard(*this, funcNode->is_generator);

        // indentation-based body (defensive: parse_statement may return null at EOF)
        while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
            auto stmt = parse_statement();
            if (!stmt) break;  // EOF or nothing more
            funcNode->body.push_back(std::move(stmt));
        }

        expect(TokenType::DEDENT, "Expected dedent to close function body");

    } else if (match(TokenType::OPENBRACE)) {
        AsyncScopeGuard async_guard(*this, funcNode->is_async);
        GeneratorScopeGuard gen_guard(*this, funcNode->is_generator);

        // --- Brace-based body ---
        // Loop but skip separators before attempting to parse a statement so that
        // trailing NEWLINE/INDENT/DEDENT before '}' don't cause parse_statement to
        // receive a '}' token unexpectedly.
        while (peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::EOF_TOKEN) {
            // consume any separators between statements
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
                consume();
            }
            if (peek().type == TokenType::CLOSEBRACE || peek().type == TokenType::EOF_TOKEN) break;
            auto stmt = parse_statement();
            if (!stmt) break;
            funcNode->body.push_back(std::move(stmt));
        }

        expect(TokenType::CLOSEBRACE, "Expected '}' to close function body");
    } else {
        // --- Syntax Error ---
        expect(TokenType::COLON, "Expected ':' or '{' to begin function body");
    }

    return funcNode;
}
std::unique_ptr<StatementNode> Parser::parse_return_statement() {
    // The 'rudisha' token was already consumed by parse_statement
    Token kwTok = tokens[position - 1];

    auto retNode = std::make_unique<ReturnStatementNode>();
    retNode->token = kwTok;

    // The return value is optional. If the next token is a semicolon or newline,
    // it's a return with no value.
    if (peek().type != TokenType::SEMICOLON && peek().type != TokenType::NEWLINE && peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::DEDENT) {
        retNode->value = parse_expression();
    }

    // Optionally consume a semicolon at the end of the statement
    if (peek().type == TokenType::SEMICOLON) {
        consume();
    }

    return retNode;
}

std::unique_ptr<StatementNode> Parser::parse_break_statement() {
    // The 'SIMAMA' token was already consumed by parse_statement
    Token kwTok = tokens[position - 1];

    auto node = std::make_unique<BreakStatementNode>();
    node->token = kwTok;

    // optionally consume a semicolon
    if (peek().type == TokenType::SEMICOLON) consume();

    return node;
}

std::unique_ptr<StatementNode> Parser::parse_continue_statement() {
    // The 'ENDELEA' token was already consumed by parse_statement
    Token kwTok = tokens[position - 1];

    auto node = std::make_unique<ContinueStatementNode>();
    node->token = kwTok;

    // optionally consume a semicolon
    if (peek().type == TokenType::SEMICOLON) consume();

    return node;
}

std::unique_ptr<CaseNode> Parser::parse_switch_case() {
    auto caseNode = std::make_unique<CaseNode>();

    if (match(TokenType::IKIWA)) {
        caseNode->test = parse_expression();
    } else if (match(TokenType::KAIDA)) {
        caseNode->test = nullptr;  // default
    } else {
        Token tok = peek();
        throw std::runtime_error("Parse error at " + tok.loc.to_string() +
            ": Expected 'ikiwa' or 'kaida' in switch" + "\n --> Traced at: \n" + tok.loc.get_line_trace());
    }

    // Now parse the body, which can be ':' (indented block) or '{' (brace block)
    if (match(TokenType::COLON)) {
        // For indented blocks we must require NEWLINE + INDENT
        expect(TokenType::NEWLINE, "Expected newline after ':' in case");
        expect(TokenType::INDENT, "Expected indented block after case ':'");

        while (peek().type != TokenType::DEDENT &&
            peek().type != TokenType::EOF_TOKEN) {
            auto stmt = parse_statement();
            if (!stmt) break;
            caseNode->body.push_back(std::move(stmt));
        }
        expect(TokenType::DEDENT, "Expected dedent to close case body");

    } else if (match(TokenType::OPENBRACE)) {
        // Brace-based block
        while (peek().type != TokenType::CLOSEBRACE &&
            peek().type != TokenType::EOF_TOKEN) {
            while (peek().type == TokenType::NEWLINE ||
                peek().type == TokenType::INDENT ||
                peek().type == TokenType::DEDENT) {
                consume();
            }
            if (peek().type == TokenType::CLOSEBRACE ||
                peek().type == TokenType::EOF_TOKEN) break;
            auto stmt = parse_statement();
            if (!stmt) break;
            caseNode->body.push_back(std::move(stmt));
        }
        expect(TokenType::CLOSEBRACE, "Expected '}' to close case body");
        if (peek().type == TokenType::NEWLINE) {
            consume();
        }
    } else {
        Token tok = peek();
        throw std::runtime_error("Parse error at " + tok.loc.to_string() +
            ": Expected ':' or '{' after case header" + "\n --> Traced at: \n" + tok.loc.get_line_trace());
    }

    return caseNode;
}

std::unique_ptr<StatementNode> Parser::parse_switch_statement() {
    // 'chagua' token was already consumed by parse_statement
    auto node = std::make_unique<SwitchNode>();
    node->discriminant = parse_expression();

    if (match(TokenType::COLON)) {
        // Pythonic style
        expect(TokenType::NEWLINE, "Expected newline after ':' in switch");
        expect(TokenType::INDENT, "Expected indented block for switch body");

        while (peek().type != TokenType::DEDENT &&
            peek().type != TokenType::EOF_TOKEN) {
            node->cases.push_back(parse_switch_case());
        }
        expect(TokenType::DEDENT, "Expected dedent to close switch body");

    } else if (match(TokenType::OPENBRACE)) {
        // C-style braces
        while (peek().type != TokenType::CLOSEBRACE &&
            peek().type != TokenType::EOF_TOKEN) {
            while (peek().type == TokenType::NEWLINE ||
                peek().type == TokenType::INDENT ||
                peek().type == TokenType::DEDENT) {
                consume();
            }
            if (peek().type == TokenType::CLOSEBRACE ||
                peek().type == TokenType::EOF_TOKEN) break;
            node->cases.push_back(parse_switch_case());
        }
        expect(TokenType::CLOSEBRACE, "Expected '}' to close switch body");

    } else {
        Token tok = peek();
        throw std::runtime_error("Parse error at " + tok.loc.to_string() +
            ": Expected ':' or '{' after 'chagua' expression" + "\n --> Traced at: \n" + tok.loc.get_line_trace());
    }

    return node;
}

std::unique_ptr<StatementNode> Parser::parse_try_catch() {
    // caller already consumed 'jaribu' (JARIBU)
    if (position == 0) {
        throw std::runtime_error("Internal parser error: at " + peek().loc.to_string() + " \n --> Parse_try_catch called with position == 0" +
            "\n --> Traced around: \n" + peek().loc.get_line_trace());
    }

    auto node = std::make_unique<TryCatchNode>();

    // record the starting 'jaribu' token on the node (useful for error reporting / locations)
    node->token = tokens[position - 1];

    // small lambda to parse either an indentation-based or brace-based block
    auto parse_block_into = [&](std::vector<std::unique_ptr<StatementNode>>& dest) {
        if (match(TokenType::COLON)) {
            // indentation-based body
            expect(TokenType::NEWLINE, "Expected newline after ':'");
            expect(TokenType::INDENT, "Expected indented block");

            while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
                auto stmt = parse_statement();
                if (!stmt) break;  // defensive: EOF or parse error
                dest.push_back(std::move(stmt));
            }

            expect(TokenType::DEDENT, "Expected dedent to close block");

        } else if (match(TokenType::OPENBRACE)) {
            // brace-based body
            while (peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::EOF_TOKEN) {
                // skip separators between statements
                while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
                    consume();
                }
                if (peek().type == TokenType::CLOSEBRACE || peek().type == TokenType::EOF_TOKEN) break;
                auto stmt = parse_statement();
                if (!stmt) break;
                dest.push_back(std::move(stmt));
            }
            expect(TokenType::CLOSEBRACE, "Expected '}' to close block");
        } else {
            // helpful error-message: caller expected a block start
            expect(TokenType::COLON, "Expected ':' or '{' to begin block");
        }
    };

    // --- parse try block ---
    parse_block_into(node->tryBlock);

    // --- parse catch (MAKOSA) ---
    if (match(TokenType::MAKOSA)) {
        // optional error variable: either IDENTIFIER or ( IDENTIFIER )
        if (match(TokenType::IDENTIFIER)) {
            // match() consumed the identifier, so the token is at tokens[position-1]
            node->errorVar = tokens[position - 1].value;  // <-- fixed: use the lexeme/text field
        } else if (match(TokenType::OPENPARENTHESIS)) {
            expect(TokenType::IDENTIFIER, "Expected identifier in catch parentheses");
            node->errorVar = tokens[position - 1].value;
            expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after catch identifier");
        } else {
            // no explicit variable provided — make this a clear error
            expect(TokenType::IDENTIFIER, "Expected an error identifier to hold the error object after 'makosa'.");
            // (expect will throw, so we won't reach here)
        }

        // now parse the catch body into catchBlock
        parse_block_into(node->catchBlock);
    } else {
        // error: 'makosa' expected after try block
        expect(TokenType::MAKOSA, "Expected 'makosa' after 'jaribu' block");
    }

    // --- optional finally (KISHA) ---
    if (match(TokenType::KISHA)) {
        parse_block_into(node->finallyBlock);
    }

    // debugging the ast
    // std::cout << trycatch_to_json(*node) << std::endl;

    return std::unique_ptr<StatementNode>(std::move(node));
}

std::unique_ptr<StatementNode> Parser::parse_throw_statement() {
    // 'throw' token (THROW) was already consumed by parse_statement
    Token throwTok = tokens[position - 1];

    auto node = std::make_unique<ThrowStatementNode>();
    node->token = throwTok;

    // throw requires an expression (no bare 'throw' allowed)
    if (peek().type == TokenType::SEMICOLON ||
        peek().type == TokenType::NEWLINE ||
        peek().type == TokenType::EOF_TOKEN) {
        throw std::runtime_error(
            "SyntaxError at " + throwTok.loc.to_string() +
            ": 'throw' requires an expression (error message, Error(...), or other callable)" +
            "\n --> Traced at:\n" + throwTok.loc.get_line_trace());
    }

    // Parse the expression that produces the error
    node->value = parse_expression();

    // Optional semicolon
    if (peek().type == TokenType::SEMICOLON) {
        consume();
    }

    return node;
}
