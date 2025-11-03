// scr/parser/parser.cpp
#include "parser.hpp"

#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>

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
            "Parse error at " + tok.loc.to_string() + ": " + errMsg + "\n--> Traced at:\n" + peek_next(-1).loc.get_line_trace());
    }
    consume();
}

bool Parser::is_lambda_ahead() {
    size_t saved = position;  // remember position

    // must start with '('
    if (peek().type != TokenType::OPENPARENTHESIS) return false;
    consume();  // consume '('

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

    // helper to detect ellipsis token tolerant to lexer differences
    auto is_ellipsis_token = [&]() -> bool {
        Token p = peek();
        return p.type == TokenType::ELLIPSIS || p.value == "...";
    };

    bool ok = true;
    bool seen_rest = false;  // <<--- new: track if we've already seen a rest param

    // Skip initial layout inside parens
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();

    // Empty parameter list
    if (peek().type == TokenType::CLOSEPARENTHESIS) {
        consume();  // consume ')'
        // skip layout before checking LAMBDA
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
            consume();
        }
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
            // If we've already seen a rest param, that's a syntax error.
            if (seen_rest) {
                // produce a helpful error pointing at the current token
                throw std::runtime_error("Multiple rest parameters are not allowed at " + peek().loc.to_string());
            }

            consume();  // consume '...'
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            if (peek().type != TokenType::IDENTIFIER) {
                ok = false;
                break;
            }
            consume();  // identifier

            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            if (peek().type == TokenType::OPENBRACKET) {
                consume();
                while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
                if (peek().type != TokenType::NUMBER) {
                    ok = false;
                    break;
                }
                consume();
                while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
                if (peek().type != TokenType::CLOSEBRACKET) {
                    ok = false;
                    break;
                }
                consume();
            }

            // rest must be last or followed only by a trailing comma before ')'
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            if (peek().type == TokenType::COMMA) {
                // if comma and next isn't closeparen, it's not a valid lambda param list
                if (peek_next().type != TokenType::CLOSEPARENTHESIS) {
                    // throw with location because a parameter follows a rest -> syntax error
                    throw std::runtime_error("Parameter not allowed after rest parameter at " + peek_next().loc.to_string());
                }
                consume();  // consume trailing comma
            }

            // mark that we've seen the rest (so any further param tokens are invalid)
            seen_rest = true;
            continue;
        }

        // If we've already seen a rest parameter, any other parameter is an error.
        if (seen_rest) {
            throw std::runtime_error("Cannot have parameter after rest parameter at " + peek().loc.to_string());
        }

        // identifier param (maybe with default 'id = <expr>')
        if (peek().type == TokenType::IDENTIFIER) {
            consume();  // identifier
            // allow layout between identifier and '='
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();

            if (peek().type == TokenType::ASSIGN) {
                consume();  // consume '='
                // allow layout after '='
                while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();

                // scan the default expression until comma or ')', leaving them unconsumed
                if (!skip_default_expr()) {
                    ok = false;
                    break;
                }

                // Now explicitly handle the token that ended the default expression:
                // accept either a comma (consume it and continue) or a close-paren (leave loop)
                while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
                if (peek().type == TokenType::COMMA) {
                    consume();  // consume separator and continue to next param
                    // allow trailing comma before ')'
                    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
                    if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                    continue;
                } else if (peek().type == TokenType::CLOSEPARENTHESIS) {
                    break;  // end of param list
                } else {
                    ok = false;
                    break;
                }
            }

            // no default: handle comma separator if present
            while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
            if (peek().type == TokenType::COMMA) {
                consume();
                // allow trailing comma before ')'
                while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
                if (peek().type == TokenType::CLOSEPARENTHESIS) break;
                continue;
            }

            // otherwise expect ')' or next param (handled by loop)
            continue;
        }

        // anything else is invalid inside a lambda param-list
        ok = false;
        break;
    }

    // must end with ')'
    if (!ok || peek().type != TokenType::CLOSEPARENTHESIS) {
        position = saved;
        return false;
    }

    consume();  // consume ')'

    // skip layout tokens between ')' and '=>'
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
        consume();
    }

    // check if next token is LAMBDA
    bool found = (peek().type == TokenType::LAMBDA);

    position = saved;  // restore
    return found;
}

std::unique_ptr<ExpressionNode> Parser::parse_pattern() {
    if (peek().type == TokenType::OPENBRACKET) {
        return parse_array_pattern();
    } else if (peek().type == TokenType::OPENBRACE) {
        return parse_object_pattern();
    }
    Token tok = peek();
    throw std::runtime_error("Parse error at " + tok.loc.to_string() + ": Expected array or object pattern");
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
        if(peek().type == TokenType::NEWLINE) {
          consume();
          if(peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
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
                    throw std::runtime_error("Parse error at " + peek_next().loc.to_string() + ": parameter not allowed after rest element");
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
            throw std::runtime_error("Parse error at " + tok.loc.to_string() + ": Unexpected token in array pattern" + "\n --> Traced at: \n" + tok.loc.get_line_trace());
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
    
    if(peek().type == TokenType::NEWLINE) {
      consume();
      if(peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
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
        if(peek().type == TokenType::NEWLINE) {
          consume();
          if(peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
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
    if(peek().type == TokenType::NEWLINE) {
          consume();
          if(peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
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