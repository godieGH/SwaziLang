#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <sstream>

Parser::Parser(const std::vector < Token>& tokens): tokens(tokens) {}

// Return current token or EOF token
Token Parser::peek() const {
   if (position < tokens.size()) return tokens[position];
   return Token {
      TokenType::EOF_TOKEN,
      "",
      TokenLocation("<eof>", 0, 0, 0)
   };
}

// Consume and return the next token or EOF token
Token Parser::consume() {
   if (position < tokens.size()) return tokens[position++];
   return Token {
      TokenType::EOF_TOKEN,
      "",
      TokenLocation("<eof>", 0, 0, 0)
   };
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
         "Parse error at " + tok.loc.to_string() + ": " + errMsg
      );
   }
   consume();
}

// ---------- parse entry ----------
std::unique_ptr < ProgramNode > Parser::parse() {
   auto program = std::make_unique < ProgramNode > ();
   while (peek().type != TokenType::EOF_TOKEN) {
      // skip separators and stray dedent/indent at top-level
      if (peek().type == TokenType::NEWLINE || peek().type == TokenType::SEMICOLON ||
         peek().type == TokenType::DEDENT || peek().type == TokenType::INDENT) {
         consume();
         continue;
      }
      auto stmt = parse_statement();
      if (stmt) program->body.push_back(std::move(stmt));
      else break; // defensive: stop if parser returned null (EOF or similar)
   }
   return program;
}

// ---------- helper: parse block ----------
std::vector < std::unique_ptr < StatementNode>> Parser::parse_block(bool accept_brace_style) {
   std::vector < std::unique_ptr < StatementNode>> body;

   if (accept_brace_style && peek().type == TokenType::OPENBRACE) {
      consume(); // consume '{'
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

// ---------- statements ----------
std::unique_ptr < StatementNode > Parser::parse_statement() {
   // skip leading newlines and any leading INDENT tokens before a statement
   while (peek().type == TokenType::NEWLINE ||
      peek().type == TokenType::INDENT) {
      consume();
   }

   // If we've reached EOF after skipping separators, return null.
   if (peek().type == TokenType::EOF_TOKEN) return nullptr;

   // If the next token is a DEDENT (end of indented block) or a CLOSEBRACE
   // (end of brace block), return null so the caller can handle block end.
   if (peek().type == TokenType::DEDENT || peek().type == TokenType::CLOSEBRACE) {
      return nullptr;
   }

   Token p = peek(); // now capture the first non-newline/non-indent token

   if (p.type == TokenType::KAZI) {
      consume(); // consume 'kazi'
      return parse_function_declaration();
   }
   if (p.type == TokenType::RUDISHA) {
      consume(); // consume 'rudisha'
      return parse_return_statement();
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

std::unique_ptr < StatementNode > Parser::parse_variable_declaration() {
   bool is_constant = false;

   if (peek().type == TokenType::CONSTANT) {
      consume();
      is_constant = true;
   }

   expect(TokenType::IDENTIFIER, "Expected identifier after 'data'");
   Token idTok = tokens[position - 1];
   std::string name = idTok.value;

   std::unique_ptr < ExpressionNode > value = nullptr;

   if (is_constant) {
      // force an initializer for constants
      expect(TokenType::ASSIGN, "Constant '" + name + "' must be initialized");
      value = parse_expression();
   } else {
      // non-constants: assignment is optional
      if (peek().type == TokenType::ASSIGN) {
         consume();
         value = parse_expression();
      }
   }

   if (peek().type == TokenType::SEMICOLON) consume();

   auto node = std::make_unique < VariableDeclarationNode > ();
   node->identifier = name;
   node->value = std::move(value);
   node->is_constant = is_constant;
   node->token = idTok;
   return node;
}

std::unique_ptr < StatementNode > Parser::parse_print_statement(bool newline) {
   // capture the keyword token (CHAPISHA / ANDIKA) which was consumed by caller
   Token kwTok = tokens[position - 1];

   std::vector < std::unique_ptr < ExpressionNode>> args;
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
   auto node = std::make_unique < PrintStatementNode > ();
   node->expressions = std::move(args);
   node->newline = newline;
   node->token = kwTok;
   return node;
}

std::unique_ptr < StatementNode > Parser::parse_assignment_or_expression_statement() {
   if (peek().type == TokenType::IDENTIFIER) {
      Token idTok = consume();
      std::string name = idTok.value;

      if (peek().type == TokenType::ASSIGN) {
         // normal assignment
         consume(); // '='
         auto value = parse_expression();
         if (peek().type == TokenType::SEMICOLON) consume();
         auto node = std::make_unique < AssignmentNode > ();
         node->identifier = name;
         node->value = std::move(value);
         node->token = idTok;
         return node;
      }
      else if (peek().type == TokenType::PLUS_ASSIGN || peek().type == TokenType::MINUS_ASSIGN) {
         Token opTok = consume(); // += or -=
         auto right = parse_expression();

         // build BinaryExpressionNode: x + right OR x - right
         auto bin = std::make_unique < BinaryExpressionNode > ();
         bin->op = (opTok.type == TokenType::PLUS_ASSIGN) ? "+": "-";
         auto leftIdent = std::make_unique < IdentifierNode > ();
         leftIdent->name = name;
         leftIdent->token = idTok;
         bin->left = std::move(leftIdent);
         bin->right = std::move(right);
         bin->token = opTok;

         auto assign = std::make_unique < AssignmentNode > ();
         assign->identifier = name;
         assign->value = std::move(bin);
         assign->token = idTok;
         if (peek().type == TokenType::SEMICOLON) consume();
         return assign;
      }
      else if (peek().type == TokenType::INCREMENT || peek().type == TokenType::DECREMENT) {
         Token opTok = consume(); // ++ or --

         // build BinaryExpressionNode: x + 1 OR x - 1
         auto bin = std::make_unique < BinaryExpressionNode > ();
         bin->op = (opTok.type == TokenType::INCREMENT) ? "+": "-";
         auto leftIdent = std::make_unique < IdentifierNode > ();
         leftIdent->name = name;
         leftIdent->token = idTok;
         bin->left = std::move(leftIdent);

         auto one = std::make_unique < NumericLiteralNode > ();
         one->value = 1;
         one->token = opTok; // reuse operator token
         bin->right = std::move(one);
         bin->token = opTok;

         auto assign = std::make_unique < AssignmentNode > ();
         assign->identifier = name;
         assign->value = std::move(bin);
         assign->token = idTok;
         if (peek().type == TokenType::SEMICOLON) consume();
         return assign;
      }
      else {
         // Could be call or identifier-expression statement
         auto ident = std::make_unique < IdentifierNode > ();
         ident->name = name;
         ident->token = idTok;
         if (peek().type == TokenType::OPENPARENTHESIS) {
            auto call = parse_call(std::move(ident));
            auto stmt = std::make_unique < ExpressionStatementNode > ();
            stmt->expression = std::move(call);
            if (peek().type == TokenType::SEMICOLON) consume();
            return stmt;
         } else {
            auto stmt = std::make_unique < ExpressionStatementNode > ();
            stmt->expression = std::move(ident);
            if (peek().type == TokenType::SEMICOLON) consume();
            return stmt;
         }
      }
   }

   // fallback: expression statement
   auto expr = parse_expression();
   if (peek().type == TokenType::SEMICOLON) consume();
   auto stmt = std::make_unique < ExpressionStatementNode > ();
   stmt->expression = std::move(expr);
   return stmt;
}

std::unique_ptr < StatementNode > Parser::parse_function_declaration() {
   // The 'kazi' token was already consumed by parse_statement

   expect(TokenType::IDENTIFIER, "Expected function name after 'kazi'");
   Token idTok = tokens[position - 1];

   auto funcNode = std::make_unique < FunctionDeclarationNode > ();
   funcNode->name = idTok.value;
   funcNode->token = idTok;

   // Parse parameters
   while (peek().type == TokenType::IDENTIFIER) {
      funcNode->parameters.push_back(consume().value);
      if (!match(TokenType::COMMA)) {
         break; // No more commas, so parameter list is done
      }
   }

   // Now, branch on the token that starts the function body
   if (match(TokenType::COLON)) {
      // --- Indentation-based body ---
      expect(TokenType::NEWLINE, "Expected newline after ':' in function declaration");
      expect(TokenType::INDENT, "Expected indented block for function body");

      // indentation-based body (defensive: parse_statement may return null at EOF)
      while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
         auto stmt = parse_statement();
         if (!stmt) break; // EOF or nothing more
         funcNode->body.push_back(std::move(stmt));
      }

      expect(TokenType::DEDENT, "Expected dedent to close function body");

   } else if (match(TokenType::OPENBRACE)) {
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

std::unique_ptr < StatementNode > Parser::parse_return_statement() {
   // The 'rudisha' token was already consumed by parse_statement
   Token kwTok = tokens[position - 1];

   auto retNode = std::make_unique < ReturnStatementNode > ();
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

// ---------- control-flow: if / else ----------
std::unique_ptr < StatementNode > Parser::parse_if_statement() {
   // caller has not consumed 'kama' yet in some flows, but in our parse_statement
   // we check p.type == KAMA and call this function; consume here to capture token.
   consume(); // consume 'kama'
   Token ifTok = tokens[position - 1];

   // parse condition expression
   auto cond = parse_expression();

   auto ifNode = std::make_unique < IfStatementNode > ();
   ifNode->token = ifTok;
   ifNode->condition = std::move(cond);

   // then-block: colon (indent) or brace style
   if (match(TokenType::COLON)) {
      expect(TokenType::NEWLINE, "Expected newline after ':' in 'kama' statement");
      // parse indented block
      auto thenBody = parse_block(false);
      ifNode->then_body = std::move(thenBody);
   } else if (match(TokenType::OPENBRACE)) {
      // we consumed '{' in parse_block, but parse_block expects to see it,
      // so call parse_block(true) which consumes '{' itself. Since we already matched,
      // we push back a fake OPENBRACE by adjusting position back one and let parse_block handle it.
      position--; // rewind one token so parse_block sees OPENBRACE
      auto thenBody = parse_block(true);
      ifNode->then_body = std::move(thenBody);
   } else {
      expect(TokenType::COLON, "Expected ':' or '{' to begin 'kama' body");
   }

   // optional else (vinginevyo)
   if (peek().type == TokenType::VINGINEVYO) {
      consume();
      ifNode->has_else = true;

      // --- Support "vinginevyo kama ..." (else if) by parsing a nested if and
      // inserting it as the single statement in the else_body. This keeps the
      // AST simple (nested IfStatementNode) and matches typical else-if semantics.
      if (peek().type == TokenType::KAMA) {
         auto nestedIf = parse_if_statement(); // consumes the 'kama' and builds the nested If
         ifNode->else_body.clear();
         ifNode->else_body.push_back(std::move(nestedIf));
      }
      else if (match(TokenType::COLON)) {
         expect(TokenType::NEWLINE, "Expected newline after ':' in 'vinginevyo' (else)");
         auto elseBody = parse_block(false);
         ifNode->else_body = std::move(elseBody);
      } else if (match(TokenType::OPENBRACE)) {
         position--;
         auto elseBody = parse_block(true);
         ifNode->else_body = std::move(elseBody);
      } else {
         expect(TokenType::COLON, "Expected ':' or '{' to begin 'vinginevyo' body");
      }
   }
   return ifNode;
}


// ---------- loops: for / while / do-while ----------

std::unique_ptr < StatementNode > Parser::parse_for_statement() {
   // consume 'kwa' and capture token for diagnostics
   consume();
   Token forTok = tokens[position - 1];

   // expect '('
   expect(TokenType::OPENPARENTHESIS, "Expected '(' after 'kwa'");

   // --- init clause (optional) ---
   std::unique_ptr < StatementNode > initStmt = nullptr;
   if (peek().type != TokenType::SEMICOLON) {
      if (peek().type == TokenType::DATA) {
         // consume 'data' and parse a variable declaration (we consumed DATA here)
         consume();
         initStmt = parse_variable_declaration();
      } else {
         // parse a small assignment/expression statement for the init.
         // We reuse similar logic to parse_assignment_or_expression_statement but
         // avoid consuming a trailing semicolon here (we'll consume it below).
         if (peek().type == TokenType::IDENTIFIER) {
            Token idTok = consume();
            std::string name = idTok.value;

            if (peek().type == TokenType::ASSIGN) {
               consume();
               auto value = parse_expression();
               auto node = std::make_unique < AssignmentNode > ();
               node->identifier = name;
               node->value = std::move(value);
               node->token = idTok;
               initStmt = std::move(node);
            } else if (peek().type == TokenType::PLUS_ASSIGN || peek().type == TokenType::MINUS_ASSIGN) {
               Token opTok = consume();
               auto right = parse_expression();
               auto bin = std::make_unique < BinaryExpressionNode > ();
               bin->op = (opTok.type == TokenType::PLUS_ASSIGN) ? "+": "-";
               auto leftIdent = std::make_unique < IdentifierNode > ();
               leftIdent->name = name;
               leftIdent->token = idTok;
               bin->left = std::move(leftIdent);
               bin->right = std::move(right);
               bin->token = opTok;
               auto assign = std::make_unique < AssignmentNode > ();
               assign->identifier = name;
               assign->value = std::move(bin);
               assign->token = idTok;
               initStmt = std::move(assign);
            } else if (peek().type == TokenType::INCREMENT || peek().type == TokenType::DECREMENT) {
               Token opTok = consume();
               auto bin = std::make_unique < BinaryExpressionNode > ();
               bin->op = (opTok.type == TokenType::INCREMENT) ? "+": "-";
               auto leftIdent = std::make_unique < IdentifierNode > ();
               leftIdent->name = name;
               leftIdent->token = idTok;
               bin->left = std::move(leftIdent);
               auto one = std::make_unique < NumericLiteralNode > ();
               one->value = 1;
               one->token = opTok;
               bin->right = std::move(one);
               bin->token = opTok;
               auto assign = std::make_unique < AssignmentNode > ();
               assign->identifier = name;
               assign->value = std::move(bin);
               assign->token = idTok;
               initStmt = std::move(assign);
            } else {
               // function call or bare identifier as expression statement
               auto ident = std::make_unique < IdentifierNode > ();
               ident->name = name;
               ident->token = idTok;
               if (peek().type == TokenType::OPENPARENTHESIS) {
                  auto call = parse_call(std::move(ident));
                  auto stmt = std::make_unique < ExpressionStatementNode > ();
                  stmt->expression = std::move(call);
                  initStmt = std::move(stmt);
               } else {
                  auto stmt = std::make_unique < ExpressionStatementNode > ();
                  stmt->expression = std::move(ident);
                  initStmt = std::move(stmt);
               }
            }
         } else {
            // fallback: allow any expression as init
            auto expr = parse_expression();
            auto stmt = std::make_unique < ExpressionStatementNode > ();
            stmt->expression = std::move(expr);
            initStmt = std::move(stmt);
         }
      }
   }

   // consume the semicolon after init
   expect(TokenType::SEMICOLON, "Expected ';' after for-loop init");

   // --- condition (optional) ---
   std::unique_ptr < ExpressionNode > condExpr = nullptr;
   if (peek().type != TokenType::SEMICOLON) {
      condExpr = parse_expression();
   }

   expect(TokenType::SEMICOLON, "Expected ';' after for-loop condition");

   // --- post expression (optional) ---
   std::unique_ptr < ExpressionNode > postExpr = nullptr;
   if (peek().type != TokenType::CLOSEPARENTHESIS) {
      // support a simple "a++" / "a--" as post as well as general expressions
      if (peek().type == TokenType::IDENTIFIER) {
         Token idTok = consume();
         std::string name = idTok.value;
         if (peek().type == TokenType::INCREMENT || peek().type == TokenType::DECREMENT) {
            Token opTok = consume();
            auto bin = std::make_unique < BinaryExpressionNode > ();
            bin->op = (opTok.type == TokenType::INCREMENT) ? "+": "-";
            auto leftIdent = std::make_unique < IdentifierNode > ();
            leftIdent->name = name;
            leftIdent->token = idTok;
            bin->left = std::move(leftIdent);
            auto one = std::make_unique < NumericLiteralNode > ();
            one->value = 1;
            one->token = opTok;
            bin->right = std::move(one);
            bin->token = opTok;
            postExpr = std::move(bin);
         } else {
            // backtrack and parse as expression
            position--;
            postExpr = parse_expression();
         }
      } else {
         postExpr = parse_expression();
      }
   }

   expect(TokenType::CLOSEPARENTHESIS, "Expected ')' to close for-loop header");

   // build node
   auto node = std::make_unique < ForStatementNode > ();
   node->token = forTok;
   node->init = std::move(initStmt);
   node->condition = std::move(condExpr);
   node->post = std::move(postExpr);

   // parse body: colon/indent or brace style
   if (match(TokenType::COLON)) {
      expect(TokenType::NEWLINE, "Expected newline after ':' in 'kwa' statement");
      node->body = parse_block(false);
   } else if (match(TokenType::OPENBRACE)) {
      position--; // rewind so parse_block sees '{'
      node->body = parse_block(true);
   } else {
      expect(TokenType::COLON, "Expected ':' or '{' to begin 'kwa' body");
   }

   return node;
}

std::unique_ptr < StatementNode > Parser::parse_while_statement() {
   // consume 'wakati' and capture token for diagnostics
   consume();
   Token whileTok = tokens[position - 1];

   // parse condition expression
   auto cond = parse_expression();

   auto node = std::make_unique < WhileStatementNode > ();
   node->token = whileTok;
   node->condition = std::move(cond);

   // then-block: colon (indent) or brace style
   if (match(TokenType::COLON)) {
      expect(TokenType::NEWLINE, "Expected newline after ':' in 'wakati' statement");
      node->body = parse_block(false);
   } else if (match(TokenType::OPENBRACE)) {
      position--;
      node->body = parse_block(true);
   } else {
      expect(TokenType::COLON, "Expected ':' or '{' to begin 'wakati' body");
   }

   return node;
}

std::unique_ptr < StatementNode > Parser::parse_do_while_statement() {
   // consume 'fanya' and capture token
   consume();
   Token doTok = tokens[position - 1];

   auto node = std::make_unique < DoWhileStatementNode > ();
   node->token = doTok;

   // parse block first (either indent style or brace style)
   if (match(TokenType::COLON)) {
      expect(TokenType::NEWLINE, "Expected newline after ':' in 'fanya' (do) statement");
      node->body = parse_block(false);
   } else if (match(TokenType::OPENBRACE)) {
      position--;
      node->body = parse_block(true);
   } else {
      expect(TokenType::COLON, "Expected ':' or '{' to begin 'fanya' body");
   }

   // now expect 'wakati' and the condition
   if (!match(TokenType::WHILE)) {
      // 'wakati' token is required after the body
      expect(TokenType::WHILE, "Expected 'wakati' after 'fanya' block for do-while");
   }
   // parse condition expression
   node->condition = parse_expression();

   // optionally consume trailing semicolon
   if (peek().type == TokenType::SEMICOLON) consume();

   return node;
}


// ---------- expressions (precedence) ----------
std::unique_ptr < ExpressionNode > Parser::parse_expression() {
   return parse_ternary();
}

std::unique_ptr < ExpressionNode > Parser::parse_ternary() {
   auto cond = parse_logical_or();

  if (peek().type != TokenType::QUESTIONMARK) {
      return cond;
   }

  Token qTok = consume(); // consume '?'

  // helper: treat NEWLINE/INDENT/DEDENT as formatting around ternary parts
   auto skip_formatting = [&]() {
      while (peek().type == TokenType::NEWLINE ||
            peek().type == TokenType::INDENT ||
             peek().type == TokenType::DEDENT) {
        consume();
      }
  };

   // Allow formatting tokens before thenExpr
   skip_formatting();
   auto thenExpr = parse_ternary();

  // Allow formatting tokens before ':'
   skip_formatting();
   expect(TokenType::COLON, "Expected ':' after ternary 'then' expression");

  // Allow formatting tokens before elseExpr
  skip_formatting();
   auto elseExpr = parse_ternary();

   auto node = std::make_unique < TernaryExpressionNode > ();
   node->token = qTok;
  node->condition = std::move(cond);
   node->thenExpr = std::move(thenExpr);
  node->elseExpr = std::move(elseExpr);
  return node;
}


std::unique_ptr < ExpressionNode > Parser::parse_logical_or() {
   auto left = parse_logical_and();
   while (peek().type == TokenType::OR) {
      Token op = consume();
      auto right = parse_logical_and();
      auto node = std::make_unique < BinaryExpressionNode > ();
      node->op = !op.value.empty() ? op.value: "||";
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      left = std::move(node);
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_logical_and() {
   auto left = parse_equality();
   while (peek().type == TokenType::AND) {
      Token op = consume();
      auto right = parse_equality();
      auto node = std::make_unique < BinaryExpressionNode > ();
      node->op = !op.value.empty() ? op.value: "&&";
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      left = std::move(node);
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_equality() {
   auto left = parse_comparison();
   while (peek().type == TokenType::EQUALITY || peek().type == TokenType::NOTEQUAL) {
      Token op = consume();
      auto right = parse_comparison();
      auto node = std::make_unique < BinaryExpressionNode > ();
      if (!op.value.empty()) node->op = op.value;
      else node->op = (op.type == TokenType::EQUALITY) ? "==": "!=";
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      left = std::move(node);
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_comparison() {
   auto left = parse_additive();
   while (peek().type == TokenType::GREATERTHAN ||
      peek().type == TokenType::GREATEROREQUALTHAN ||
      peek().type == TokenType::LESSTHAN ||
      peek().type == TokenType::LESSOREQUALTHAN) {
      Token op = consume();
      auto right = parse_additive();
      auto node = std::make_unique < BinaryExpressionNode > ();
      node->op = !op.value.empty() ? op.value: std::string();
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      left = std::move(node);
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_additive() {
   auto left = parse_multiplicative();
   while (peek().type == TokenType::PLUS || peek().type == TokenType::MINUS) {
      Token op = consume();
      auto right = parse_multiplicative();
      auto node = std::make_unique < BinaryExpressionNode > ();
      node->op = !op.value.empty() ? op.value: (op.type == TokenType::PLUS ? "+": "-");
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      left = std::move(node);
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_multiplicative() {
   auto left = parse_exponent();
   while (peek().type == TokenType::STAR || peek().type == TokenType::SLASH || peek().type == TokenType::PERCENT) {
      Token op = consume();
      auto right = parse_exponent();
      auto node = std::make_unique < BinaryExpressionNode > ();
      if (!op.value.empty()) node->op = op.value;
      else {
         if (op.type == TokenType::STAR) node->op = "*";
         else if (op.type == TokenType::SLASH) node->op = "/";
         else node->op = "%";
      }
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      left = std::move(node);
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_exponent() {
   // right-associative exponent
   auto left = parse_unary();
   if (peek().type == TokenType::POWER) {
      Token op = consume();
      auto right = parse_exponent(); // right-associative
      auto node = std::make_unique < BinaryExpressionNode > ();
      node->op = !op.value.empty() ? op.value: "**";
      node->left = std::move(left);
      node->right = std::move(right);
      node->token = op;
      return node;
   }
   return left;
}

std::unique_ptr < ExpressionNode > Parser::parse_unary() {
   if (peek().type == TokenType::NOT || peek().type == TokenType::MINUS) {
      Token op = consume();
      auto operand = parse_unary();
      auto node = std::make_unique < UnaryExpressionNode > ();
      node->op = !op.value.empty() ? op.value: (op.type == TokenType::NOT ? "!": "-");
      node->operand = std::move(operand);
      node->token = op;
      return node;
   }
   return parse_primary();
}

std::unique_ptr < ExpressionNode > Parser::parse_primary() {
   Token t = peek();
   if (t.type == TokenType::NUMBER) {
      Token numTok = consume();
      auto n = std::make_unique < NumericLiteralNode > ();
      n->value = std::stod(numTok.value);
      n->token = numTok;
      return n;
   }
   if (t.type == TokenType::STRING) {
      Token s = consume();
      auto node = std::make_unique < StringLiteralNode > ();
      node->value = s.value;
      node->token = s;
      return node;
   }
   if (t.type == TokenType::BOOLEAN) {
      Token b = consume();
      auto node = std::make_unique < BooleanLiteralNode > ();
      node->value = (b.value == "kweli" || b.value == "true");
      node->token = b;
      return node;
   }
   if (t.type == TokenType::IDENTIFIER) {
      Token id = consume();
      auto ident = std::make_unique < IdentifierNode > ();
      ident->name = id.value;
      ident->token = id;
      if (peek().type == TokenType::OPENPARENTHESIS) {
         return parse_call(std::move(ident));
      }
      return ident;
   }
   if (t.type == TokenType::OPENPARENTHESIS) {
      consume();
      auto inner = parse_expression();
      expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after expression");
      return inner;
   }

   Token tok = peek();
   throw std::runtime_error(
      "Unexpected token '" + tok.value + "' at " + tok.loc.to_string()
   );
}

std::unique_ptr < ExpressionNode > Parser::parse_call(std::unique_ptr < ExpressionNode > callee) {
   expect(TokenType::OPENPARENTHESIS, "Expected '(' in call");
   Token openTok = tokens[position - 1]; // the '(' token
   auto call = std::make_unique < CallExpressionNode > ();
   call->callee = std::move(callee);
   call->token = openTok;
   if (peek().type != TokenType::CLOSEPARENTHESIS) {
      do {
         call->arguments.push_back(parse_expression());
      } while (match(TokenType::COMMA));
   }
   expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after call arguments");
   return call;
}