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

// Parse an assignment or expression statement starting with an identifier (or general expression fallback).
// This version builds full postfix expressions (member, index, calls) from the initial identifier,
// so forms like arr[0] = 1 and arr.ongeza(4) parse correctly. For compound ops (+=, ++/--)
// we only support them when the target is a simple IdentifierNode (preserves previous semantics).
// Parse an assignment or expression statement (updated to avoid moving the same unique_ptr twice
// and to allow assignable L-values beyond plain identifiers: Identifier, Member, Index).
std::unique_ptr < StatementNode > Parser::parse_assignment_or_expression_statement() {
   if (peek().type == TokenType::IDENTIFIER || peek().type == TokenType::SELF) {
      Token idTok = consume();

      std::unique_ptr < ExpressionNode > nodeExpr;
      if (idTok.type == TokenType::SELF) {
         nodeExpr = std::make_unique < ThisExpressionNode > ();
         static_cast<ThisExpressionNode*>(nodeExpr.get())->token = idTok;
      } else {
         auto ident = std::make_unique < IdentifierNode > ();
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
            Token dotTok = consume(); // consume '.'
            expect(TokenType::IDENTIFIER, "Expected identifier after '.'");
            Token propTok = tokens[position - 1];
            auto mem = std::make_unique < MemberExpressionNode > ();
            mem->object = std::move(nodeExpr);
            mem->property = propTok.value;
            mem->token = dotTok;
            nodeExpr = std::move(mem);
            continue;
         }
         if (peek().type == TokenType::OPENBRACKET) {
            Token openIdx = consume(); // consume '['
            auto idxExpr = parse_expression();
            expect(TokenType::CLOSEBRACKET, "Expected ']' after index expression");
            auto idxNode = std::make_unique < IndexExpressionNode > ();
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
            throw std::runtime_error("Invalid assignment target at " + opTok.loc.to_string());
         }
         consume(); // '='
         auto value = parse_expression();
         if (peek().type == TokenType::SEMICOLON) consume();
         auto assign = std::make_unique < AssignmentNode > ();
         assign->target = std::move(nodeExpr);
         assign->value = std::move(value);
         assign->token = idTok;
         return assign;
      }


      // Support += / -= for assignable L-values (Identifier, Member, Index)
      if (peek().type == TokenType::PLUS_ASSIGN || peek().type == TokenType::MINUS_ASSIGN) {
         if (!is_assignable(nodeExpr.get())) {
            Token opTok = peek();
            throw std::runtime_error("Compound assignment is only supported for assignable targets at " + opTok.loc.to_string());
         }
         Token opTok = consume(); // += or -=
         auto right = parse_expression();

         // build BinaryExpressionNode: left + right OR left - right
         auto bin = std::make_unique < BinaryExpressionNode > ();
         bin->op = (opTok.type == TokenType::PLUS_ASSIGN) ? "+": "-";
         // clone the left for the computed expression so we don't lose ownership of the original
         bin->left = nodeExpr->clone();
         bin->right = std::move(right);
         bin->token = opTok;

         auto assign = std::make_unique < AssignmentNode > ();
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
            throw std::runtime_error("Increment/decrement is only supported for assignable targets at " + opTok.loc.to_string());
         }
         Token opTok = consume();
         // build BinaryExpressionNode: left + 1 OR left - 1
         auto bin = std::make_unique < BinaryExpressionNode > ();
         bin->op = (opTok.type == TokenType::INCREMENT) ? "+": "-";
         // clone left for computation, keep original to move into the assignment target
         bin->left = nodeExpr->clone();
         auto one = std::make_unique < NumericLiteralNode > ();
         one->value = 1;
         one->token = opTok;
         bin->right = std::move(one);
         bin->token = opTok;

         auto assign = std::make_unique < AssignmentNode > ();
         // assign target is the original nodeExpr (moved from here)
         assign->target = std::move(nodeExpr);
         assign->value = std::move(bin);
         assign->token = idTok;
         if (peek().type == TokenType::SEMICOLON) consume();
         return assign;
      }

      // Otherwise it's an expression statement: return the fully-expanded expression (call/member/index)
      if (peek().type == TokenType::SEMICOLON) consume();
      auto stmt = std::make_unique < ExpressionStatementNode > ();
      stmt->expression = std::move(nodeExpr);
      return stmt;
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
