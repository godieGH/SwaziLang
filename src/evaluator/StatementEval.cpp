#include <cmath>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "ClassRuntime.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"

static std::string to_property_key(const Value& v, Token token) {
    // string first
    if (auto ps = std::get_if<std::string>(&v)) {
        return *ps;
    }

    // number -> canonical integer if whole, otherwise decimal string
    if (auto pd = std::get_if<double>(&v)) {
        double d = *pd;
        if (!std::isfinite(d)) {
            throw SwaziError(
                "TypeError",
                "Invalid number for property key — must be finite.",
                token.loc);
        }
        double floor_d = std::floor(d);
        if (d == floor_d) {
            // whole number — print as integer to match typical JS-like semantics
            return std::to_string(static_cast<long long>(d));
        }
        return std::to_string(d);
    }

    // boolean
    if (auto pb = std::get_if<bool>(&v)) {
        return *pb ? "kweli" : "sikweli";
    }

    // null/undefined handling: depending on your Value types implement accordingly.
    // If you have a NullPtr/UndefinedPtr type, handle here (example below is generic)
    // if (std::holds_alternative<NullPtr>(v)) return "null";

    throw SwaziError(
        "TypeError",
        "Cannot convert value to a property key — unsupported type.",
        token.loc);
}
// ----------------- Statement evaluation -----------------
void Evaluator::evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value, bool* did_return, LoopControl* lc) {
    if (!stmt) return;

    if (auto imp = dynamic_cast<ImportDeclarationNode*>(stmt)) {
        // Load the module (may return an exports object even for circular deps)
        ObjectPtr exports = import_module(imp->module_path, imp->module_token, env);

        // If side-effect-only with no specifiers (tumia "./file.sl"), bring all exported names into current env
        if (imp->side_effect_only && imp->specifiers.empty() && !imp->import_all) {
            if (!exports) return;
            for (const auto& kv : exports->properties) {
                const std::string& name = kv.first;
                const PropertyDescriptor& pd = kv.second;
                Environment::Variable var;
                var.value = pd.value;
                var.is_constant = false;
                env->set(name, var);
            }
            return;
        }

        // If import_all (tumia * kutoka "./file.sl"), bring all exported names under their own names into current env
        if (imp->import_all) {
            if (!exports) return;
            for (const auto& kv : exports->properties) {
                const std::string& name = kv.first;
                const PropertyDescriptor& pd = kv.second;
                Environment::Variable var;
                var.value = pd.value;
                var.is_constant = false;
                env->set(name, var);
            }
            return;
        }

        // Otherwise named/default specifiers
        for (const auto& sptr : imp->specifiers) {
            if (!sptr) continue;
            std::string requested = sptr->imported;  // "default" for default import
            std::string local = sptr->local;

            Value v = std::monostate{};

            if (requested == "default") {
                // Default-style import semantics:
                // - If module has an explicit "default" export -> use it.
                // - Otherwise bind the import name to the module's exports object (so tumia ddd from "./mod"
                //   gives you an object of the named exports).
                if (exports) {
                    auto it = exports->properties.find("default");
                    if (it != exports->properties.end()) {
                        v = it->second.value;
                    } else {
                        // no explicit default -> bind the entire exports object
                        v = exports;
                    }
                } else {
                    v = std::monostate{};
                }
            } else {
                // Named import: look up the export by name; missing -> undefined
                if (exports) {
                    auto it = exports->properties.find(requested);
                    if (it != exports->properties.end()) {
                        v = it->second.value;
                    } else {
                        v = std::monostate{};
                    }
                }
            }

            Environment::Variable var;
            var.value = v;
            var.is_constant = false;
            env->set(local, var);
        }

        return;
    }
    if (auto ed = dynamic_cast<ExportDeclarationNode*>(stmt)) {
        return;
    }

    if (auto vd = dynamic_cast<VariableDeclarationNode*>(stmt)) {
        Value val = std::monostate{};
        if (vd->value) val = evaluate_expression(vd->value.get(), env);

        // If this declaration uses a destructuring pattern, route to binder
        if (vd->pattern) {
            // For constants, parser already enforced an initializer is required.
            // However, ensure we don't allow uninitialized constant elements here:
            if (vd->is_constant && std::holds_alternative<std::monostate>(val)) {
                throw SwaziError(
                    "SyntaxError",
                    "Constant pattern must be initialized.",
                    vd->token.loc);
            }
            bind_pattern_to_value(vd->pattern.get(), val, env, vd->is_constant, vd->token);
            return;
        }

        // Otherwise simple identifier declaration (existing behavior)
        Environment::Variable var{
            val,
            vd->is_constant};
        if (vd->is_constant && std::holds_alternative<std::monostate>(val)) {
            throw SwaziError(
                "SyntaxError",
                "Constant '" + vd->identifier + "' must be initialized.",
                vd->token.loc);
        }
        env->set(vd->identifier, var);
        return;
    }
    // Assignment: target is now an ExpressionNode (IdentifierNode / IndexExpressionNode / MemberExpressionNode)
    if (auto an = dynamic_cast<AssignmentNode*>(stmt)) {
        Value rhs = evaluate_expression(an->value.get(), env);

        // Identifier target: update variable in enclosing environment (search up chain) or create in current env
        if (auto id = dynamic_cast<IdentifierNode*>(an->target.get())) {
            EnvPtr walk = env;
            while (walk) {
                auto it = walk->values.find(id->name);
                if (it != walk->values.end()) {
                    if (it->second.is_constant)
                        throw std::runtime_error(
                            "TypeError at " + id->token.loc.to_string() +
                            "\nCannot assign to constant '" + id->name + "'." +
                            "\n --> Traced at:\n" + id->token.loc.get_line_trace());
                    it->second.value = rhs;
                    return;
                }
                walk = walk->parent;
            }
            // not found -> create in current env
            Environment::Variable var{
                rhs,
                false};
            env->set(id->name, var);
            return;
        }

        if (auto idx = dynamic_cast<IndexExpressionNode*>(an->target.get())) {
            Value objVal = evaluate_expression(idx->object.get(), env);
            Value indexVal = evaluate_expression(idx->index.get(), env);

            // array path (unchanged)
            if (std::holds_alternative<ArrayPtr>(objVal)) {
                long long rawIndex = static_cast<long long>(to_number(indexVal, idx->token));
                ArrayPtr arr = std::get<ArrayPtr>(objVal);
                if (!arr) {
                    throw std::runtime_error(
                        "TypeError at " + idx->token.loc.to_string() +
                        "\nCannot assign into null array." +
                        "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                }
                if (rawIndex < 0) {
                    throw std::runtime_error(
                        "TypeError at " + idx->token.loc.to_string() +
                        "\nNegative array index not supported." +
                        "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
                }
                size_t uidx = static_cast<size_t>(rawIndex);
                if (uidx >= arr->elements.size()) arr->elements.resize(uidx + 1);
                arr->elements[uidx] = rhs;
                return;
            }

            // object property path: o[key] = rhs
            if (std::holds_alternative<ObjectPtr>(objVal)) {
    ObjectPtr op = std::get<ObjectPtr>(objVal);
    std::string prop = to_property_key(indexVal, idx->token);  // convert index -> property key
    // Delegate creation/permission checks/frozen/proxy handling to the helper
    set_object_property(op, prop, rhs, env, idx->token);
    return;
}
            throw std::runtime_error(
                "TypeError at " + idx->token.loc.to_string() +
                "\nAttempted index assignment on non-array/non-object value." +
                "\n --> Traced at:\n" + idx->token.loc.get_line_trace());
        }

        if (auto mem = dynamic_cast<MemberExpressionNode*>(an->target.get())) {
    Value objVal = evaluate_expression(mem->object.get(), env);

    // class static member assignment
    if (std::holds_alternative<ClassPtr>(objVal)) {
        ClassPtr cls = std::get<ClassPtr>(objVal);
        if (!cls) {
            throw std::runtime_error(
                "TypeError at " + mem->token.loc.to_string() +
                "\nMember assignment on null class." +
                "\n --> Traced at:\n" + mem->token.loc.get_line_trace());
        }
        set_object_property(cls->static_table, mem->property, rhs, env, mem->token);
        return;
    }

    // normal object member assignment (centralized)
    if (std::holds_alternative<ObjectPtr>(objVal)) {
        ObjectPtr op = std::get<ObjectPtr>(objVal);
        set_object_property(op, mem->property, rhs, env, mem->token);
        return;
    }

    // clearer error for non-object/non-class member assignment
    throw std::runtime_error(
        "TypeError at " + mem->token.loc.to_string() +
        "\nMember assignment on non-object value." +
        "\n --> Traced at:\n" + mem->token.loc.get_line_trace());
}
        throw std::runtime_error(
            "TypeError at " + an->token.loc.to_string() +
            "\nUnsupported assignment target." +
            "\n --> Traced at:\n" + an->token.loc.get_line_trace());
    }

    if (auto ps = dynamic_cast<PrintStatementNode*>(stmt)) {
        std::string out;
        for (size_t i = 0; i < ps->expressions.size(); ++i) {
            out += to_string_value(evaluate_expression(ps->expressions[i].get(), env));
            if (i + 1 < ps->expressions.size()) out += " ";
        }
        if (ps->newline)
            std::cout << out << std::endl;
        else
            std::cout << out;
        return;
    }

    if (auto es = dynamic_cast<ExpressionStatementNode*>(stmt)) {
        evaluate_expression(es->expression.get(), env);
        return;
    }

    if (auto fd = dynamic_cast<FunctionDeclarationNode*>(stmt)) {
        // Persist a fresh FunctionDeclarationNode and clone parameter descriptors so
        // the evaluator owns independent ParameterNode instances.
        auto persisted = std::make_shared<FunctionDeclarationNode>();
        persisted->token = fd->token;
        persisted->name = fd->name;

        // Clone parameter descriptors (fd->parameters is vector<unique_ptr<ParameterNode>>)
        persisted->parameters.reserve(fd->parameters.size());
        for (const auto& p : fd->parameters) {
            if (p)
                persisted->parameters.push_back(p->clone());
            else
                persisted->parameters.push_back(nullptr);
        }

        // Move or clone the body into persisted (we can move here since fd is ephemeral)
        persisted->body.reserve(fd->body.size());
        for (const auto& s : fd->body) {
            persisted->body.push_back(s ? s->clone() : nullptr);
        }

        // Construct FunctionValue from persisted declaration (FunctionValue ctor will
        // clone the parameter descriptors into its own storage as required).
        auto fn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, env, persisted->token);
        Environment::Variable var{fn, true};
        env->set(persisted->name, var);
        return;
    }

    if (auto cd = dynamic_cast<ClassDeclarationNode*>(stmt)) {
        // create runtime class descriptor
        auto classDesc = std::make_shared<ClassValue>();
        classDesc->token = cd->token;
        classDesc->name = cd->name ? cd->name->name : "<lamda>";
        if (cd->body) {
            classDesc->body = cd->body->clone();
        }

        classDesc->defining_env = env;

        // resolve super if present
        if (cd->superClass) {
            // try to get super class from environment
            EnvPtr walk = env;
            bool found = false;
            while (walk) {
                auto it = walk->values.find(cd->superClass->name);
                if (it != walk->values.end()) {
                    if (std::holds_alternative<ClassPtr>(it->second.value)) {
                        classDesc->super = std::get<ClassPtr>(it->second.value);
                        found = true;
                    } else {
                        throw SwaziError(
                            "TypeError",
                            "Super identifier '" + cd->superClass->name + "' is not a class.",
                            cd->superClass->token.loc);
                    }
                    break;
                }
                walk = walk->parent;
            }
            if (!found) {
                throw SwaziError(
                    "ReferenceError",
                    "Unknown super class '" + cd->superClass->name + "'.",
                    cd->superClass->token.loc);
            }
        }
        {
            Environment::Variable earlyVar;
            earlyVar.value = classDesc;
            earlyVar.is_constant = true;
            env->set(classDesc->name, earlyVar);
        }
        // materialize static table: iterate properties and methods in the ClassBodyNode clone
        if (classDesc->body) {
            // properties
            for (auto& p_uptr : classDesc->body->properties) {
                if (!p_uptr) continue;
                if (p_uptr->is_static) {
                    // evaluate initializer if present
                    Value initVal = std::monostate{};
                    if (p_uptr->value) initVal = evaluate_expression(p_uptr->value.get(), env);
                    PropertyDescriptor pd;
                    pd.value = initVal;
                    pd.is_private = p_uptr->is_private;
                    pd.is_locked = p_uptr->is_locked;
                    // ClassPropertyNode currently doesn't have an 'is_readonly' flag in AST,
                    // so set readonly = false for plain static properties (static getters are methods).
                    pd.is_readonly = false;
                    pd.token = p_uptr->token;
                    classDesc->static_table->properties[p_uptr->name] = std::move(pd);
                }
            }

            // methods
            for (auto& m_uptr : classDesc->body->methods) {
                if (!m_uptr) continue;
                if (m_uptr->is_static) {
                    // create a FunctionDeclarationNode persisted from the ClassMethodNode (existing code above)
                    auto persisted = std::make_shared<FunctionDeclarationNode>();
                    persisted->name = m_uptr->name;
                    persisted->token = m_uptr->token;
                    persisted->parameters.reserve(m_uptr->params.size());
                    for (const auto& pp : m_uptr->params) {
                        if (pp)
                            persisted->parameters.push_back(pp->clone());
                        else
                            persisted->parameters.push_back(nullptr);
                    }
                    persisted->body.reserve(m_uptr->body.size());
                    for (const auto& s : m_uptr->body) persisted->body.push_back(s ? s->clone() : nullptr);

                    // --- create a closure where "$" is bound to the class's static_table ---
                    EnvPtr staticClosure = std::make_shared<Environment>(classDesc->defining_env);
                    Environment::Variable classThisVar;
                    classThisVar.value = classDesc->static_table;  // bind $ -> static table object
                    classThisVar.is_constant = true;
                    staticClosure->set("$", classThisVar);

                    // create the FunctionValue with closure = staticClosure so inside static methods/getters "$" resolves to the class static object
                    auto fn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, staticClosure, persisted->token);

                    PropertyDescriptor pd;
                    pd.value = fn;
                    pd.is_private = m_uptr->is_private;
                    pd.is_locked = m_uptr->is_locked;
                    pd.is_readonly = m_uptr->is_getter;  // getter on static method makes it readonly
                    pd.token = m_uptr->token;
                    classDesc->static_table->properties[m_uptr->name] = std::move(pd);
                }
            }
        }

        // store class descriptor into current env as a constant
        Environment::Variable var;
        var.value = classDesc;
        var.is_constant = true;
        env->set(classDesc->name, var);
        return;
    }
    if (auto ds = dynamic_cast<DeleteStatementNode*>(stmt)) {
        // evaluate the delete expression to call destructor and cleanup
        // Evaluate inner expression which should be a DeleteExpressionNode
        if (!ds->expr) return;
        // Evaluate target object value
        Value v = evaluate_expression(ds->expr->target.get(), env);
        if (!std::holds_alternative<ObjectPtr>(v)) {
            // nothing to delete or error depending on language semantics
            return;
        }
        ObjectPtr obj = std::get<ObjectPtr>(v);

        // evaluate destructor args (if any)
        std::vector<Value> args;
        args.reserve(ds->expr->arguments.size());
        for (auto& aexpr : ds->expr->arguments) {
            if (!aexpr)
                args.push_back(std::monostate{});
            else
                args.push_back(evaluate_expression(aexpr.get(), env));
        }

        // try to find class meta on object: __class__ property
        auto it = obj->properties.find("__class__");
        if (it != obj->properties.end() && std::holds_alternative<ClassPtr>(it->second.value)) {
            ClassPtr cls = std::get<ClassPtr>(it->second.value);
            // find destructor in cls->body
            if (cls->body) {
                for (auto& m : cls->body->methods) {
                    if (m && m->is_destructor) {
                        // create FunctionPtr and call it with receiver and args
                        auto persisted = std::make_shared<FunctionDeclarationNode>();
                        persisted->name = m->name;
                        persisted->token = m->token;

                        // clone parameter descriptors from method node
                        persisted->parameters.reserve(m->params.size());
                        for (const auto& pp : m->params) {
                            if (pp)
                                persisted->parameters.push_back(pp->clone());
                            else
                                persisted->parameters.push_back(nullptr);
                        }

                        persisted->body.reserve(m->body.size());
                        for (const auto& s : m->body) persisted->body.push_back(s ? s->clone() : nullptr);

                        auto fn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, env, persisted->token);

                        // call with receiver bound and forward args
                        call_function_with_receiver(fn, obj, args, env, m->token);
                        break;
                    }
                }
            }
        }
        // Cleanup: remove properties so object is effectively destroyed
        obj->properties.clear();
        return;
    }

    if (auto rs = dynamic_cast<ReturnStatementNode*>(stmt)) {
        if (did_return) *did_return = true;
        if (return_value) {
            *return_value = rs->value ? evaluate_expression(rs->value.get(), env) : std::monostate{};
        }
        return;
    }

    if (auto ifn = dynamic_cast<IfStatementNode*>(stmt)) {
        Value condVal = evaluate_expression(ifn->condition.get(), env);
        if (to_bool(condVal)) {
            auto blockEnv = std::make_shared<Environment>(env);
            for (auto& s : ifn->then_body) {
                evaluate_statement(s.get(), blockEnv, return_value, did_return, lc);
                if (did_return && *did_return) return;
            }
        } else if (ifn->has_else) {
            auto blockEnv = std::make_shared<Environment>(env);
            for (auto& s : ifn->else_body) {
                evaluate_statement(s.get(), blockEnv, return_value, did_return, lc);
                if (did_return && *did_return) return;
            }
        }
        return;
    }

    // --- ForStatementNode (kwa) ---
    if (auto fn = dynamic_cast<ForStatementNode*>(stmt)) {
        auto forEnv = std::make_shared<Environment>(env);

        // ensure a loop-control exists for this loop (use caller's lc if present)
        LoopControl local_lc;
        LoopControl* loopCtrl = lc ? lc : &local_lc;

        if (fn->init) {
            evaluate_statement(fn->init.get(), forEnv, nullptr, nullptr, loopCtrl);
            // if init triggered a return it would have returned earlier
        }

        while (true) {
            // condition check
            if (fn->condition) {
                Value condVal = evaluate_expression(fn->condition.get(), forEnv);
                if (!to_bool(condVal)) break;
            }

            auto bodyEnv = std::make_shared<Environment>(forEnv);

            // run body
            for (auto& s : fn->body) {
                evaluate_statement(s.get(), bodyEnv, return_value, did_return, loopCtrl);
                if (did_return && *did_return) return;
                if (loopCtrl->did_break || loopCtrl->did_continue) break;
            }

            // handle break
            if (loopCtrl->did_break) {
                loopCtrl->did_break = false;  // reset for outer loops
                break;
            }

            // handle continue: run post (if any) then next iteration
            if (loopCtrl->did_continue) {
                loopCtrl->did_continue = false;
                if (fn->post) evaluate_expression(fn->post.get(), forEnv);
                continue;
            }

            // normal end-of-iteration: run post then loop
            if (fn->post) evaluate_expression(fn->post.get(), forEnv);
        }

        return;
    }

    // --- ForInStatementNode (kwa kila) ---
    if (auto fin = dynamic_cast<ForInStatementNode*>(stmt)) {
        Value iterableVal = evaluate_expression(fin->iterable.get(), env);

        // loop control owner
        LoopControl local_lc;
        LoopControl* loopCtrl = lc ? lc : &local_lc;

        // Array case
        if (std::holds_alternative<ArrayPtr>(iterableVal)) {
            ArrayPtr arr = std::get<ArrayPtr>(iterableVal);
            if (!arr) return;

            for (size_t i = 0; i < arr->elements.size(); ++i) {
                auto loopEnv = std::make_shared<Environment>(env);

                if (fin->valueVar) {
                    Environment::Variable var{
                        arr->elements[i],
                        false};
                    loopEnv->set(fin->valueVar->name, var);
                }
                if (fin->indexVar) {
                    Environment::Variable var{
                        static_cast<double>(i),
                        false};
                    loopEnv->set(fin->indexVar->name, var);
                }

                // execute body
                for (auto& s : fin->body) {
                    evaluate_statement(s.get(), loopEnv, return_value, did_return, loopCtrl);
                    if (did_return && *did_return) return;
                    if (loopCtrl->did_break || loopCtrl->did_continue) break;
                }

                // if break -> stop iterating the array
                if (loopCtrl->did_break) {
                    loopCtrl->did_break = false;
                    break;
                }

                // if continue -> reset and proceed to next element
                if (loopCtrl->did_continue) {
                    loopCtrl->did_continue = false;
                    continue;
                }
            }

            return;
        }

        // Object case
        else if (std::holds_alternative<ObjectPtr>(iterableVal)) {
            ObjectPtr obj = std::get<ObjectPtr>(iterableVal);
            if (!obj) return;

            for (auto& p : obj->properties) {
                auto loopEnv = std::make_shared<Environment>(env);

                // per your design: valueVar = key, indexVar = value
                if (fin->valueVar) {
                    Environment::Variable var{
                        p.first,
                        false};  // key (string)
                    loopEnv->set(fin->valueVar->name, var);
                }
                if (fin->indexVar) {
                    Environment::Variable var{
                        p.second.value,
                        false};  // value
                    loopEnv->set(fin->indexVar->name, var);
                }

                for (auto& s : fin->body) {
                    evaluate_statement(s.get(), loopEnv, return_value, did_return, loopCtrl);
                    if (did_return && *did_return) return;
                    if (loopCtrl->did_break || loopCtrl->did_continue) break;
                }

                if (loopCtrl->did_break) {
                    loopCtrl->did_break = false;
                    break;
                }
                if (loopCtrl->did_continue) {
                    loopCtrl->did_continue = false;
                    continue;
                }
            }

            return;
        }

        else {
            throw SwaziError(
                "TypeError",
                "Cannot iterate over a non-array/non-object value in 'kwa kila' loop.",
                fin->token.loc);
        }
    }

    // --- WhileStatementNode (wakati) ---
    if (auto wn = dynamic_cast<WhileStatementNode*>(stmt)) {
        LoopControl local_lc;
        LoopControl* loopCtrl = lc ? lc : &local_lc;

        while (true) {
            Value condVal = evaluate_expression(wn->condition.get(), env);
            if (!to_bool(condVal)) break;

            auto bodyEnv = std::make_shared<Environment>(env);

            for (auto& s : wn->body) {
                evaluate_statement(s.get(), bodyEnv, return_value, did_return, loopCtrl);
                if (did_return && *did_return) return;
                if (loopCtrl->did_break || loopCtrl->did_continue) break;
            }

            if (loopCtrl->did_break) {
                loopCtrl->did_break = false;
                break;
            }
            if (loopCtrl->did_continue) {
                loopCtrl->did_continue = false;
                continue;
            }
        }

        return;
    }

    // --- DoWhileStatementNode (fanya-wakati) ---
    if (auto dwn = dynamic_cast<DoWhileStatementNode*>(stmt)) {
        LoopControl local_lc;
        LoopControl* loopCtrl = lc ? lc : &local_lc;

        do {
            auto bodyEnv = std::make_shared<Environment>(env);

            for (auto& s : dwn->body) {
                evaluate_statement(s.get(), bodyEnv, return_value, did_return, loopCtrl);
                if (did_return && *did_return) return;
                if (loopCtrl->did_break || loopCtrl->did_continue) break;
            }

            if (loopCtrl->did_break) {
                loopCtrl->did_break = false;
                break;
            }
            if (loopCtrl->did_continue) {
                loopCtrl->did_continue = false;
                // continue -> evaluate condition then maybe loop again (like normal continue)
                Value condVal = evaluate_expression(dwn->condition.get(), bodyEnv);
                if (!to_bool(condVal))
                    break;
                else
                    continue;
            }

            // normal case: test condition for next iteration
            Value condVal = evaluate_expression(dwn->condition.get(), bodyEnv);
            if (!to_bool(condVal)) break;

        } while (true);

        return;
    }

    if (auto bs = dynamic_cast<BreakStatementNode*>(stmt)) {
        if (lc) lc->did_break = true;
        return;
    }

    if (auto cs = dynamic_cast<ContinueStatementNode*>(stmt)) {
        if (lc) lc->did_continue = true;
        return;
    }

    // --- DoStatementNode (fanya) ---
    if (auto dn = dynamic_cast<DoStatementNode*>(stmt)) {
        auto bodyEnv = std::make_shared<Environment>(env);

        for (auto& s : dn->body) {
            evaluate_statement(s.get(), bodyEnv, return_value, did_return, lc);
            if (did_return && *did_return) return;
            if (lc && (lc->did_break || lc->did_continue)) return;
        }

        return;
    }

    if (auto sn = dynamic_cast<SwitchNode*>(stmt)) {
        // Evaluate the discriminant (the switch condition expression)
        Value switchVal = evaluate_expression(sn->discriminant.get(), env);

        CaseNode* defaultCase = nullptr;
        bool matched = false;

        LoopControl local_lc;
        LoopControl* loopCtrl = lc ? lc : &local_lc;

        for (auto& casePtr : sn->cases) {
            CaseNode* cn = casePtr.get();

            // If this is the default case (kaida), remember it for later
            if (!cn->test) {
                defaultCase = cn;
                continue;
            }

            // Evaluate case test expression
            Value caseVal = evaluate_expression(cn->test.get(), env);

            // Check equality (reuse your equals() or same helper you use elsewhere)
            if (!matched && is_equal(switchVal, caseVal)) {
                matched = true;
            }

            // If already matched, execute this case body (fall-through unless simama)
            if (matched) {
                auto bodyEnv = std::make_shared<Environment>(env);

                for (auto& s : cn->body) {
                    evaluate_statement(s.get(), bodyEnv, return_value, did_return, loopCtrl);
                    if (did_return && *did_return) return;
                    if (loopCtrl->did_break) {
                        loopCtrl->did_break = false;  // reset for outer flow
                        return;                       // exit entire switch
                    }
                }
            }
        }

        // No case matched, execute default if present
        if (!matched && defaultCase) {
            auto bodyEnv = std::make_shared<Environment>(env);

            for (auto& s : defaultCase->body) {
                evaluate_statement(s.get(), bodyEnv, return_value, did_return, loopCtrl);
                if (did_return && *did_return) return;
                if (loopCtrl->did_break) {
                    loopCtrl->did_break = false;
                    return;  // exit switch
                }
            }
        }

        return;
    }

    if (auto tcf = dynamic_cast<TryCatchNode*>(stmt)) {
        bool hadException = false;
        std::exception_ptr eptr;

        // Try block (use a separate env)
        {
            auto tryEnv = std::make_shared<Environment>(env);
            try {
                for (auto& s : tcf->tryBlock) {
                    evaluate_statement(s.get(), tryEnv, return_value, did_return, lc);
                    // don't return here — break so finally can run
                    if (did_return && *did_return) break;
                }
            } catch (...) {
                // capture exception for catch handling
                hadException = true;
                eptr = std::current_exception();
            }
        }

        // Catch block (if an exception occurred)
        if (hadException) {
            auto catchEnv = std::make_shared<Environment>(env);

            // bind errorVar if provided
            if (!tcf->errorVar.empty()) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception& e) {
                    Environment::Variable var{std::string(e.what()), false};
                    catchEnv->set(tcf->errorVar, var);
                } catch (...) {
                    Environment::Variable var{std::string("non-standard exception"), false};
                    catchEnv->set(tcf->errorVar, var);
                }
            }

            for (auto& s : tcf->catchBlock) {
                evaluate_statement(s.get(), catchEnv, return_value, did_return, lc);
                if (did_return && *did_return) break;
            }
        }

        // Finally block — ALWAYS run (if non-empty)
        if (!tcf->finallyBlock.empty()) {
            auto finallyEnv = std::make_shared<Environment>(env);
            for (auto& s : tcf->finallyBlock) {
                evaluate_statement(s.get(), finallyEnv, return_value, did_return, lc);
                if (did_return && *did_return) break;
            }
        }

        return;
    }

    throw SwaziError(
        "InternalError",
        "Unhandled statement node encountered in evaluator — likely a bug in the interpreter.",
        stmt->token.loc);
}