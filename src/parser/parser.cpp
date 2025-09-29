// scr/parser/parser.cpp
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

Token Parser::peek_next(size_t offset) const {
   if (position + offset < tokens.size()) {
      return tokens[position + offset];
   }
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

bool Parser::is_lambda_ahead() {
   size_t saved = position; // remember position

   consume(); // consume '('
   bool found = false;

   // check for zero or more identifiers separated by commas
   while (peek().type != TokenType::CLOSEPARENTHESIS &&
      peek().type != TokenType::EOF_TOKEN) {
      if (peek().type != TokenType::IDENTIFIER) {
         position = saved;
         return false;
      }
      consume();
      if (peek().type == TokenType::COMMA) consume();
   }

   if (peek().type != TokenType::CLOSEPARENTHESIS) {
      position = saved;
      return false;
   }

   consume(); // consume ')'

   // check if next token is LAMBDA
   if (peek().type == TokenType::LAMBDA) {
      found = true;
   }

   position = saved; // restore
   return found;
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
   
   if (p.type == TokenType::CHAGUA) {
      consume(); // consume 'chagua'
      return parse_switch_statement();
   }
   
   if (p.type == TokenType::RUDISHA) {
      consume(); // consume 'rudisha'
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