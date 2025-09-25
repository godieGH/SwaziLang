#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <sstream>

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

               // assignment target is an IdentifierNode now
               auto targetIdent = std::make_unique<IdentifierNode>();
               targetIdent->name = name;
               targetIdent->token = idTok;
               node->target = std::move(targetIdent);

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

               // target identifier for assignment
               auto targetIdent = std::make_unique<IdentifierNode>();
               targetIdent->name = name;
               targetIdent->token = idTok;
               assign->target = std::move(targetIdent);

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

               // target identifier for assignment
               auto targetIdent = std::make_unique<IdentifierNode>();
               targetIdent->name = name;
               targetIdent->token = idTok;
               assign->target = std::move(targetIdent);

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