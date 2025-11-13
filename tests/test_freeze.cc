#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "evaluator.hpp"
#include "lexer.hpp"
#include "parser.hpp"

// Helper: parse & evaluate a program string using the same Evaluator instance
static void evalProgram(Evaluator& ev, const std::string& src) {
    Lexer lx(src, "<test>");
    auto toks = lx.tokenize();
    Parser p(toks);
    auto prog = p.parse();
    ev.evaluate(prog.get());
}

// Helper: evaluate a single expression (returns Value) using evaluator's ambient env
// NOTE: This helper throws std::runtime_error on parse/assert failures (avoids using
// GoogleTest ASSERT_* inside a non-void helper which would cause void returns).
static Value evalExpr(Evaluator& ev, const std::string& src) {
    Lexer lx(src, "<test-expr>");
    auto toks = lx.tokenize();
    Parser p(toks);
    auto prog = p.parse();
    if (!prog || prog->body.size() < 1u) {
        throw std::runtime_error("parse produced empty program for expression helper");
    }
    auto* es = dynamic_cast<ExpressionStatementNode*>(prog->body[0].get());
    if (!es) {
        throw std::runtime_error("expected an ExpressionStatement in helper");
    }
    return ev.evaluate_expression(es->expression.get());
}

TEST(FreezeRuntimeTest, FreezeRespectsAndDeleteBypasses) {
    Evaluator ev;
    ev.set_entry_point("<test>");

    // 1) Create object, with an instance method (ppp) that mutates the object via $ (self)
    //    then freeze it and attempt various mutations.
    std::string setup = R"sw(
data ob = {
  name: "John Doe",
  age: 20,
  tabia ppp(n):
    $.name = n
}

Object.freeze(ob)
)sw";

    // Evaluate setup
    EXPECT_NO_THROW(evalProgram(ev, setup));

    // 2) Attempt external mutations which should be silently ignored (no throw).
    //    (a) simple member assignment
    EXPECT_NO_THROW(evalProgram(ev, "ob.name = \"Jane Doe\"\n"));
    //    (b) index-style compound assignment
    EXPECT_NO_THROW(evalProgram(ev, "ob['age'] += 5\n"));
    //    (c) adding a new property should be ignored (no throw)
    EXPECT_NO_THROW(evalProgram(ev, "ob.newprop = 123\n"));

    // Verify the above external mutations did NOT change the object:
    Value v_name = evalExpr(ev, "ob.name\n");
    ASSERT_TRUE(std::holds_alternative<std::string>(v_name));
    EXPECT_EQ(std::get<std::string>(v_name), "John Doe");  // unchanged

    Value v_age = evalExpr(ev, "ob.age\n");
    ASSERT_TRUE(std::holds_alternative<double>(v_age));
    EXPECT_EQ(std::get<double>(v_age), 20.0);  // unchanged

    // newprop should not exist (reading it yields undefined / nullish -> std::monostate)
    Value v_new = evalExpr(ev, "ob.newprop\n");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(v_new));

    // 3) __proto__.delete('age') should throw because the object is frozen
    {
        Lexer lx("ob.__proto__.delete('age')\n", "<test>");
        auto toks = lx.tokenize();
        Parser p(toks);
        auto prog = p.parse();
        // executing the call should throw (PermissionError thrown as std::runtime_error in __proto__.delete)
        EXPECT_THROW(ev.evaluate(prog.get()), std::exception);
    }

    // 4) Internal mutation (method on the object) should be allowed:
    //    call ob.ppp("Inner") which sets $.name = "Inner"
    EXPECT_NO_THROW(evalProgram(ev, "ob.ppp(\"Inner\")\n"));

    Value v_name2 = evalExpr(ev, "ob.name\n");
    ASSERT_TRUE(std::holds_alternative<std::string>(v_name2));
    EXPECT_EQ(std::get<std::string>(v_name2), "Inner");  // mutated from inside object

    // 5) The delete (futa) statement should bypass frozen and clear the object's properties.
    EXPECT_NO_THROW(evalProgram(ev, "futa ob\n"));

    // After futa, the object binding 'ob' should still exist but its properties cleared.
    Value v_name_after = evalExpr(ev, "ob.name\n");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(v_name_after));

    Value v_age_after = evalExpr(ev, "ob.age\n");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(v_age_after));

    // The object pointer should still be an object (not null). Use .aina to check type
    Value t = evalExpr(ev, "ob.aina\n");  // .aina returns a string type name
    ASSERT_TRUE(std::holds_alternative<std::string>(t));
    EXPECT_EQ(std::get<std::string>(t), "object");
}