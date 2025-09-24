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

std::unique_ptr<ExpressionNode> Parser::parse_template_literal() {
   // This function supports two lexer styles:
   // 1) Simple lexer: emits one TEMPLATE_STRING token containing the whole content
   //    -> we produce TemplateLiteralNode with quasis = { value } and no expressions.
   // 2) Full interpolation lexer: emits a sequence like:
   //    TEMPLATE_CHUNK("Hello "), TEMPLATE_EXPR_START, ...expr tokens..., TEMPLATE_EXPR_END, TEMPLATE_CHUNK(", world"), ... , TEMPLATE_END
   //    -> we consume the chunks/exprs and produce quasis+expressions accordingly.
   auto node = std::make_unique<TemplateLiteralNode>();

   Token t = peek();
   if (t.type == TokenType::TEMPLATE_STRING) {
      Token tok = consume();
      node->quasis.push_back(tok.value);
      node->token = tok;
      return node;
   }

   // expect the first chunk (lexer should emit TEMPLATE_CHUNK even if empty)
   if (t.type != TokenType::TEMPLATE_CHUNK) {
      throw std::runtime_error("Expected template chunk or template-string at " + t.loc.to_string());
   }

   // first chunk
   Token chunkTok = consume();
   node->quasis.push_back(chunkTok.value);
   node->token = chunkTok;

   // now loop: while next is TEMPLATE_EXPR_START parse expression, then expect a following CHUNK (or end)
   while (peek().type == TokenType::TEMPLATE_EXPR_START) {
      consume(); // consume TEMPLATE_EXPR_START (the "${")
      // parse the embedded expression
      auto expr = parse_expression();
      node->expressions.push_back(std::move(expr));

      // accept either a dedicated TEMPLATE_EXPR_END or a plain CLOSEBRACE (fallback)
      if (peek().type == TokenType::TEMPLATE_EXPR_END) {
         consume();
      } else if (peek().type == TokenType::CLOSEBRACE) {
         consume();
      } else {
         Token bad = peek();
         throw std::runtime_error("Expected '}' to close template expression at " + bad.loc.to_string());
      }

      // after closing the expression, the lexer should provide the next chunk (possibly empty),
      // or the TEMPLATE_END if the template ended right after the expression.
      if (peek().type == TokenType::TEMPLATE_CHUNK) {
         Token nextChunk = consume();
         node->quasis.push_back(nextChunk.value);
      } else if (peek().type == TokenType::TEMPLATE_END) {
         // end: push an empty final chunk so quasis.size() == expressions.size()+1
         node->quasis.push_back("");
         consume(); // consume TEMPLATE_END
         break;
      } else {
         Token bad = peek();
         throw std::runtime_error("Expected template chunk or end after interpolation at " + bad.loc.to_string());
      }
   }

   // If loop ended without seeing TEMPLATE_END, allow a final TEMPLATE_END now (some lexers may emit it)
   if (peek().type == TokenType::TEMPLATE_END) {
      consume();
   }

   // Ensure invariant: quasis.size() == expressions.size() + 1
   if (node->quasis.size() != node->expressions.size() + 1) {
      // normalize by appending empty chunk if needed
      while (node->quasis.size() < node->expressions.size() + 1) node->quasis.push_back("");
   }

   return node;
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

   // accept both double-quoted and single-quoted strings
   if (t.type == TokenType::STRING || t.type == TokenType::SINGLE_QUOTED_STRING) {
      Token s = consume();
      auto node = std::make_unique < StringLiteralNode > ();
      node->value = s.value;
      node->token = s;
      return node;
   }

   // template literals: either a single TEMPLATE_STRING token (no interpolation)
   // or interpolation-aware sequence starting with TEMPLATE_CHUNK
   if (t.type == TokenType::TEMPLATE_STRING || t.type == TokenType::TEMPLATE_CHUNK) {
      return parse_template_literal();
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


