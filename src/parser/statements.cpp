#include <iostream>
#include "parser.hpp"
#include <stdexcept>
#include <cctype>
#include <sstream>
#include <unordered_map>

#include <csignal>
#include "debugging/outputTry.hpp"


std::unique_ptr < StatementNode > Parser::parse_variable_declaration() {
  bool is_constant = false;

  if (peek().type == TokenType::CONSTANT) {
    consume();
    is_constant = true;
  }

  // After 'data' we allow either:
  //   IDENTIFIER                    -> simple var
  //   OPENBRACKET '[' ... ']'       -> array destructuring
  //   OPENBRACE   '{' ... '}'       -> object destructuring
  std::unique_ptr < ExpressionNode > pattern = nullptr;
  std::string name;
  Token idTok;

  if (peek().type == TokenType::IDENTIFIER) {
    consume();
    idTok = tokens[position - 1];
    name = idTok.value;
  } else if (peek().type == TokenType::OPENBRACKET || peek().type == TokenType::OPENBRACE) {
    // parse a pattern and use its token as the declaration token
    pattern = parse_pattern();
    // choose token for error reporting
    idTok = pattern->token;
  } else {
    Token tok = peek();
    throw std::runtime_error("Parse error at " + tok.loc.to_string() + ": Expected identifier or destructuring pattern after 'data'");
  }

  std::unique_ptr < ExpressionNode > value = nullptr;

  if (is_constant) {
    // force an initializer for constants
    expect(TokenType::ASSIGN, "Constant '" + (pattern ? "<pattern>": name) + "' must be initialized");
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
  if (pattern) {
    node->pattern = std::move(pattern);
    node->identifier = ""; // not a simple identifier
  } else {
    node->identifier = name;
    node->pattern = nullptr;
  }
  node->value = std::move(value);
  node->is_constant = is_constant;
  node->token = idTok;
  return node;
}


// --- REPLACE existing Parser::parse_import_declaration() and Parser::parse_export_declaration()
// with these more layout-tolerant implementations that accept NEWLINE/INDENT/DEDENT before '}'
// and allow trailing commas and dedent-then-close style used by the indent grammar.

std::unique_ptr < StatementNode > Parser::parse_import_declaration() {
  // 'tumia' token already consumed by caller; tokens[position-1] is the 'tumia' token.
  Token tumiaTok = tokens[position - 1];

  auto node = std::make_unique < ImportDeclarationNode > ();
  node->token = tumiaTok;

  // Helper to consume optional trailing semicolon
  auto maybe_consume_semicolon = [&]() {
    if (peek().type == TokenType::SEMICOLON) consume();
  };

  // Helper: skip formatting tokens
  auto skip_formatting = [&]() {
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
  };

  // Case A: side-effect only import: tumia "path"
  if (peek().type == TokenType::STRING || peek().type == TokenType::SINGLE_QUOTED_STRING) {
    Token pathTok = consume();
    node->side_effect_only = true;
    node->module_path = pathTok.value;
    node->module_token = pathTok;
    maybe_consume_semicolon();
    return node;
  }

  // Case B: tumia * kutoka "path"
  if (peek().type == TokenType::STAR) {
    Token star = consume();
    node->import_all = true;
    expect(TokenType::KUTOKA, "Expected 'kutoka' after '*' in import");
    expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
    Token pathTok = tokens[position - 1];
    node->module_path = pathTok.value;
    node->module_token = pathTok;
    maybe_consume_semicolon();
    return node;
  }

  // Case C: tumia { a, b as c } kutoka "path"
  if (peek().type == TokenType::OPENBRACE) {
    consume(); // '{'
    skip_formatting();

    // empty import list {}
    if (peek().type == TokenType::CLOSEBRACE) {
      consume();
      expect(TokenType::KUTOKA, "Expected 'kutoka' after import specifiers");
      expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
      Token pathTok = tokens[position - 1];
      node->module_path = pathTok.value;
      node->module_token = pathTok;
      maybe_consume_semicolon();
      return node;
    }

    while (true) {
      skip_formatting();

      // allow early close when layout inserted a DEDENT/newline before '}'
      if (peek().type == TokenType::CLOSEBRACE) break;

      expect(TokenType::IDENTIFIER, "Expected identifier in import specifier");
      Token impTok = tokens[position - 1];
      std::string imported = impTok.value;
      std::string local = imported;

      // optional alias: 'kama' IDENT
      skip_formatting();
      if (peek().type == TokenType::KAMA) {
        consume(); // 'kama'
        skip_formatting();
        expect(TokenType::IDENTIFIER, "Expected identifier after 'kama' in import alias");
        Token localTok = tokens[position - 1];
        local = localTok.value;
      }

      auto spec = std::make_unique < ImportSpecifier > ();
      spec->imported = imported;
      spec->local = local;
      spec->token = impTok;
      node->specifiers.push_back(std::move(spec));

      skip_formatting();

      if (peek().type == TokenType::COMMA) {
        consume();
        // allow trailing comma before close (skip formatting then allow close)
        skip_formatting();
        if (peek().type == TokenType::CLOSEBRACE) break;
        continue;
      }

      // If next is closebrace or layout that eventually leads to closebrace, continue loop to detect it
      if (peek().type == TokenType::CLOSEBRACE) break;

      // otherwise break and let expect(CLOSEBRACE) validate
      break;
    }

    // Accept closing brace possibly after layout tokens
    skip_formatting();
    expect(TokenType::CLOSEBRACE, "Expected '}' after import specifiers");
    // require 'kutoka' and string path
    skip_formatting();
    expect(TokenType::KUTOKA, "Expected 'kutoka' after import specifiers");
    skip_formatting();
    expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
    Token pathTok = tokens[position - 1];
    node->module_path = pathTok.value;
    node->module_token = pathTok;
    maybe_consume_semicolon();
    return node;
  }

  // Case D: default/module binding: tumia localName [kama alias] kutoka "path"
  if (peek().type == TokenType::IDENTIFIER) {
    Token idTok = consume();
    std::string imported = "default"; // by convention: single identifier imports module default/object
    std::string local = idTok.value;

    // optional alias: tumia original kama alias  (rare; support either exported name or default)
    skip_formatting();
    if (peek().type == TokenType::KAMA) {
      consume(); // 'kama'
      skip_formatting();
      expect(TokenType::IDENTIFIER, "Expected identifier after 'kama' for import alias");
      Token aliasTok = tokens[position - 1];
      local = aliasTok.value;
    }

    // require 'kutoka' and module path
    skip_formatting();
    expect(TokenType::KUTOKA, "Expected 'kutoka' after import target");
    skip_formatting();
    expect(TokenType::STRING, "Expected module string after 'kutoka' in import");
    Token pathTok = tokens[position - 1];

    auto spec = std::make_unique < ImportSpecifier > ();
    spec->imported = imported;
    spec->local = local;
    spec->token = idTok;
    node->specifiers.push_back(std::move(spec));
    node->module_path = pathTok.value;
    node->module_token = pathTok;
    maybe_consume_semicolon();
    return node;
  }

  // Otherwise it's a syntax error
  Token bad = peek();
  throw std::runtime_error("Parse error at " + bad.loc.to_string() + ": invalid import syntax after 'tumia'");
}

std::unique_ptr < StatementNode > Parser::parse_export_declaration() {
  // 'ruhusu' token already consumed by caller
  Token ruhusuTok = tokens[position - 1];

  if (saw_export) {
    throw std::runtime_error("Parse error at " + ruhusuTok.loc.to_string() + ": multiple 'ruhusu' (export) declarations are not allowed");
  }

  auto node = std::make_unique < ExportDeclarationNode > ();
  node->token = ruhusuTok;

  // Helper to skip formatting tokens
  auto skip_formatting = [&]() {
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
  };

  // Two main forms:
  //  - ruhusu IDENT  (default export of identifier)
  //  - ruhusu { a, b, c }  (named exports list)
  skip_formatting();
  if (peek().type == TokenType::IDENTIFIER) {
    Token idTok = consume();
    node->is_default = true;
    node->single_identifier = idTok.value;
    saw_export = true;
    if (peek().type == TokenType::SEMICOLON) consume();
    return node;
  }

  if (peek().type == TokenType::OPENBRACE) {
    consume(); // '{'
    skip_formatting();

    // empty export list {}
    if (peek().type == TokenType::CLOSEBRACE) {
      consume();
      saw_export = true;
      if (peek().type == TokenType::SEMICOLON) consume();
      return node;
    }

    while (true) {
      skip_formatting();

      // allow early close when layout inserted a DEDENT/newline before '}'
      if (peek().type == TokenType::CLOSEBRACE) break;

      expect(TokenType::IDENTIFIER, "Expected identifier in export list");
      Token idTok = tokens[position - 1];
      node->names.push_back(idTok.value);

      skip_formatting();
      if (peek().type == TokenType::COMMA) {
        consume();
        // allow trailing comma before close
        skip_formatting();
        if (peek().type == TokenType::CLOSEBRACE) break;
        continue;
      } else {
        // if next is CLOSEBRACE, loop will exit; otherwise break and let expect validate
        if (peek().type == TokenType::CLOSEBRACE) break;
        // allow layout -> next iteration will handle it
        continue;
      }
    }

    skip_formatting();
    expect(TokenType::CLOSEBRACE, "Expected '}' after export list");
    saw_export = true;
    if (peek().type == TokenType::SEMICOLON) consume();
    return node;
  }

  Token bad = peek();
  throw std::runtime_error("Parse error at " + bad.loc.to_string() + ": expected identifier or '{' after 'ruhusu'");
}



std::unique_ptr < StatementNode > Parser::parse_class_declaration() {
  // muundo has already been consumed by caller
  expect(TokenType::IDENTIFIER, "Expected class name after 'muundo' keyword.");
  Token idTok = tokens[position - 1];
  auto nameNode = std::make_unique < IdentifierNode > ();
  nameNode->name = idTok.value;
  nameNode->token = idTok;

  std::unique_ptr < IdentifierNode > superNode = nullptr;

  // NEW: accept either:
  //   muundo Name rithi Super
  // or
  //   muundo Name(Super)
  // The parenthesized form is a shorthand (python-style) and works with either ':' (indent) or '{' body forms.
  if (peek().type == TokenType::RITHI) {
    consume(); // consume 'rithi'
    expect(TokenType::IDENTIFIER, "Expected base class name after 'rithi'.");
    Token superTok = tokens[position - 1];
    superNode = std::make_unique < IdentifierNode > ();
    superNode->name = superTok.value;
    superNode->token = superTok;
  } else if (peek().type == TokenType::OPENPARENTHESIS) {
    // parenthesized shorthand: (Super)
    consume(); // consume '('

    // allow optional formatting inside parentheses (follow other parser tolerant patterns)
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();

    expect(TokenType::IDENTIFIER, "Expected base class name inside parentheses after class name");
    Token superTok = tokens[position - 1];
    superNode = std::make_unique < IdentifierNode > ();
    superNode->name = superTok.value;
    superNode->token = superTok;

    // allow optional formatting before closing ')'
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) consume();
    expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after base class name");
  }

  std::unique_ptr < ClassBodyNode > body = nullptr;

  if (match(TokenType::COLON)) {
    // pythonic / indent-based body
    expect(TokenType::NEWLINE, "Expected newline after ':' in class declaration.");
    expect(TokenType::INDENT, "Expected indented block for class body.");

    // pass the class name and indicate non-brace style
    body = parse_class_body(nameNode->name, /*braceStyle=*/false);
    if (!body) body = std::make_unique < ClassBodyNode > ();

    // caller closes the indented block
    expect(TokenType::DEDENT, "Expected dedent to close class body.");

  } else if (match(TokenType::OPENBRACE)) {
    // C-style block { ... }
    // pass the class name and indicate brace style = true
    body = parse_class_body(nameNode->name, /*braceStyle=*/true);
    if (!body) body = std::make_unique < ClassBodyNode > ();

    // skip stray separators, then expect the closing brace
    while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
      consume();
    }
    expect(TokenType::CLOSEBRACE, "Expected '}' to close class body.");
  } else {
    expect(TokenType::COLON, "Expected ':' or '{' to begin class body.");
  }

  // Construct node
  auto classNode = std::make_unique < ClassDeclarationNode > (
    std::move(nameNode),
    std::move(superNode),
    std::move(body)
  );

  // --- IMPORTANT: set token BEFORE any validation that might use it ---
  classNode->token = idTok;

  // Validation block (constructor/destructor handled specially)
  if (classNode->body) {
    int constructorCount = 0;
    int destructorCount = 0;
    std::unordered_map < std::string,
    int > memberCount; // composite key -> count

    // methods
    for (const auto &mPtr: classNode->body->methods) {
      if (!mPtr) continue;
      const ClassMethodNode &m = *mPtr;

      // token fallback (use class token if method token is missing)
      Token methodTok = (m.token.type != TokenType::EOF_TOKEN) ? m.token: classNode->token;

      // constructor
      if (m.is_constructor) {
        constructorCount++;
        if (m.name != classNode->name->name) {
          throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
            ": constructor name '" + m.name + "' must match class name '" + classNode->name->name + "'.");
        }
        if (m.is_static) {
          throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
            ": constructor must not be static.");
        }
        // do NOT add constructor to memberCount (avoid collision with members named same as class)
        continue; // <-- important: skip normal member counting
      }

      // destructor
      if (m.is_destructor) {
        destructorCount++;
        if (m.name != classNode->name->name) {
          throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
            ": destructor name '" + m.name + "' must match class name '" + classNode->name->name + "'.");
        }
        if (m.is_static) {
          throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
            ": destructor must not be static.");
        }
        // do NOT add destructor to memberCount
        continue; // <-- important
      }

      // getter arg rule
      if (m.is_getter && !m.params.empty()) {
        throw std::runtime_error("Parse error at " + methodTok.loc.to_string() +
          ": getter '" + m.name + "' must not have parameters.");
      }

      // composite key: static/instance + method + name
      std::string mkey = std::string(m.is_static ? "S:": "I:") + "M:" + m.name;
      memberCount[mkey]++;
    }

    // properties
    for (const auto &pPtr: classNode->body->properties) {
      if (!pPtr) continue;
      const ClassPropertyNode &p = *pPtr;
      Token propTok = (p.token.type != TokenType::EOF_TOKEN) ? p.token: classNode->token;

      std::string pkey = std::string(p.is_static ? "S:": "I:") + "P:" + p.name;
      memberCount[pkey]++;
    }

    // enforce at most one constructor/destructor
    if (constructorCount > 1) {
      throw std::runtime_error("Parse error at " + classNode->token.loc.to_string() +
        ": multiple constructors defined for class '" + classNode->name->name + "'.");
    }
    if (destructorCount > 1) {
      throw std::runtime_error("Parse error at " + classNode->token.loc.to_string() +
        ": multiple destructors defined for class '" + classNode->name->name + "'.");
    }

    // report duplicates (static/instance and kind-aware)
    for (const auto &kv: memberCount) {
      if (kv.second > 1) {
        // pretty name: substring after last ':'
        auto pos = kv.first.rfind(':');
        std::string pretty = (pos == std::string::npos) ? kv.first: kv.first.substr(pos + 1);
        throw std::runtime_error("Parse error at " + classNode->token.loc.to_string() +
          ": duplicate member name '" + pretty + "' found " + std::to_string(kv.second) + " times.");
      }
    }
  }

  return std::unique_ptr < StatementNode > (std::move(classNode));
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


    // Support compound assignment operators (+=, -=, *=) for assignable L-values (Identifier, Member, Index)
    if (peek().type == TokenType::PLUS_ASSIGN ||
      peek().type == TokenType::MINUS_ASSIGN ||
      peek().type == TokenType::TIMES_ASSIGN) {
      if (!is_assignable(nodeExpr.get())) {
        Token opTok = peek();
        throw std::runtime_error("Compound assignment is only supported for assignable targets at " + opTok.loc.to_string());
      }

      Token opTok = consume(); // one of +=, -=, *=
      auto right = parse_expression();

      // build BinaryExpressionNode: left <op> right
      auto bin = std::make_unique < BinaryExpressionNode > ();
      if (opTok.type == TokenType::PLUS_ASSIGN) bin->op = "+";
      else if (opTok.type == TokenType::MINUS_ASSIGN) bin->op = "-";
      else /* TIMES_ASSIGN */ bin->op = "*";

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
  bool rest_seen = false;
  while (true) {
    // support rest param: ...name[<number>]
    if (peek().type == TokenType::ELLIPSIS) {
      Token ellTok = consume(); // consume '...'

      if (rest_seen) {
        throw std::runtime_error("Parse error at " + ellTok.loc.to_string() + ": only one rest parameter is allowed");
      }

      expect(TokenType::IDENTIFIER, "Expected identifier after '...'");
      Token nameTok = tokens[position - 1];

      auto p = std::make_unique < ParameterNode > ();
      p->token = ellTok;
      p->name = nameTok.value;
      p->is_rest = true;
      p->rest_required_count = 0;

      // optional [NUMBER] to indicate required count for the rest array
      if (peek().type == TokenType::OPENBRACKET) {
        consume(); // '['
        expect(TokenType::NUMBER, "Expected number inside rest count brackets");
        Token numTok = tokens[position - 1];
        try {
          p->rest_required_count = static_cast<size_t > (std::stoul(numTok.value));
        } catch (...) {
          throw std::runtime_error("Invalid number in rest parameter at " + numTok.loc.to_string());
        }
        expect(TokenType::CLOSEBRACKET, "Expected ']' after rest count");
      }

      funcNode->parameters.push_back(std::move(p));
      rest_seen = true;

      // rest must be last
      if (peek().type == TokenType::COMMA) {
        Token c = tokens[position]; // lookahead comma
        throw std::runtime_error("Rest parameter must be the last parameter at " + c.loc.to_string());
      }
    }
    // normal identifier parameter (with optional default)
    else if (peek().type == TokenType::IDENTIFIER) {
      Token nameTok = consume();
      auto p = std::make_unique < ParameterNode > ();
      p->token = nameTok;
      p->name = nameTok.value;
      p->is_rest = false;
      p->rest_required_count = 0;
      p->defaultValue = nullptr;

      // optional default initializer: '=' expression
      if (peek().type == TokenType::ASSIGN) {
        consume(); // consume '='
        p->defaultValue = parse_expression();
        if (!p->defaultValue) {
          throw std::runtime_error("Expected expression after '=' for default parameter at " + tokens[position-1].loc.to_string());
        }
      }

      funcNode->parameters.push_back(std::move(p));
    }
    else {
      // no more parameters
      break;
    }

    // if next token is a comma, consume and continue; otherwise parameter list ended
    if (!match(TokenType::COMMA)) break;
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


std::unique_ptr < StatementNode > Parser::parse_break_statement() {
  // The 'SIMAMA' token was already consumed by parse_statement
  Token kwTok = tokens[position - 1];

  auto node = std::make_unique < BreakStatementNode > ();
  node->token = kwTok;

  // optionally consume a semicolon
  if (peek().type == TokenType::SEMICOLON) consume();

  return node;
}

std::unique_ptr < StatementNode > Parser::parse_continue_statement() {
  // The 'ENDELEA' token was already consumed by parse_statement
  Token kwTok = tokens[position - 1];

  auto node = std::make_unique < ContinueStatementNode > ();
  node->token = kwTok;

  // optionally consume a semicolon
  if (peek().type == TokenType::SEMICOLON) consume();

  return node;
}



std::unique_ptr < CaseNode > Parser::parse_switch_case() {
  auto caseNode = std::make_unique < CaseNode > ();

  if (match(TokenType::IKIWA)) {
    caseNode->test = parse_expression();
  } else if (match(TokenType::KAIDA)) {
    caseNode->test = nullptr; // default
  } else {
    Token tok = peek();
    throw std::runtime_error("Parse error at " + tok.loc.to_string() +
      ": Expected 'ikiwa' or 'kaida' in switch");
  }

  // Now parse the body, which can be ':' (indented block) or '{' (brace block)
  if (match(TokenType::COLON)) {
    // For indented blocks we must require NEWLINE + INDENT
    expect(TokenType::NEWLINE, "Expected newline after ':' in case");
    expect(TokenType::INDENT, "Expected indented block after case ':'");

    while (peek().type != TokenType::DEDENT &&
      peek().type != TokenType::EOF_TOKEN) {
      auto stmt = parse_statement();
      if (!stmt) break;
      caseNode->body.push_back(std::move(stmt));
    }
    expect(TokenType::DEDENT, "Expected dedent to close case body");

  } else if (match(TokenType::OPENBRACE)) {
    // Brace-based block
    while (peek().type != TokenType::CLOSEBRACE &&
      peek().type != TokenType::EOF_TOKEN) {
      while (peek().type == TokenType::NEWLINE ||
        peek().type == TokenType::INDENT ||
        peek().type == TokenType::DEDENT) {
        consume();
      }
      if (peek().type == TokenType::CLOSEBRACE ||
        peek().type == TokenType::EOF_TOKEN) break;
      auto stmt = parse_statement();
      if (!stmt) break;
      caseNode->body.push_back(std::move(stmt));
    }
    expect(TokenType::CLOSEBRACE, "Expected '}' to close case body");

  } else {
    Token tok = peek();
    throw std::runtime_error("Parse error at " + tok.loc.to_string() +
      ": Expected ':' or '{' after case header");
  }

  return caseNode;
}

std::unique_ptr < StatementNode > Parser::parse_switch_statement() {
  // 'chagua' token was already consumed by parse_statement
  auto node = std::make_unique < SwitchNode > ();
  node->discriminant = parse_expression();

  if (match(TokenType::COLON)) {
    // Pythonic style
    expect(TokenType::NEWLINE, "Expected newline after ':' in switch");
    expect(TokenType::INDENT, "Expected indented block for switch body");

    while (peek().type != TokenType::DEDENT &&
      peek().type != TokenType::EOF_TOKEN) {
      node->cases.push_back(parse_switch_case());
    }
    expect(TokenType::DEDENT, "Expected dedent to close switch body");

  } else if (match(TokenType::OPENBRACE)) {
    // C-style braces
    while (peek().type != TokenType::CLOSEBRACE &&
      peek().type != TokenType::EOF_TOKEN) {
      while (peek().type == TokenType::NEWLINE ||
        peek().type == TokenType::INDENT ||
        peek().type == TokenType::DEDENT) {
        consume();
      }
      if (peek().type == TokenType::CLOSEBRACE ||
        peek().type == TokenType::EOF_TOKEN) break;
      node->cases.push_back(parse_switch_case());
    }
    expect(TokenType::CLOSEBRACE, "Expected '}' to close switch body");

  } else {
    Token tok = peek();
    throw std::runtime_error("Parse error at " + tok.loc.to_string() +
      ": Expected ':' or '{' after 'chagua' expression");
  }

  return node;
}




std::unique_ptr < StatementNode > Parser::parse_try_catch() {
  // caller already consumed 'jaribu' (JARIBU)
  if (position == 0) {
    throw std::runtime_error("Internal parser error: parse_try_catch called with position == 0");
  }

  auto node = std::make_unique < TryCatchNode > ();

  // record the starting 'jaribu' token on the node (useful for error reporting / locations)
  node->token = tokens[position - 1];

  // small lambda to parse either an indentation-based or brace-based block
  auto parse_block_into = [&](std::vector < std::unique_ptr < StatementNode>>& dest) {
    if (match(TokenType::COLON)) {
      // indentation-based body
      expect(TokenType::NEWLINE, "Expected newline after ':'");
      expect(TokenType::INDENT, "Expected indented block");

      while (peek().type != TokenType::DEDENT && peek().type != TokenType::EOF_TOKEN) {
        auto stmt = parse_statement();
        if (!stmt) break; // defensive: EOF or parse error
        dest.push_back(std::move(stmt));
      }

      expect(TokenType::DEDENT, "Expected dedent to close block");

    } else if (match(TokenType::OPENBRACE)) {
      // brace-based body
      while (peek().type != TokenType::CLOSEBRACE && peek().type != TokenType::EOF_TOKEN) {
        // skip separators between statements
        while (peek().type == TokenType::NEWLINE || peek().type == TokenType::INDENT || peek().type == TokenType::DEDENT) {
          consume();
        }
        if (peek().type == TokenType::CLOSEBRACE || peek().type == TokenType::EOF_TOKEN) break;
        auto stmt = parse_statement();
        if (!stmt) break;
        dest.push_back(std::move(stmt));
      }
      expect(TokenType::CLOSEBRACE, "Expected '}' to close block");
    } else {
      // helpful error-message: caller expected a block start
      expect(TokenType::COLON, "Expected ':' or '{' to begin block");
    }
  };

  // --- parse try block ---
  parse_block_into(node->tryBlock);

  // --- parse catch (MAKOSA) ---
  if (match(TokenType::MAKOSA)) {
    // optional error variable: either IDENTIFIER or ( IDENTIFIER )
    if (match(TokenType::IDENTIFIER)) {
      // match() consumed the identifier, so the token is at tokens[position-1]
      node->errorVar = tokens[position - 1].value; // <-- fixed: use the lexeme/text field
    } else if (match(TokenType::OPENPARENTHESIS)) {
      expect(TokenType::IDENTIFIER, "Expected identifier in catch parentheses");
      node->errorVar = tokens[position - 1].value;
      expect(TokenType::CLOSEPARENTHESIS, "Expected ')' after catch identifier");
    } else {
      // no explicit variable provided — make this a clear error
      expect(TokenType::IDENTIFIER, "Expected an error identifier to hold the error object after 'makosa'.");
      // (expect will throw, so we won't reach here)
    }

    // now parse the catch body into catchBlock
    parse_block_into(node->catchBlock);
  } else {
    // error: 'makosa' expected after try block
    expect(TokenType::MAKOSA, "Expected 'makosa' after 'jaribu' block");
  }

  // --- optional finally (KISHA) ---
  if (match(TokenType::KISHA)) {
    parse_block_into(node->finallyBlock);
  }

  // debugging the ast
  //std::cout << trycatch_to_json(*node) << std::endl;


  return std::unique_ptr < StatementNode > (std::move(node));
}