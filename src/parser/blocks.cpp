// src/parser/blocks.cpp
#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <sstream>

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
