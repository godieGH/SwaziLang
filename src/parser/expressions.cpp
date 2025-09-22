// src/parser/expressions.cpp
#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <sstream>

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