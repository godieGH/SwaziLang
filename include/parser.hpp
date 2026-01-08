// (snippet) add declaration to existing parser.hpp
#pragma once
#include <memory>
#include <vector>

#include "ast.hpp"
#include "token.hpp"

class Parser {
   public:
    Parser(const std::vector<Token>& tokens);
    std::unique_ptr<ProgramNode> parse();

   private:
    std::vector<Token> tokens;
    size_t position = 0;

    bool in_async_function = false;
    bool in_generator_function = false;

    Token peek() const;
    Token peek_next(size_t offset = 1) const;

    Token consume();
    bool match(TokenType t);
    void expect(TokenType t, const std::string& errMsg);

    bool is_lambda_ahead();

    std::unique_ptr<ExpressionNode> parse_condition();

    std::unique_ptr<ExpressionNode> parse_pattern();
    std::unique_ptr<ExpressionNode> parse_array_pattern();
    std::unique_ptr<ExpressionNode> parse_object_pattern();

    // expression parsing (precedence chain)
    std::unique_ptr<ExpressionNode> parse_expression();
    std::unique_ptr<ExpressionNode> parse_ternary();
    std::unique_ptr<ExpressionNode> parse_logical_nullish();
    std::unique_ptr<ExpressionNode> parse_logical_or();
    std::unique_ptr<ExpressionNode> parse_logical_and();
    std::unique_ptr<ExpressionNode> parse_equality();
    std::unique_ptr<ExpressionNode> parse_comparison();
    std::unique_ptr<ExpressionNode> parse_bitwise();
    std::unique_ptr<ExpressionNode> parse_additive();
    std::unique_ptr<ExpressionNode> parse_multiplicative();
    std::unique_ptr<ExpressionNode> parse_exponent();
    std::unique_ptr<ExpressionNode> parse_unary();
    std::unique_ptr<ExpressionNode> parse_primary();
    std::unique_ptr<ExpressionNode> parse_call(std::unique_ptr<ExpressionNode> callee);
    std::unique_ptr<ExpressionNode> parse_range();

    // NEW: helper to parse a template literal (backtick-style). Implementation in expressions.cpp
    std::unique_ptr<ExpressionNode> parse_template_literal();

    std::unique_ptr<ExpressionNode> parse_object_expression();
    std::unique_ptr<ExpressionNode> parse_tabia_method();
    std::unique_ptr<ExpressionNode> parse_lambda();

    std::unique_ptr<StatementNode> parse_import_declaration();
    std::unique_ptr<StatementNode> parse_export_declaration();

    bool saw_export = false;

    // statements
    std::unique_ptr<StatementNode> parse_statement();
    std::unique_ptr<StatementNode> parse_sequential_declarations(bool outer_is_constant);
    std::unique_ptr<StatementNode> parse_variable_declaration();
    std::unique_ptr<StatementNode> parse_print_statement(bool newline);
    std::unique_ptr<StatementNode> parse_throw_statement();
    std::unique_ptr<StatementNode> parse_assignment_or_expression_statement();
    std::unique_ptr<StatementNode> parse_try_catch();

    std::unique_ptr<StatementNode> parse_sequential_functions(bool outer_is_async, bool outer_is_generator);

    // function parsing
    std::unique_ptr<StatementNode> parse_function_declaration();
    std::unique_ptr<StatementNode> parse_class_declaration();
    std::unique_ptr<ClassBodyNode> parse_class_body(const std::string& className, bool braceStyle = false);
    std::unique_ptr<ClassMethodNode> parse_class_method(
        bool is_private,
        bool is_static,
        bool is_locked,
        const std::string& className,
        bool isCtor = false,
        bool isDtor = false,
        bool braceStyle = false);
    std::unique_ptr<StatementNode> parse_return_statement();

    // control-flow parsing
    std::unique_ptr<StatementNode> parse_if_statement();

    // loops
    std::unique_ptr<StatementNode> parse_for_statement();  // parses 'kwa (...) { ... }' or 'kwa (...) : <INDENT> ... <DEDENT>'
    std::unique_ptr<StatementNode> parse_for_classic_statement(Token forTok);
    std::unique_ptr<StatementNode> parse_for_in_statement(Token kwaTok);
    std::unique_ptr<StatementNode> parse_while_statement();     // parses 'wakati <cond> { ... }' or 'wakati <cond> : <INDENT> ... <DEDENT>'
    std::unique_ptr<StatementNode> parse_do_while_statement();  // parses 'fanya { ... } wakati <cond>' or 'fanya : <INDENT> ... wakati <cond>'

    // continue and breake controls for loops
    std::unique_ptr<StatementNode> parse_continue_statement();
    std::unique_ptr<StatementNode> parse_break_statement();

    std::unique_ptr<CaseNode> parse_switch_case();
    std::unique_ptr<StatementNode> parse_switch_statement();

    // helper to parse a block of statements either from an INDENT/DEDENT
    // or from brace-style { ... }. accept_brace_style==true allows '{ ... }',
    // otherwise will expect INDENT-style (called after COLON/NEWLINE).
    std::vector<std::unique_ptr<StatementNode>> parse_block(bool accept_brace_style);
};