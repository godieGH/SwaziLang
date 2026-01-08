// scr/parser/parser.cpp
#include "parser.hpp"

#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "SwaziError.hpp"

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens) {}

// Return current token or EOF token
Token Parser::peek() const {
    if (position < tokens.size()) return tokens[position];
    return Token{
        TokenType::EOF_TOKEN,
        "",
        TokenLocation("<eof>", 0, 0, 0)};
}

Token Parser::peek_next(size_t offset) const {
    if (position + offset < tokens.size()) {
        return tokens[position + offset];
    }
    return Token{
        TokenType::EOF_TOKEN,
        "",
        TokenLocation("<eof>", 0, 0, 0)};
}

// Consume and return the next token or EOF token
Token Parser::consume() {
    if (position < tokens.size()) return tokens[position++];
    return Token{
        TokenType::EOF_TOKEN,
        "",
        TokenLocation("<eof>", 0, 0, 0)};
}

bool Parser::match(TokenType t) {
    if (peek().type == t) {
        consume();
        return true;
    }
    return false;
}

void Parser::expect(TokenType t, const std::string& errMsg) {
    if (peek().type != t) {
        Token tok = peek();
        throw std::runtime_error(
            "SyntaxError at " + tok.loc.to_string() + ": " + errMsg + "\n--> Traced at:\n" + peek_next(-1).loc.get_line_trace());
    }
    consume();
}

bool Parser::is_lambda_ahead() {
    size_t saved = position;  // remember position

    auto skip_layout = [&]() {
        while (peek().type == TokenType::NEWLINE ||
            peek().type == TokenType::INDENT ||
            peek().type == TokenType::DEDENT) {
            consume();
        }
    };

    // optional leading ASYNC
    if (peek().type == TokenType::ASYNC) {
        consume();  // consume ASYNC for lookahead
        skip_layout();
    }

    // helper to detect ellipsis token tolerant to lexer differences
    auto is_ellipsis_token = [&]() -> bool {
        Token p = peek();
        return p.type == TokenType::ELLIPSIS || p.value == "...";
    };

    // helper to advance over a "default expression" in a tolerant way:
    auto skip_default_expr = [&]() -> bool {
        int depth = 0;
        while (true) {
            Token t = peek();
            if (t.type == TokenType::EOF_TOKEN) {
                return false;
            }
            if (t.type == TokenType::NEWLINE || t.type == TokenType::INDENT || t.type == TokenType::DEDENT) {
                consume();
                continue;
            }
            if (t.type == TokenType::OPENPARENTHESIS || t.type == TokenType::OPENBRACKET) {
                depth++;
                consume();
                continue;
            }
            if (t.type == TokenType::CLOSEPARENTHESIS || t.type == TokenType::CLOSEBRACKET) {
                if (depth > 0) {
                    depth--;
                    consume();
                    continue;
                }
                return true;
            }
            if (depth == 0 && t.type == TokenType::COMMA) {
                return true;
            }
            consume();
        }
    };

    // Parenthesized lambda form: (...) => ...
    if (peek().type == TokenType::OPENPARENTHESIS) {
        consume();  // consume '('

        bool ok = true;
        bool seen_rest = false;

        // Skip initial layout inside parens
        skip_layout();

        // Empty parameter list
        if (peek().type == TokenType::CLOSEPARENTHESIS) {
            consume();  // consume ')'
            skip_layout();
            bool found = (peek().type == TokenType::LAMBDA);
            position = saved;
            return found;
        }

        while (peek().type != TokenType::CLOSEPARENTHESIS &&
            peek().type != TokenType::EOF_TOKEN) {
            // skip layout between parameters
            if (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
                consume();
                continue;
            }

            // rest param '...name' (optional [...number])
            if (is_ellipsis_token()) {
                Token loc_token = peek();  // capture loc for possible error
                if (seen_rest) {
                    position = saved;
                    throw SwaziError("SyntaxError", "Multiple rest parameters are not allowed.", loc_token.loc);
                }

                consume();  // consume '...'
                skip_layout();

                if (peek().type != TokenType::IDENTIFIER) {
                    Token err_tok = peek();
                    position = saved;
                    throw SwaziError("SyntaxError", "Expected identifier after rest parameter '...'.", err_tok.loc);
                }
                consume();  // identifier

                skip_layout();
                if (peek().type == TokenType::OPENBRACKET) {
                    consume();
                    skip_layout();
                    if (peek().type != TokenType::NUMBER) {
                        position = saved;
                        return false;
                    }
                    consume();  // number
                    skip_layout();
                    if (peek().type != TokenType::CLOSEBRACKET) {
                        position = saved;
                        return false;
                    }
                    consume();  // ]
                }

                // rest must be last or followed only by a trailing comma before ')'
                skip_layout();
                if (peek().type == TokenType::COMMA) {
                    // if comma and next isn't closeparen, it's invalid (parameter follows rest)
                    if (peek_next().type != TokenType::CLOSEPARENTHESIS) {
                        Token err_tok = peek_next();
                        position = saved;
                        throw SwaziError("SyntaxError", "Parameter not allowed after rest parameter.", err_tok.loc);
                    }
                    consume();  // consume trailing comma
                }

                seen_rest = true;
                continue;
            }

            // If we've already seen a rest parameter, any other parameter is an error.
            if (seen_rest) {
                Token err_tok = peek();
                position = saved;
                throw SwaziError("SyntaxError", "Cannot have parameter after rest parameter.", err_tok.loc);
            }

            // identifier param (maybe with default 'id = <expr>')
            if (peek().type == TokenType::IDENTIFIER) {
                consume();  // identifier
                skip_layout();

                if (peek().type == TokenType::ASSIGN) {
                    consume();  // consume '='
                    skip_layout();

                    // scan the default expression until comma or ')', leaving them unconsumed
                    if (!skip_default_expr()) {
                        position = saved;
                        return false;
                    }

                    // handle the token that ended the default expression:
                    skip_layout();
                    if (peek().type == TokenType::COMMA) {
                        consume();  // separator and continue
                        skip_layout();
                        if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                        continue;
                    } else if (peek().type == TokenType::CLOSEPARENTHESIS) {
                        break;
                    } else {
                        position = saved;
                        return false;
                    }
                }

                // no default: handle comma separator if present
                skip_layout();
                if (peek().type == TokenType::COMMA) {
                    consume();
                    skip_layout();
                    if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                    continue;
                }

                // otherwise continue to next param or close-paren
                continue;
            }

            // anything else is invalid inside a lambda param-list
            position = saved;
            return false;
        }

        // must end with ')'
        if (peek().type != TokenType::CLOSEPARENTHESIS) {
            position = saved;
            return false;
        }

        consume();  // consume ')'
        skip_layout();

        bool found = (peek().type == TokenType::LAMBDA);
        position = saved;
        return found;
    }

    // Single-identifier lambda form: id => ...
    if (peek().type == TokenType::IDENTIFIER) {
        consume();  // identifier
        skip_layout();
        bool found = (peek().type == TokenType::LAMBDA);
        position = saved;
        return found;
    }

    // Not a lambda
    position = saved;
    return false;
}
std::unique_ptr<ExpressionNode> Parser::parse_pattern() {
    if (peek().type == TokenType::OPENBRACKET) {
        return parse_array_pattern();
    } else if (peek().type == TokenType::OPENBRACE) {
        return parse_object_pattern();
    }
    Token tok = peek();
    throw SwaziError("SyntaxError", "Expected array or object pattern.", tok.loc);
}

std::unique_ptr<ExpressionNode> Parser::parse_array_pattern() {
    Token openTok = consume();  // consume '['
    auto node = std::make_unique<ArrayPatternNode>();
    node->token = openTok;

    // empty pattern []
    if (peek().type == TokenType::CLOSEBRACKET) {
        consume();
        return node;
    }

    while (true) {
        if (peek().type == TokenType::NEWLINE) {
            consume();
            if (peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            continue;
        }
        // hole: leading comma indicates an empty slot
        if (peek().type == TokenType::COMMA) {
            consume();
            node->elements.push_back(nullptr);  // hole
            // if immediate close then end
            if (peek().type == TokenType::CLOSEBRACKET)
                break;
            else
                continue;
        }

        // rest element: ...name
        if (peek().type == TokenType::ELLIPSIS || peek().value == "...") {
            Token ell = consume();
            // allow layout tokens between ellipsis and identifier
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
            Token nameTok = tokens[position - 1];
            auto id = std::make_unique<IdentifierNode>();
            id->name = nameTok.value;
            id->token = nameTok;
            // wrap as SpreadElementNode so evaluator can detect rest
            auto spread = std::make_unique<SpreadElementNode>(ell, std::move(id));
            node->elements.push_back(std::move(spread));

            // rest must be last (optional trailing comma allowed)
            if (peek().type == TokenType::COMMA) {
                // if comma is followed by something other than CLOSEBRACKET it's an error
                if (peek_next().type != TokenType::CLOSEBRACKET) {
                    throw SwaziError("SyntaxError", "Parameter not allowed after rest element.", peek_next().loc);
                }
                consume();  // consume trailing comma
            }
            break;
        }

        // identifier element
        if (peek().type == TokenType::IDENTIFIER) {
            Token nameTok = consume();
            auto id = std::make_unique<IdentifierNode>();
            id->name = nameTok.value;
            id->token = nameTok;
            node->elements.push_back(std::move(id));
        } else {
            Token tok = peek();
            throw SwaziError("SyntaxError", "Unexpected token in array pattern.", tok.loc);
        }

        // separator or end
        if (peek().type == TokenType::COMMA) {
            consume();
            // if trailing comma and immediate close, allow and break
            if (peek().type == TokenType::CLOSEBRACKET) break;
            continue;
        } else {
            break;
        }
    }

    if (peek().type == TokenType::NEWLINE) {
        consume();
        if (peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
    }
    expect(TokenType::CLOSEBRACKET, "Expected ']' to close array pattern");
    return node;
}

std::unique_ptr<ExpressionNode> Parser::parse_object_pattern() {
    Token openTok = consume();  // consume '{'
    auto node = std::make_unique<ObjectPatternNode>();
    node->token = openTok;

    // empty pattern {}
    if (peek().type == TokenType::CLOSEBRACE) {
        consume();
        return node;
    }

    while (true) {
        if (peek().type == TokenType::NEWLINE) {
            consume();
            if (peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            continue;
        }
        // expect identifier as key
        expect(TokenType::IDENTIFIER, "Expected property name in object pattern");
        Token keyTok = tokens[position - 1];
        std::string key = keyTok.value;

        std::unique_ptr<ExpressionNode> target;

        // either shorthand: name  OR  name : alias
        if (peek().type == TokenType::COLON) {
            consume();  // consume ':'
            // allow layout between ':' and identifier
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            expect(TokenType::IDENTIFIER, "Expected identifier as target after ':' in object pattern");
            Token tgtTok = tokens[position - 1];
            auto id = std::make_unique<IdentifierNode>();
            id->name = tgtTok.value;
            id->token = tgtTok;
            target = std::move(id);
        } else {
            // shorthand: bind to same name
            auto id = std::make_unique<IdentifierNode>();
            id->name = key;
            id->token = keyTok;
            target = std::move(id);
        }

        auto prop = std::make_unique<ObjectPatternProperty>();
        prop->key = key;
        prop->value = std::move(target);
        node->properties.push_back(std::move(prop));

        if (peek().type == TokenType::COMMA) {
            consume();
            // allow trailing comma before closing brace
            if (peek().type == TokenType::CLOSEBRACE) break;
            continue;
        } else {
            break;
        }
    }
    if (peek().type == TokenType::NEWLINE) {
        consume();
        if (peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
    }
    expect(TokenType::CLOSEBRACE, "Expected '}' to close object pattern");
    return node;
}

// ---------- parse entry ----------
std::unique_ptr<ProgramNode> Parser::parse() {
    auto program = std::make_unique<ProgramNode>();
    while (peek().type != TokenType::EOF_TOKEN) {
        // skip separators and stray dedent/indent at top-level
        if (peek().type == TokenType::NEWLINE || peek().type == TokenType::SEMICOLON ||
            peek().type == TokenType::DEDENT || peek().type == TokenType::INDENT) {
            consume();
            continue;
        }
        auto stmt = parse_statement();
        if (stmt)
            program->body.push_back(std::move(stmt));
        else
            break;  // defensive: stop if parser returned null (EOF or similar)
    }
    return program;
}

// ---------- statements ----------
std::unique_ptr<StatementNode> Parser::parse_statement() {
    // skip leading newlines before a statement
    while (peek().type == TokenType::NEWLINE) {
        consume();
    }

    // If we've reached EOF after skipping separators, return null.
    if (peek().type == TokenType::EOF_TOKEN) return nullptr;

    // If the next token is a DEDENT (end of indented block) or a CLOSEBRACE
    // (end of brace block), return null so the caller can handle block end.
    if (peek().type == TokenType::DEDENT || peek().type == TokenType::CLOSEBRACE) {
        return nullptr;
    }

    Token p = peek();  // now capture the first non-newline token

    if (p.type == TokenType::FUTA) {
        Token futaTok = consume();  // consume 'futa'

        // if followed by '(' treat as expression form with optional args, else treat as bare-target form
        if (peek().type == TokenType::OPENPARENTHESIS) {
            // parse futa(...) then wrap as expression-statement
            expect(TokenType::OPENPARENTHESIS, "Expected '(' after 'futa'");
            std::unique_ptr<ExpressionNode> target;
            auto delExpr = std::make_unique<DeleteExpressionNode>();
            delExpr->token = futaTok;

            if (peek().type != TokenType::CLOSEPARENTHESIS) {
                // first expression is the target
                delExpr->target = parse_expression();

                // optional additional arguments (destructor args)
                while (match(TokenType::COMMA)) {
                    delExpr->arguments.push_back(parse_expression());
                }
            }

            expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after futa(...)");

            auto exprStmt = std::make_unique<ExpressionStatementNode>();
            exprStmt->token = futaTok;
            exprStmt->expression = std::move(delExpr);
            return exprStmt;

        } else {
            // Bare-form: futa <expression>   (no parentheses, no args)
            // parse the target as an expression (typically an IDENTIFIER, member, or call result)
            auto target = parse_expression();  // parse expression grammar to consume the target correctly
            auto delExpr = std::make_unique<DeleteExpressionNode>(std::move(target));
            delExpr->token = futaTok;

            // wrap in statement so it's a statement-level delete
            auto exprStmt = std::make_unique<ExpressionStatementNode>();
            exprStmt->token = futaTok;
            exprStmt->expression = std::move(delExpr);
            return exprStmt;
        }
    }

    if (p.type == TokenType::JARIBU) {
        Token tryTok = consume();
        return parse_try_catch();
    }

    if (p.type == TokenType::KAZI) {
        consume();  // consume 'kazi'
        Token kaziTok = tokens[position - 1];

        bool is_generator = false;
        bool is_async = false;

        // Check for generator marker
        if (peek().type == TokenType::STAR) {
            consume();
            is_generator = true;
        }

        // Check for async modifier
        if (peek().type == TokenType::ASYNC) {
            consume();
            is_async = true;
        }

        // Reject async generators
        if (is_async && is_generator) {
            throw SwaziError("SyntaxError",
                "Async functions cannot be generators", kaziTok.loc);
        }

        // Check for sequential form: kazi (...) or kazi* (...) or kazi async (...)
        if (peek().type == TokenType::OPENPARENTHESIS) {
            return parse_sequential_functions(is_async, is_generator);
        }

        // Otherwise parse single function (existing logic, but need to preserve modifiers)
        // Rewind position to before 'kazi' consumption and call existing parser
        position--;                    // back to 'kazi'
        if (is_generator) position--;  // back to '*'
        if (is_async) position--;      // back to 'async'
        consume();                     // re-consume 'kazi'
        return parse_function_declaration();
    }

    if (p.type == TokenType::MUUNDO) {
        consume();  // consume 'muundo'
        return parse_class_declaration();
    }

    if (p.type == TokenType::TUMIA) {
        consume();  // consume 'tumia'
        return parse_import_declaration();
    }

    if (p.type == TokenType::RUHUSU) {
        consume();  // consume 'ruhusu'
        return parse_export_declaration();
    }

    if (p.type == TokenType::CHAGUA) {
        consume();  // consume 'chagua'
        return parse_switch_statement();
    }

    if (p.type == TokenType::RUDISHA) {
        consume();  // consume 'rudisha'
        return parse_return_statement();
    }
    if (p.type == TokenType::THROW) {
        consume();  // consume 'throw/tupa'
        return parse_throw_statement();
    }
    if (p.type == TokenType::ENDELEA) {
        consume();
        return parse_continue_statement();
    }
    if (p.type == TokenType::SIMAMA) {
        consume();
        return parse_break_statement();
    }
    if (p.type == TokenType::KAMA) {
        return parse_if_statement();
    }
    // New loop keyword dispatch
    if (p.type == TokenType::FOR) {
        return parse_for_statement();
    }
    if (p.type == TokenType::WHILE) {
        return parse_while_statement();
    }
    if (p.type == TokenType::DOWHILE) {
        return parse_do_while_statement();
    }

    if (p.type == TokenType::DATA) {
        consume();
        return parse_variable_declaration();
    }
    if (p.type == TokenType::CHAPISHA) {
        consume();
        return parse_print_statement(true);
    }
    if (p.type == TokenType::ANDIKA) {
        consume();
        return parse_print_statement(false);
    }
    return parse_assignment_or_expression_statement();
}