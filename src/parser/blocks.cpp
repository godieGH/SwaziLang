// src/parser/blocks.cpp
#include <cctype>
#include <sstream>
#include <stdexcept>

#include "SwaziError.hpp"
#include "parser.hpp"

// ---------- helper: parse block ----------
std::vector<std::unique_ptr<StatementNode>> Parser::parse_block(bool accept_brace_style) {
    std::vector<std::unique_ptr<StatementNode>> body;

    if (accept_brace_style && peek().type == TokenType::OPENBRACE) {
        consume();  // consume '{'
        // loop until closing brace
        while (peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::EOF_TOKEN) {
            // skip separators between statements
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
                consume();
            }
            if (peek().type == TokenType::CLOSEBRACE || peek().type == TokenType::EOF_TOKEN) break;
            auto stmt = parse_statement();
            if (!stmt) break;
            body.push_back(std::move(stmt));
        }
        expect(TokenType::CLOSEBRACE, "Expected '}' to close block");
    } else {
        // INDENT-style block: caller should have consumed COLON and NEWLINE already,
        // and we expect an INDENT token now.
        expect(TokenType::INDENT, "Expected indented block");
        while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
            auto stmt = parse_statement();
            if (!stmt) break;
            body.push_back(std::move(stmt));
        }
        expect(TokenType::DEDENT, "Expected dedent to close indented block");
    }

    return body;
}

std::unique_ptr<ClassBodyNode> Parser::parse_class_body(const std::string& className, bool braceStyle) {
    auto body = std::make_unique<ClassBodyNode>();
    body->token = peek();

    while (true) {
        Token t = peek();

        // termination (caller will consume the class terminator)
        if (!braceStyle && (t.type == TokenType::DEDENT || t.type == TokenType::EOF_TOKEN)) break;
        if (braceStyle && (t.type == TokenType::CLOSEBRACE || t.type == TokenType::EOF_TOKEN)) break;

        // skip separators / blank lines
        if (t.type == TokenType::NEWLINE) {
            consume();
            continue;
        }

        // collect modifiers for this single member
        bool is_static = false;
        bool is_private = false;
        bool is_locked = false;
        while (peek().type == TokenType::STAR || peek().type == TokenType::AT_SIGN || peek().type == TokenType::AMPERSAND) {
            Token mod = consume();
            if (mod.type == TokenType::STAR)
                is_static = true;
            else if (mod.type == TokenType::AT_SIGN)
                is_private = true;
            else if (mod.type == TokenType::AMPERSAND)
                is_locked = true;
        }

        if (t.type == TokenType::NEWLINE ||
            (braceStyle && (t.type == TokenType::INDENT || t.type == TokenType::DEDENT))) {
            consume();
            continue;
        }

        Token cur = peek();

        // METHOD: 'tabia' <method-name or thabiti or ...>
        if (cur.type == TokenType::TABIA) {
            consume();  // consume 'tabia'
            auto method = parse_class_method(is_private, is_static, is_locked, className,
                /*isCtor=*/false,
                /*isDtor=*/false,
                /*braceStyle=*/braceStyle);
            if (method) body->methods.push_back(std::move(method));

            // optional separators
            if (peek().type == TokenType::SEMICOLON) consume();
            if (peek().type == TokenType::NEWLINE) consume();
            continue;
        }

        // DESTRUCTOR: '~' IDENTIFIER
        if (cur.type == TokenType::TILDE) {
            consume();  // consume '~'
            expect(TokenType::IDENTIFIER, "Expected class name after '~' for destructor");
            Token nameTok = tokens[position - 1];
            if (nameTok.value != className) {
                throw SwaziError("SyntaxError", "Destructor name must match class name '" + className + "'.", nameTok.loc);
            }

            auto method = parse_class_method(is_private, is_static, is_locked, className,
                /*isCtor=*/false,
                /*isDtor=*/true,
                /*braceStyle=*/braceStyle);
            if (method) body->methods.push_back(std::move(method));

            if (peek().type == TokenType::SEMICOLON) consume();
            if (peek().type == TokenType::NEWLINE) consume();
            continue;
        }

        // IDENTIFIER: Could be property (x = ... or just 'y'), or constructor (className)
        if (cur.type == TokenType::IDENTIFIER) {
            Token next = peek_next(1);

            // A token sequence where the identifier is followed by ASSIGN or newline/semicolon/comma or end-of-member
            // is a property (supports `y` uninitialized, or `x = 7`).
            bool nextSignalsProperty =
                next.type == TokenType::ASSIGN ||
                next.type == TokenType::COMMA ||
                next.type == TokenType::SEMICOLON ||
                next.type == TokenType::NEWLINE ||
                next.type == TokenType::CLOSEBRACE ||
                next.type == TokenType::DEDENT ||
                next.type == TokenType::EOF_TOKEN;

            // If the identifier equals the class name -> constructor (preferred)
            if (cur.value == className) {
                // do NOT pre-consume name; let parse_class_method handle it
                auto ctor = parse_class_method(is_private, is_static, is_locked, className,
                    /*isCtor=*/true,
                    /*isDtor=*/false,
                    /*braceStyle=*/braceStyle);
                if (ctor) body->methods.push_back(std::move(ctor));

                if (peek().type == TokenType::SEMICOLON) consume();
                if (peek().type == TokenType::NEWLINE) consume();
                continue;
            }

            // Otherwise if next suggests a property, parse it as a property
            if (nextSignalsProperty) {
                expect(TokenType::IDENTIFIER, "Expected property name");
                Token nameTok = tokens[position - 1];
                std::string propName = nameTok.value;

                std::unique_ptr<ExpressionNode> initExpr = nullptr;
                if (peek().type == TokenType::ASSIGN) {
                    consume();  // consume '='
                    initExpr = parse_expression();
                    if (!initExpr) {
                        throw std::runtime_error("Parse error at " + peek().loc.to_string() + ": Expected expression after '='" + "\n --> Traced at: \n" + peek().loc.get_line_trace());
                    }
                }

                auto propNode = std::make_unique<ClassPropertyNode>();
                propNode->token = nameTok;
                propNode->name = propName;
                propNode->is_private = is_private;
                propNode->is_static = is_static;
                propNode->is_locked = is_locked;
                propNode->value = initExpr ? std::move(initExpr) : nullptr;
                body->properties.push_back(std::move(propNode));

                if (peek().type == TokenType::SEMICOLON) consume();
                if (peek().type == TokenType::NEWLINE) consume();
                continue;
            }

            // If none matched, it's unexpected (we require 'tabia' for non-constructor methods)
            throw std::runtime_error("Parse error at " + cur.loc.to_string() + ": Unexpected identifier in class body; expected property, constructor, or 'tabia' method." + "\n --> Traced at: \n" + cur.loc.get_line_trace());
        }

        // anything else is unexpected
        throw std::runtime_error("Parse error at " + peek().loc.to_string() + ": Unexpected token in class body" + "\n --> Traced at: \n" + peek().loc.get_line_trace());
    }

    return body;
}

std::unique_ptr<ClassMethodNode> Parser::parse_class_method(
    bool is_private,
    bool is_static,
    bool is_locked,
    const std::string& className,
    bool isCtor,
    bool isDtor,
    bool braceStyle) {
    auto node = std::make_unique<ClassMethodNode>();
    node->is_private = is_private;
    node->is_static = is_static;
    node->is_locked = is_locked;
    node->is_constructor = isCtor;
    node->is_destructor = isDtor;
    node->is_getter = false;

    // ---- determine method name ----
    if (isDtor) {
        // caller consumed '~' and verified identifier equality; the identifier token is previous token
        node->name = className;
        node->token = tokens[position - 1];
    } else {
        // Detect getter form: lexer classifies 'thabiti' as CONSTANT in your lexer
        if (peek().type == TokenType::CONSTANT && peek().value == "thabiti") {
            consume();  // consume 'thabiti'
            node->is_getter = true;
            expect(TokenType::IDENTIFIER, "Expected getter name after 'thabiti'");
            Token nameTok = tokens[position - 1];
            node->name = nameTok.value;
            node->token = nameTok;
        } else {
            // Expect normal method name (or constructor name if isCtor)
            expect(TokenType::IDENTIFIER, "Expected method name");
            Token nameTok = tokens[position - 1];
            node->name = nameTok.value;
            node->token = nameTok;

            if (isCtor && node->name != className) {
                throw std::runtime_error("Parse error at " + nameTok.loc.to_string() +
                    ": Constructor name must match class name '" + className + "'" + "\n --> Traced at: \n" + nameTok.loc.get_line_trace());
            }
        }
    }

    // ---- parse parameters (getter must not accept any) ----
    node->params.clear();
    if (!node->is_getter) {
        bool rest_seen = false;

        if (match(TokenType::OPENPARENTHESIS)) {
            // parenthesized list
            while (peek().type != TokenType::CLOSEPARENTHESIS && peek().type != TokenType::EOF_TOKEN) {
                // support rest param: ...name[<number>]
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

                    node->params.push_back(std::move(p));
                    rest_seen = true;

                    // after rest param there must be a closing ')' (rest must be last)
                    if (peek().type != TokenType::CLOSEPARENTHESIS) {
                        Token bad = peek();
                        throw std::runtime_error("Rest parameter must be the last parameter at " + bad.loc.to_string() + "\n --> Traced at: \n" + bad.loc.get_line_trace());
                    }
                    break;
                }

                // normal identifier param (with optional default)
                expect(TokenType::IDENTIFIER, "Expected parameter name");
                Token pTok = tokens[position - 1];
                auto pnode = std::make_unique<ParameterNode>();
                pnode->token = pTok;
                pnode->name = pTok.value;
                pnode->is_rest = false;
                pnode->rest_required_count = 0;
                pnode->defaultValue = nullptr;

                // optional default initializer: '=' expression
                if (peek().type == TokenType::ASSIGN) {
                    consume();  // consume '='
                    pnode->defaultValue = parse_expression();
                    if (!pnode->defaultValue) {
                        throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position - 1].loc.to_string() + "\n --> Traced at: \n" + tokens[position - 1].loc.get_line_trace());
                    }
                }

                node->params.push_back(std::move(pnode));

                if (match(TokenType::COMMA)) {
                    // allow trailing comma before ')'
                    if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                    continue;
                }
                break;
            }
            expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after parameter list");
        } else {
            // bare identifiers (stop when body-start tokens encountered)
            while (peek().type == TokenType::IDENTIFIER || peek().type == TokenType::ELLIPSIS) {
                // rest param inline
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

                    node->params.push_back(std::move(p));
                    rest_seen = true;

                    // rest must be last in bare form
                    if (peek().type == TokenType::COMMA) {
                        Token c = tokens[position];  // lookahead
                        throw std::runtime_error("Rest parameter must be the last parameter at " + c.loc.to_string() + "\n --> Traced at: \n" + c.loc.get_line_trace());
                    }
                    break;
                }

                // normal identifier
                Token pTok = consume();
                if (pTok.type != TokenType::IDENTIFIER) {
                    throw std::runtime_error("Expected parameter name at " + pTok.loc.to_string() + "\n --> Traced at: \n" + pTok.loc.get_line_trace());
                }
                auto pnode = std::make_unique<ParameterNode>();
                pnode->token = pTok;
                pnode->name = pTok.value;
                pnode->is_rest = false;
                pnode->rest_required_count = 0;
                pnode->defaultValue = nullptr;

                // optional default initializer: '=' expression (only allowed in paren form semantically; but accept here for consistency)
                if (peek().type == TokenType::ASSIGN) {
                    consume();  // consume '='
                    pnode->defaultValue = parse_expression();
                    if (!pnode->defaultValue) {
                        throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position - 1].loc.to_string() + "\n --> Traced at: \n" + tokens[position - 1].loc.get_line_trace());
                    }
                }

                node->params.push_back(std::move(pnode));

                if (match(TokenType::COMMA)) continue;
                break;
            }
        }
    } else {
        // getter cannot have parameters
        if (peek().type == TokenType::OPENPARENTHESIS || peek().type == TokenType::IDENTIFIER) {
            throw std::runtime_error("Parse error at " + peek().loc.to_string() + ": Getter must not accept parameters" + "\n --> Traced at: \n" + peek().loc.get_line_trace());
        }
    }

    // ---- parse body using parse_block helper ----
    // Two valid forms:
    //  1) ':' NEWLINE INDENT ... DEDENT  (pythonic)  -> we should consume ':' and NEWLINE then call parse_block(false)
    //  2) '{' ... '}'                      (c-style) -> call parse_block(true) which will consume '{' itself
    if (peek().type == TokenType::COLON) {
        // consume ':' and the following NEWLINE so parse_block(false) can expect INDENT
        consume();  // ':'
        expect(TokenType::NEWLINE, "Expected newline after ':' for method body");
        // parse_block(false) will expect INDENT and DEDENT and return statements
        auto stmts = parse_block(false);  // returns vector<unique_ptr<StatementNode>>
        node->body = std::move(stmts);
    } else if (peek().type == TokenType::OPENBRACE) {
        // parse_block(true) will consume '{' and '}' and return body
        auto stmts = parse_block(true);
        node->body = std::move(stmts);
    } else {
        throw std::runtime_error("Parse error at " + peek().loc.to_string() + ": Expected ':' or '{' to begin method body" + "\n --> Traced at: \n" + peek().loc.get_line_trace());
    }

    return node;
}