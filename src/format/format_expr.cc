#include "SourceManager.hpp"
#include "ast.hpp"
#include "format/format.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "token.hpp"

std::string format_expression(ExpressionNode* expr) {
    if (!expr) return "";

    if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) {
        std::ostringstream ss;
        ss << n->value;
        return ss.str();
    }

    if (auto s = dynamic_cast<StringLiteralNode*>(expr)) {
        return "\"" + s->value + "\"";
    }

    if (auto b = dynamic_cast<BooleanLiteralNode*>(expr)) {
        return b->value ? "kweli" : "sikweli";
    }

    if (auto nn = dynamic_cast<NullNode*>(expr)) {
        return "null";
    }

    if (auto line = dynamic_cast<LineNode*>(expr)) {
        return "__line__";
    }

    if (auto nan = dynamic_cast<NaNNode*>(expr)) {
        return "nan";
    }

    if (auto inf = dynamic_cast<InfNode*>(expr)) {
        return "inf";
    }

    if (auto dt = dynamic_cast<DateTimeLiteralNode*>(expr)) {
        return dt->literalText;
    }

    if (auto id = dynamic_cast<IdentifierNode*>(expr)) {
        return id->name;
    }

    if (auto self = dynamic_cast<SelfExpressionNode*>(expr)) {
        return "this";
    }

    if (auto u = dynamic_cast<UnaryExpressionNode*>(expr)) {
        std::string operand = format_expression(u->operand.get());
        return u->op + operand;
    }

    if (auto b = dynamic_cast<BinaryExpressionNode*>(expr)) {
        std::string left = format_expression(b->left.get());
        std::string right = format_expression(b->right.get());
        return left + " " + b->op + " " + right;
    }

    if (auto tern = dynamic_cast<TernaryExpressionNode*>(expr)) {
        std::string cond = format_expression(tern->condition.get());
        std::string then_expr = format_expression(tern->thenExpr.get());
        std::string else_expr = format_expression(tern->elseExpr.get());
        return cond + " ? " + then_expr + " : " + else_expr;
    }

    if (auto arr = dynamic_cast<ArrayExpressionNode*>(expr)) {
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < arr->elements.size(); i++) {
            if (i > 0) ss << ", ";
            ss << format_expression(arr->elements[i].get());
        }
        ss << "]";
        return ss.str();
    }

    if (auto obj = dynamic_cast<ObjectExpressionNode*>(expr)) {
        std::ostringstream ss;
        ss << "{\n";
        for (size_t i = 0; i < obj->properties.size(); i++) {
            auto& prop = obj->properties[i];
            ss << "  ";

            if (prop->is_private) ss << "@";
            if (prop->is_static) ss << "*";
            if (prop->is_locked) ss << "&";

            if (prop->kind == PropertyKind::Spread) {
                ss << "..." << format_expression(prop->value.get());
            } else if (prop->kind == PropertyKind::Shorthand) {
                ss << prop->key_name;
            } else {
                if (prop->computed) {
                    ss << "[" << format_expression(prop->key.get()) << "]";
                } else {
                    ss << prop->key_name;
                }
                ss << ": " << format_expression(prop->value.get());
            }

            if (i < obj->properties.size() - 1) ss << ",";
            ss << "\n";
        }
        ss << "}";
        return ss.str();
    }

    if (auto mem = dynamic_cast<MemberExpressionNode*>(expr)) {
        std::string obj = format_expression(mem->object.get());
        std::string op = mem->is_optional ? "?." : ".";
        return obj + op + mem->property;
    }

    if (auto idx = dynamic_cast<IndexExpressionNode*>(expr)) {
        std::string obj = format_expression(idx->object.get());
        std::string index = format_expression(idx->index.get());
        std::string op = idx->is_optional ? "?[" : "[";
        return obj + op + index + "]";
    }

    if (auto call = dynamic_cast<CallExpressionNode*>(expr)) {
        std::string callee = format_expression(call->callee.get());
        std::ostringstream ss;
        ss << callee << (call->is_optional ? "?(" : "(");
        for (size_t i = 0; i < call->arguments.size(); i++) {
            if (i > 0) ss << ", ";
            ss << format_expression(call->arguments[i].get());
        }
        ss << ")";
        return ss.str();
    }

    if (auto tpl = dynamic_cast<TemplateLiteralNode*>(expr)) {
        std::ostringstream ss;
        ss << "`";
        for (size_t i = 0; i < tpl->quasis.size(); i++) {
            ss << tpl->quasis[i];
            if (i < tpl->expressions.size()) {
                ss << "${" << format_expression(tpl->expressions[i].get()) << "}";
            }
        }
        ss << "`";
        return ss.str();
    }

    if (auto rng = dynamic_cast<RangeExpressionNode*>(expr)) {
        std::string start = format_expression(rng->start.get());
        std::string end = format_expression(rng->end.get());
        std::string op = rng->inclusive ? "..." : "..";
        std::string result = start + op + end;
        if (rng->step) {
            result += " step " + format_expression(rng->step.get());
        }
        return result;
    }

    if (auto lambda = dynamic_cast<LambdaNode*>(expr)) {
        std::ostringstream ss;
        if (lambda->is_async) ss << "async ";

        if (lambda->params.size() == 1) {
            ss << lambda->params[0]->name;
        } else {
            ss << "(";
            for (size_t i = 0; i < lambda->params.size(); i++) {
                if (i > 0) ss << ", ";
                ss << lambda->params[i]->name;
            }
            ss << ")";
        }

        ss << " => ";

        if (lambda->isBlock) {
            ss << "{\n";
            for (auto& stmt : lambda->blockBody) {
                ss << "  " << format_statement(stmt.get(), 1) << "\n";
            }
            ss << "}";
        } else {
            ss << format_expression(lambda->exprBody.get());
        }

        return ss.str();
    }

    if (auto await_expr = dynamic_cast<AwaitExpressionNode*>(expr)) {
        return "subiri " + format_expression(await_expr->expression.get());
    }

    if (auto yield_expr = dynamic_cast<YieldExpressionNode*>(expr)) {
        return "yield " + format_expression(yield_expr->expression.get());
    }

    if (auto new_expr = dynamic_cast<NewExpressionNode*>(expr)) {
        std::ostringstream ss;
        ss << "unda " << format_expression(new_expr->callee.get()) << "(";
        for (size_t i = 0; i < new_expr->arguments.size(); i++) {
            if (i > 0) ss << ", ";
            ss << format_expression(new_expr->arguments[i].get());
        }
        ss << ")";
        return ss.str();
    }

    if (auto del_expr = dynamic_cast<DeleteExpressionNode*>(expr)) {
        return "futa " + format_expression(del_expr->target.get());
    }

    if (auto super_expr = dynamic_cast<SuperExpressionNode*>(expr)) {
        std::ostringstream ss;
        ss << "supa(";
        for (size_t i = 0; i < super_expr->arguments.size(); i++) {
            if (i > 0) ss << ", ";
            ss << format_expression(super_expr->arguments[i].get());
        }
        ss << ")";
        return ss.str();
    }

    if (auto assign_expr = dynamic_cast<AssignmentExpressionNode*>(expr)) {
        return assign_expr->target_name + " ni " + format_expression(assign_expr->value.get());
    }

    return "/* unknown expr */";
}
