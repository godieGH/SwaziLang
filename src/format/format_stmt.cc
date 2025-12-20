#include "ast.hpp"
#include "format/format.hpp"
#include "lexer.hpp"
#include "parser.hpp"
#include "token.hpp"

std::string format_statement(StatementNode* stmt, int depth) {
    if (!stmt) return "";

    std::string indent(depth * 2, ' ');

    if (auto vd = dynamic_cast<VariableDeclarationNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "data ";
        if (vd->is_constant) ss << "thabiti ";
        ss << vd->identifier;
        if (vd->value) {
            ss << " = " << format_expression(vd->value.get());
        }
        ss << ";";
        return ss.str();
    }

    if (auto an = dynamic_cast<AssignmentNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << format_expression(an->target.get());
        ss << " = " << format_expression(an->value.get());
        ss << ";";
        return ss.str();
    }

    if (auto ps = dynamic_cast<PrintStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << (ps->newline ? "chapisha" : "andika");
        if (!ps->expressions.empty()) {
            ss << "(";
            for (size_t i = 0; i < ps->expressions.size(); i++) {
                if (i > 0) ss << ", ";
                ss << format_expression(ps->expressions[i].get());
            }
            ss << ")";
        }
        ss << ";";
        return ss.str();
    }

    if (auto es = dynamic_cast<ExpressionStatementNode*>(stmt)) {
        return indent + format_expression(es->expression.get()) + ";";
    }

    if (auto fd = dynamic_cast<FunctionDeclarationNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "kazi";
        if (fd->is_generator) ss << "*";
        if (fd->is_async) ss << " async";
        ss << " " << fd->name << "(";

        for (size_t i = 0; i < fd->parameters.size(); i++) {
            if (i > 0) ss << ", ";
            auto& p = fd->parameters[i];
            if (p->is_rest) ss << "...";
            ss << p->name;
            if (p->defaultValue) {
                ss << " = " << format_expression(p->defaultValue.get());
            }
        }

        ss << ") {\n";
        for (auto& s : fd->body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";
        return ss.str();
    }

    if (auto rs = dynamic_cast<ReturnStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "rudisha";
        if (rs->value) {
            ss << " " << format_expression(rs->value.get());
        }
        ss << ";";
        return ss.str();
    }

    if (auto ifn = dynamic_cast<IfStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "kama " << format_expression(ifn->condition.get()) << " {\n";
        for (auto& s : ifn->then_body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";

        if (ifn->has_else) {
            ss << " vinginevyo {\n";
            for (auto& s : ifn->else_body) {
                ss << format_statement(s.get(), depth + 1) << "\n";
            }
            ss << indent << "}";
        }
        return ss.str();
    }

    if (auto fn = dynamic_cast<ForStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "kwa (";
        if (fn->init) ss << format_statement(fn->init.get(), 0);
        ss << " ";
        if (fn->condition) ss << format_expression(fn->condition.get());
        ss << "; ";
        if (fn->post) ss << format_expression(fn->post.get());
        ss << ") {\n";
        for (auto& s : fn->body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";
        return ss.str();
    }

    if (auto fin = dynamic_cast<ForInStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "kwa kila " << fin->valueVar->name;
        if (fin->indexVar) {
            ss << ", " << fin->indexVar->name;
        }
        ss << " katika " << format_expression(fin->iterable.get()) << " {\n";
        for (auto& s : fin->body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";
        return ss.str();
    }

    if (auto wn = dynamic_cast<WhileStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "wakati " << format_expression(wn->condition.get()) << " {\n";
        for (auto& s : wn->body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";
        return ss.str();
    }

    if (auto dwn = dynamic_cast<DoWhileStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "fanya {\n";
        for (auto& s : dwn->body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "} wakati " << format_expression(dwn->condition.get()) << ";";
        return ss.str();
    }

    if (auto dn = dynamic_cast<DoStatementNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "fanya {\n";
        for (auto& s : dn->body) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";
        return ss.str();
    }

    if (dynamic_cast<BreakStatementNode*>(stmt)) {
        return indent + "simama;";
    }

    if (dynamic_cast<ContinueStatementNode*>(stmt)) {
        return indent + "endelea;";
    }

    if (auto cd = dynamic_cast<ClassDeclarationNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "muundo " << cd->name->name;
        if (cd->superClass) {
            ss << " rithi " << cd->superClass->name;
        }
        ss << " {\n";

        if (cd->body) {
            // Properties
            for (auto& prop : cd->body->properties) {
                ss << std::string((depth + 1) * 2, ' ');
                if (prop->is_static) ss << "*";
                if (prop->is_private) ss << "@";
                if (prop->is_locked) ss << "&";
                ss << prop->name;
                if (prop->value) {
                    ss << " = " << format_expression(prop->value.get());
                }
                ss << ";\n";
            }

            // Methods
            for (auto& method : cd->body->methods) {
                ss << std::string((depth + 1) * 2, ' ');
                if (method->is_static) ss << "*";
                if (method->is_private) ss << "@";
                if (method->is_locked) ss << "&";

                if (method->is_constructor) {
                    ss << method->name;
                } else if (method->is_destructor) {
                    ss << "~" << method->name;
                } else {
                    if (method->is_async) ss << "async ";
                    ss << "tabia ";
                    if (method->is_getter) ss << "thabiti ";
                    ss << method->name;
                }

                if (!method->is_getter) {
                    ss << "(";
                    for (size_t i = 0; i < method->params.size(); i++) {
                        if (i > 0) ss << ", ";
                        ss << method->params[i]->name;
                    }
                    ss << ")";
                }

                ss << " {\n";
                for (auto& s : method->body) {
                    ss << format_statement(s.get(), depth + 2) << "\n";
                }
                ss << std::string((depth + 1) * 2, ' ') << "}\n";
            }
        }

        ss << indent << "}";
        return ss.str();
    }

    if (auto sn = dynamic_cast<SwitchNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "chagua " << format_expression(sn->discriminant.get()) << " {\n";

        for (auto& case_node : sn->cases) {
            ss << std::string((depth + 1) * 2, ' ');
            if (case_node->test) {
                ss << "ikiwa " << format_expression(case_node->test.get());
            } else {
                ss << "kaida";
            }
            ss << " {\n";
            for (auto& s : case_node->body) {
                ss << format_statement(s.get(), depth + 2) << "\n";
            }
            ss << std::string((depth + 1) * 2, ' ') << "}\n";
        }

        ss << indent << "}";
        return ss.str();
    }

    if (auto tcn = dynamic_cast<TryCatchNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "jaribu {\n";
        for (auto& s : tcn->tryBlock) {
            ss << format_statement(s.get(), depth + 1) << "\n";
        }
        ss << indent << "}";

        if (!tcn->catchBlock.empty()) {
            ss << " makosa " << tcn->errorVar << " {\n";
            for (auto& s : tcn->catchBlock) {
                ss << format_statement(s.get(), depth + 1) << "\n";
            }
            ss << indent << "}";
        }

        if (!tcn->finallyBlock.empty()) {
            ss << " kisha {\n";
            for (auto& s : tcn->finallyBlock) {
                ss << format_statement(s.get(), depth + 1) << "\n";
            }
            ss << indent << "}";
        }

        return ss.str();
    }

    if (auto ts = dynamic_cast<ThrowStatementNode*>(stmt)) {
        return indent + "tupa " + format_expression(ts->value.get()) + ";";
    }

    if (auto imp = dynamic_cast<ImportDeclarationNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "tumia ";

        if (imp->import_all) {
            ss << "*";
        } else if (!imp->specifiers.empty()) {
            ss << "{";
            for (size_t i = 0; i < imp->specifiers.size(); i++) {
                if (i > 0) ss << ", ";
                ss << imp->specifiers[i]->imported;
                if (imp->specifiers[i]->local != imp->specifiers[i]->imported) {
                    ss << " kama " << imp->specifiers[i]->local;
                }
            }
            ss << "}";
        }

        if (!imp->side_effect_only || imp->import_all) {
            ss << " kutoka";
        }
        ss << " \"" << imp->module_path << "\";";
        return ss.str();
    }

    if (auto exp = dynamic_cast<ExportDeclarationNode*>(stmt)) {
        std::ostringstream ss;
        ss << indent << "ruhusu ";

        if (exp->is_default) {
            ss << exp->single_identifier;
        } else if (!exp->names.empty()) {
            ss << "{";
            for (size_t i = 0; i < exp->names.size(); i++) {
                if (i > 0) ss << ", ";
                ss << exp->names[i];
            }
            ss << "}";
        }
        ss << ";";
        return ss.str();
    }

    return indent + "/* unknown statement */";
}
