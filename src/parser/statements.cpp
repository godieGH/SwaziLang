// src/parser/statements.cpp
#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <sstream>

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
