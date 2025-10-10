#include "parser.hpp"
#include <iostream>
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
   while (peek().type == TokenType::EQUALITY ||
          peek().type == TokenType::NOTEQUAL ||
          peek().type == TokenType::STRICT_EQUALITY ||
          peek().type == TokenType::STRICT_NOTEQUAL) {
      Token op = consume();
      auto right = parse_comparison();
      auto node = std::make_unique < BinaryExpressionNode > ();
      if (!op.value.empty()) node->op = op.value;
      else {
         // assign textual operator string based on token type
         if (op.type == TokenType::EQUALITY) node->op = "==";
         else if (op.type == TokenType::NOTEQUAL) node->op = "!=";
         else if (op.type == TokenType::STRICT_EQUALITY) node->op = "===";
         else /* STRICT_NOTEQUAL */ node->op = "!==";
      }
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

   // parse primary expression first
   auto node = parse_primary();

   // Postfix loop: handle calls, member access (obj.prop), and indexing (obj[index])
   while (true) {
      if (peek().type == TokenType::OPENPARENTHESIS) {
         // call: convert current node into a CallExpressionNode via parse_call
         node = parse_call(std::move(node));
         continue;
      }
      if (peek().type == TokenType::DOT) {
         Token dotTok = consume(); // consume '.'
         // expect identifier after dot
         expect(TokenType::IDENTIFIER, "Expected identifier after '.'");
         Token propTok = tokens[position - 1];
         auto mem = std::make_unique < MemberExpressionNode > ();
         mem->object = std::move(node);
         mem->property = propTok.value;
         mem->token = dotTok;
         node = std::move(mem);
         continue;
      }
      if (peek().type == TokenType::OPENBRACKET) {
         Token openIdx = consume(); // consume '['
         auto idxExpr = parse_expression();
         expect(TokenType::CLOSEBRACKET, "Expected ']' after index expression");
         auto idxNode = std::make_unique < IndexExpressionNode > ();
         idxNode->object = std::move(node);
         idxNode->index = std::move(idxExpr);
         idxNode->token = openIdx;
         node = std::move(idxNode);
         continue;
      }
      break;
   }

   return node;
}

std::unique_ptr < ExpressionNode > Parser::parse_template_literal() {
   // This function supports two lexer styles:
   // 1) Simple lexer: emits one TEMPLATE_STRING token containing the whole content
   //    -> we produce TemplateLiteralNode with quasis = { value } and no expressions.
   // 2) Full interpolation lexer: emits a sequence like:
   //    TEMPLATE_CHUNK("Hello "), TEMPLATE_EXPR_START, ...expr tokens..., TEMPLATE_EXPR_END, TEMPLATE_CHUNK(", world"), ... , TEMPLATE_END
   //    -> we consume the chunks/exprs and produce quasis+expressions accordingly.
   auto node = std::make_unique < TemplateLiteralNode > ();

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



std::unique_ptr < ExpressionNode > Parser::parse_tabia_method() {
   // Accept either a dedicated TABIA token or IDENTIFIER "tabia"
   Token startTok;
   if (peek().type == TokenType::TABIA) {
      startTok = consume();
   } else if (peek().type == TokenType::IDENTIFIER && peek().value == "tabia") {
      startTok = consume();
   } else {
      throw std::runtime_error("parse_tabia_method called without 'tabia' at " + peek().loc.to_string());
   }

   // optional 'thabiti' keyword -> getter flag (allow either dedicated token or identifier)
   bool is_getter = false;
   if (peek().type == TokenType::CONSTANT ||
      (peek().type == TokenType::IDENTIFIER && peek().value == "thabiti")) {
      consume();
      is_getter = true;
   }

   // method name: must be identifier
   expect(TokenType::IDENTIFIER, "Expected method name after 'tabia'");
   Token nameTok = tokens[position - 1];

   // parse parameters into ParameterNode descriptors: either parenthesized or inline identifiers
   std::vector<std::unique_ptr<ParameterNode>> params;
   // allow optional formatting before parameters
   auto skip_formatting_local = [&]() {
      while (peek().type == TokenType::NEWLINE ||
         peek().type == TokenType::INDENT ||
         peek().type == TokenType::DEDENT) {
         consume();
      }
   };
   skip_formatting_local();

   bool rest_seen = false;

   if (match(TokenType::OPENPARENTHESIS)) {
      // parenthesized params
      skip_formatting_local();
      if (peek().type != TokenType::CLOSEPARENTHESIS) {
         while (true) {
            // rest param
            if (peek().type == TokenType::ELLIPSIS) {
               Token ellTok = consume();
               if (rest_seen) {
                  throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed");
               }
               expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
               Token id = tokens[position - 1];

               auto p = std::make_unique<ParameterNode>();
               p->token = ellTok;
               p->name = id.value;
               p->is_rest = true;
               p->rest_required_count = 0;

               // optional [...][NUMBER]
               if (peek().type == TokenType::OPENBRACKET) {
                  consume(); // '['
                  expect(TokenType::NUMBER, "Expected number inside rest count brackets");
                  Token numTok = tokens[position - 1];
                  try {
                     p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
                  } catch (...) {
                     throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string());
                  }
                  expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
               }

               params.push_back(std::move(p));
               rest_seen = true;

               // rest must be last (allow trailing comma only if closing paren next)
               if (peek().type == TokenType::COMMA) {
                  // if comma is followed by something other than ')' it's an error
                  if (peek_next().type != TokenType::CLOSEPARENTHESIS) {
                     Token bad = tokens[position];
                     throw std::runtime_error("Rest parameter must be the last parameter at " + bad.loc.to_string());
                  }
                  // allow trailing comma then break
               }

               // done with param list
               break;
            }

            // normal identifier param (with optional default)
            expect(TokenType::IDENTIFIER, "Expected identifier in parameter list");
            Token pTok = tokens[position - 1];
            auto pnode = std::make_unique<ParameterNode>();
            pnode->token = pTok;
            pnode->name = pTok.value;
            pnode->is_rest = false;
            pnode->rest_required_count = 0;
            pnode->defaultValue = nullptr;

            // optional default initializer: '=' expression
            if (peek().type == TokenType::ASSIGN) {
               consume(); // consume '='
               pnode->defaultValue = parse_expression();
               if (!pnode->defaultValue) {
                  throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position-1].loc.to_string());
               }
            }

            params.push_back(std::move(pnode));

            if (match(TokenType::COMMA)) {
               skip_formatting_local();
               if (peek().type == TokenType::CLOSEPARENTHESIS) {
                  // allow trailing comma
                  break;
               }
               continue;
            }
            break;
         }
      }
      expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after parameter list");
   } else {
      // legacy / compact form: zero or more identifiers separated by commas; support rest and defaults inline
      while (peek().type == TokenType::IDENTIFIER || peek().type == TokenType::ELLIPSIS) {
         if (peek().type == TokenType::ELLIPSIS) {
            Token ellTok = consume();
            if (rest_seen) {
               throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed");
            }
            expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
            Token id = tokens[position - 1];

            auto p = std::make_unique<ParameterNode>();
            p->token = ellTok;
            p->name = id.value;
            p->is_rest = true;
            p->rest_required_count = 0;

            if (peek().type == TokenType::OPENBRACKET) {
               consume(); // '['
               expect(TokenType::NUMBER, "Expected number inside rest count brackets");
               Token numTok = tokens[position - 1];
               try {
                  p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
               } catch (...) {
                  throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string());
               }
               expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
            }

            params.push_back(std::move(p));
            rest_seen = true;

            // rest must be last in bare form
            if (peek().type == TokenType::COMMA) {
               Token c = tokens[position]; // lookahead
               throw std::runtime_error("Rest parameter must be the last parameter at " + c.loc.to_string());
            }
            break;
         }

         // normal identifier
         Token idTok = consume();
         if (idTok.type != TokenType::IDENTIFIER) {
            throw std::runtime_error("Expected parameter name at " + idTok.loc.to_string());
         }
         auto pnode = std::make_unique<ParameterNode>();
         pnode->token = idTok;
         pnode->name = idTok.value;
         pnode->is_rest = false;
         pnode->rest_required_count = 0;
         pnode->defaultValue = nullptr;

         // optional default initializer: '=' expression (allow in bare form for consistency)
         if (peek().type == TokenType::ASSIGN) {
            consume(); // consume '='
            pnode->defaultValue = parse_expression();
            if (!pnode->defaultValue) {
               throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position-1].loc.to_string());
            }
         }

         params.push_back(std::move(pnode));

         if (!match(TokenType::COMMA)) break;
      }
   }

   // skip formatting before checking body
   skip_formatting_local();

   // parse body (either colon + indented block, or brace block)
   std::vector < std::unique_ptr < StatementNode>> bodyStmts;

   if (match(TokenType::COLON)) {
      // indentation-based body
      expect(TokenType::NEWLINE, "Expected newline after ':' in tabia method");
      expect(TokenType::INDENT, "Expected INDENT for tabia method body");

      while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
         auto stmt = parse_statement();
         if (!stmt) break;
         bodyStmts.push_back(std::move(stmt));
      }
      expect(TokenType::DEDENT, "Expected DEDENT to close tabia method body");

   } else if (match(TokenType::OPENBRACE)) {
      // brace-based body
      while (peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::EOF_TOKEN) {
         // skip formatting tokens between statements
         while (peek().type == TokenType::NEWLINE ||
            peek().type == TokenType::INDENT ||
            peek().type == TokenType::DEDENT) {
            consume();
         }
         if (peek().type == TokenType::CLOSEBRACE || peek().type == TokenType::EOF_TOKEN) break;
         auto stmt = parse_statement();
         if (!stmt) break;
         bodyStmts.push_back(std::move(stmt));
      }
      expect(TokenType::CLOSEBRACE, "Expected '}' to close tabia method body");
   } else {
      expect(TokenType::COLON, "Expected ':' or '{' to begin tabia method body");
   }

   if (is_getter && !params.empty()) {
      throw std::runtime_error("'thabiti' method cannot take parameters at " + nameTok.loc.to_string());
   }

   // Build the FunctionExpressionNode
   auto func = std::make_unique < FunctionExpressionNode > ();
   func->token = startTok;
   func->name = nameTok.value;
   func->parameters = std::move(params);
   func->body = std::move(bodyStmts);
   func->is_getter = is_getter;

   return func;
}
std::unique_ptr < ExpressionNode > Parser::parse_object_expression() {
   expect(TokenType::OPENBRACE, "Expected '{' to start object literal");
   Token openTok = tokens[position - 1];

   auto obj = std::make_unique < ObjectExpressionNode > ();
   obj->token = openTok;

   auto skip_formatting = [&]() {
      while (peek().type == TokenType::NEWLINE ||
         peek().type == TokenType::INDENT ||
         peek().type == TokenType::DEDENT) {
         consume();
      }
   };

   skip_formatting();

   // empty object {}
   if (peek().type == TokenType::CLOSEBRACE) {
      consume();
      return obj;
   }

   while (peek().type != TokenType::CLOSEBRACE) {
      skip_formatting();

      bool is_private_flag = false;
      bool is_locked_flag = false;
      Token privateTok,
      lockedTok;

      while (true) {
         if (peek().type == TokenType::AT_SIGN) {
            privateTok = consume();
            is_private_flag = true;
            skip_formatting();
         } else if (peek().type == TokenType::AMPERSAND) {
            lockedTok = consume();
            is_locked_flag = true;
            skip_formatting();
         } else {
            break; // no more modifiers
         }
      }

      // --- special: tabia method inside object ---
      if ((peek().type == TokenType::TABIA) ||
         (peek().type == TokenType::IDENTIFIER && peek().value == "tabia")) {
         // parse method expression (consumes the tabia token and body)
         auto methodExpr = parse_tabia_method();

         // move into PropertyNode
         auto prop = std::make_unique < PropertyNode > ();
         prop->kind = PropertyKind::Method;
         prop->value = std::move(methodExpr);

         // extract name and getter flag if possible
         if (auto func = dynamic_cast<FunctionExpressionNode*>(prop->value.get())) {
            prop->key_name = func->name;
            prop->is_readonly = func->is_getter;
            prop->token = func->token;
         }

         // apply privacy marker if present
         prop->is_private = is_private_flag;
         prop->is_locked = is_locked_flag;

         obj->properties.push_back(std::move(prop));

         skip_formatting();
         if (peek().type == TokenType::COMMA) consume();
         continue;
      }

      // --- non-tabia property path (existing logic) ---
      if (peek().type == TokenType::ELLIPSIS) {
         if (is_private_flag) {
            throw std::runtime_error("Private modifier '@' cannot be applied to spread at " + privateTok.loc.to_string());
         }
         Token ell = consume();
         auto spread = std::make_unique < SpreadElementNode > ();
         spread->token = ell;
         spread->argument = parse_expression();
         auto prop = std::make_unique < PropertyNode > ();
         prop->token = ell;
         prop->kind = PropertyKind::Spread;
         prop->value = std::move(spread);
         obj->properties.push_back(std::move(prop));
         skip_formatting();
         if (peek().type == TokenType::COMMA) consume();
         continue;
      }

      auto prop = std::make_unique < PropertyNode > ();
      prop->kind = PropertyKind::KeyValue;
      prop->is_private = is_private_flag; // apply privacy to property node
      prop->is_locked = is_locked_flag; // apply lock to property node

      // key: identifier, string, number, or computed [expr]
      if (peek().type == TokenType::OPENBRACKET) {
         Token openIdx = consume(); // '['
         prop->computed = true;
         prop->key = parse_expression();
         expect(TokenType::CLOSEBRACKET, "Expected ']' after computed property key");
         prop->token = openIdx;
      } else {
         Token t = peek();
         if (t.type == TokenType::IDENTIFIER) {
            Token idTok = consume();
            prop->key_name = idTok.value;
            prop->token = idTok;
         } else if (t.type == TokenType::STRING || t.type == TokenType::SINGLE_QUOTED_STRING) {
            Token s = consume();
            auto keyNode = std::make_unique < StringLiteralNode > ();
            keyNode->value = s.value;
            keyNode->token = s;
            prop->key = std::move(keyNode);
            prop->token = s;
         } else if (t.type == TokenType::NUMBER) {
            Token n = consume();
            auto keyNode = std::make_unique < NumericLiteralNode > ();
            keyNode->value = std::stod(n.value);
            keyNode->token = n;
            prop->key = std::move(keyNode);
            prop->token = n;
         } else {
            Token bad = peek();
            throw std::runtime_error("Unexpected token in object property key: '" + bad.value +
               "' at " + bad.loc.to_string());
         }
      }

      skip_formatting();

      // decide property kind & parse value
      if (peek().type == TokenType::COLON) {
         consume(); // ':'
         skip_formatting();
         prop->kind = PropertyKind::KeyValue;
         prop->value = parse_expression();
      } else if (peek().type == TokenType::OPENPARENTHESIS) {
         // method shorthand not supported; treat as shorthand (per your rules)
         prop->kind = PropertyKind::Shorthand;
         if (!prop->key_name.empty()) {
            Token fakeTok; fakeTok.type = TokenType::IDENTIFIER; fakeTok.value = prop->key_name;
            auto ident = std::make_unique < IdentifierNode > ();
            ident->name = prop->key_name;
            ident->token = fakeTok;
            prop->value = std::move(ident);
         } else if (prop->key) {
            prop->value = prop->key->clone();
         } else {
            throw std::runtime_error("Invalid property shorthand without identifier at " + peek().loc.to_string());
         }
      } else {
         // shorthand property
         prop->kind = PropertyKind::Shorthand;
         if (!prop->key_name.empty()) {
            Token fakeTok; fakeTok.type = TokenType::IDENTIFIER; fakeTok.value = prop->key_name;
            auto ident = std::make_unique < IdentifierNode > ();
            ident->name = prop->key_name;
            ident->token = fakeTok;
            prop->value = std::move(ident);
         } else if (prop->key) {
            prop->value = prop->key->clone();
         } else {
            throw std::runtime_error("Invalid property shorthand without identifier at " + peek().loc.to_string());
         }
      }

      obj->properties.push_back(std::move(prop));

      skip_formatting();
      if (peek().type == TokenType::COMMA) {
         consume();
         skip_formatting();
         if (peek().type == TokenType::CLOSEBRACE) break;
         continue;
      }

      if (peek().type == TokenType::CLOSEBRACE) break;

      Token bad = peek();
      throw std::runtime_error("Expected ',' or '}' in object literal at " + bad.loc.to_string());
   }
   expect(TokenType::CLOSEBRACE, "Expected '}' to close object literal");


   return obj;
}


std::unique_ptr < ExpressionNode > Parser::parse_lambda() {
   // Produce ParameterNode descriptors (support defaults and rest syntax)
   std::vector<std::unique_ptr<ParameterNode>> params;
   bool rest_seen = false;

   if (peek().type == TokenType::OPENPARENTHESIS) {
      consume(); // consume '('

      // allow empty parameter list
      while (peek().type != TokenType::CLOSEPARENTHESIS && peek().type != TokenType::EOF_TOKEN) {
         // rest param form: ...name[<number>]
         if (peek().type == TokenType::ELLIPSIS) {
            Token ellTok = consume();
            if (rest_seen) {
               throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed");
            }
            expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
            Token nameTok = tokens[position - 1];

            auto p = std::make_unique<ParameterNode>();
            p->token = ellTok;
            p->name = nameTok.value;
            p->is_rest = true;
            p->rest_required_count = 0;

            // optional bracketed required count: [...][NUMBER]
            if (peek().type == TokenType::OPENBRACKET) {
               consume(); // '['
               expect(TokenType::NUMBER, "Expected number inside rest count brackets");
               Token numTok = tokens[position - 1];
               try {
                  p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
               } catch (...) {
                  throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string());
               }
               expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
            }

            params.push_back(std::move(p));
            rest_seen = true;

            // rest must be last; allow trailing comma only if immediate close follows
            if (peek().type == TokenType::COMMA) {
               if (peek_next().type != TokenType::CLOSEPARENTHESIS) {
                  Token bad = tokens[position];
                  throw std::runtime_error("Rest parameter must be the last parameter at " + bad.loc.to_string());
               }
               // consume the comma; the loop will then see CLOSEPARENTHESIS and exit normally
            }

            // consume optional comma (if any) so trailing comma like (..., ) is tolerated
            if (peek().type == TokenType::COMMA) consume();
            break; // nothing allowed after rest in the parameter list
         }

         // normal identifier param (with optional default)
         Token id = consume();
         if (id.type != TokenType::IDENTIFIER) {
            throw std::runtime_error("Expected parameter name at " + id.loc.to_string());
         }

         auto pnode = std::make_unique<ParameterNode>();
         pnode->token = id;
         pnode->name = id.value;
         pnode->is_rest = false;
         pnode->rest_required_count = 0;
         pnode->defaultValue = nullptr;

         // optional default initializer: '=' expression
         if (peek().type == TokenType::ASSIGN) {
            consume(); // consume '='
            pnode->defaultValue = parse_expression();
            if (!pnode->defaultValue) {
               throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position-1].loc.to_string());
            }
         }

         params.push_back(std::move(pnode));

         // separator handling: comma between params
         if (peek().type == TokenType::COMMA) {
            consume();
            // allow trailing comma before ')'
            if (peek().type == TokenType::CLOSEPARENTHESIS) break;
            continue;
         }

         // otherwise end of param list
         break;
      }

      Token close = consume();
      if (close.type != TokenType::CLOSEPARENTHESIS) {
         throw std::runtime_error("Expected ')' after parameters at " + close.loc.to_string());
      }

   } else {
      // single identifier without parentheses: allow single identifier or single rest form
      if (peek().type == TokenType::ELLIPSIS) {
         Token ellTok = consume();
         if (rest_seen) {
            throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed");
         }
         expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
         Token nameTok = tokens[position - 1];

         auto p = std::make_unique<ParameterNode>();
         p->token = ellTok;
         p->name = nameTok.value;
         p->is_rest = true;
         p->rest_required_count = 0;

         if (peek().type == TokenType::OPENBRACKET) {
            consume(); // '['
            expect(TokenType::NUMBER, "Expected number inside rest count brackets");
            Token numTok = tokens[position - 1];
            try {
               p->rest_required_count = static_cast<size_t>(std::stoul(numTok.value));
            } catch (...) {
               throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string());
            }
            expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
         }

         params.push_back(std::move(p));
         rest_seen = true;
      } else {
         Token id = consume();
         if (id.type != TokenType::IDENTIFIER) {
            throw std::runtime_error("Expected parameter name at " + id.loc.to_string());
         }
         auto pnode = std::make_unique<ParameterNode>();
         pnode->token = id;
         pnode->name = id.value;
         pnode->is_rest = false;
         pnode->rest_required_count = 0;
         pnode->defaultValue = nullptr;
         params.push_back(std::move(pnode));
      }
   }

   // next token must be '=>'
   Token arrow = consume();
   if (arrow.type != TokenType::LAMBDA) {
      throw std::runtime_error("Expected '=>' after parameter list at " + arrow.loc.to_string());
   }

   if (peek().type == TokenType::OPENBRACE) {
      auto block = parse_block(true); // returns vector<StatementNode>
      return std::make_unique<LambdaNode>(std::move(params), std::move(block));
   } else {
      auto expr = parse_expression();
      return std::make_unique<LambdaNode>(std::move(params), std::move(expr));
   }
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


   if(t.type == TokenType::NULL_LITERAL) {
      return std::make_unique < NullNode > (consume());
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
      if (peek_next().type == TokenType::LAMBDA) {
         return parse_lambda(); // single-param without parentheses
      }

      // normal identifier
      Token id = consume();
      auto ident = std::make_unique < IdentifierNode > ();
      ident->name = id.value;
      ident->token = id;
      return ident;
   }
   
   if (t.type == TokenType::FUTA) {
    Token futaTok = consume(); // consume 'futa'
    auto node = std::make_unique<DeleteExpressionNode>();
    node->token = futaTok;

    // require parenthesized argument list for expression form
    expect(TokenType::OPENPARENTHESIS, "Expected '(' after 'futa' for expression form");
    // single-arg or allow comma-separated but we expect a single primary/expression as the target
    if (peek().type != TokenType::CLOSEPARENTHESIS) {
        node->target = parse_expression();
        // if you allow multiple args for some reason, you can parse more
    }
    expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after futa(...)");
    return node;
}
   
   
   if (t.type == TokenType::SELF) {
      Token id = consume();
      auto thisNode = std::make_unique < ThisExpressionNode > ();
      thisNode->token = id;
      return thisNode;
   }

   if (t.type == TokenType::UNDA) {
      Token newTok = consume();

      auto node = std::make_unique < NewExpressionNode > ();
      node->token = newTok;

      // expect a class identifier or callee expression
      node->callee = parse_primary();

      // optional argument list
      if (match(TokenType::OPENPARENTHESIS)) {
         if (peek().type != TokenType::CLOSEPARENTHESIS) {
            do {
               node->arguments.push_back(parse_expression());
            } while (match(TokenType::COMMA));
         }
         expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after new expression arguments.");
      }

      return node;
   }

   if (t.type == TokenType::SUPA) {
      Token superTok = consume();

      auto node = std::make_unique < SuperExpressionNode > ();
      node->token = superTok;

      // require argument list
      expect(TokenType::OPENPARENTHESIS, "Expected '(' after 'super'.");
      if (peek().type != TokenType::CLOSEPARENTHESIS) {
         do {
            node->arguments.push_back(parse_expression());
         } while (match(TokenType::COMMA));
      }
      expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after super arguments.");

      return node;
   }

   if (t.type == TokenType::OPENPARENTHESIS) {
      if (is_lambda_ahead()) {
         return parse_lambda();
      } else {
         consume();
         auto inner = parse_expression();
         expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after expression");
         return inner;
      }
   }


   auto skip_layout_tokens = [&]() {
      while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
         consume();
      }
   };

   if (t.type == TokenType::OPENBRACKET) {
      Token openTok = consume();
      auto arrayNode = std::make_unique < ArrayExpressionNode > ();
      arrayNode->token = openTok;

      skip_layout_tokens(); // ignore layout tokens immediately after '['

      if (peek().type != TokenType::CLOSEBRACKET) {
         while (true) {
            skip_layout_tokens(); // ignore before element

            if (peek().type == TokenType::ELLIPSIS) {
               Token ell = consume();
               auto spread = std::make_unique < SpreadElementNode > ();
               spread->token = ell;
               spread->argument = parse_expression();
               arrayNode->elements.push_back(std::move(spread));
            } else {
               arrayNode->elements.push_back(parse_expression());
            }

            skip_layout_tokens(); // ignore after element

            if (!match(TokenType::COMMA)) break;

            skip_layout_tokens(); // allow comma then layout before next element
            if (peek().type == TokenType::CLOSEBRACKET) break; // trailing comma allowed
         }
      }

      skip_layout_tokens(); // before ']'
      expect(TokenType::CLOSEBRACKET, "Expected ']' after array elements");
      return arrayNode;
   }


   if (t.type == TokenType::OPENBRACE) {
      return parse_object_expression();
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
         if (peek().type == TokenType::ELLIPSIS) {
            Token ell = consume();
            auto spread = std::make_unique < SpreadElementNode > ();
            spread->token = ell;
            spread->argument = parse_expression();
            call->arguments.push_back(std::move(spread));
         } else {
            call->arguments.push_back(parse_expression());
         }
      } while (match(TokenType::COMMA));
   }

   expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after call arguments");
   return call;
}