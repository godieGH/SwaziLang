#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "ClassRuntime.hpp"
#include "evaluator.hpp"

Value Evaluator::evaluate_expression(ExpressionNode* expr, EnvPtr env) {
    if (!expr) return std::monostate{};

    if (auto n = dynamic_cast<NumericLiteralNode*>(expr)) return Value{
        n->value};
    if (auto s = dynamic_cast<StringLiteralNode*>(expr)) return Value{
        s->value};

    if (auto nn = dynamic_cast<NullNode*>(expr)) {
        return Value(std::monostate{});
    }

    // Template literal evaluation: concatenate quasis and evaluated expressions.
    if (auto tpl = dynamic_cast<TemplateLiteralNode*>(expr)) {
        // quasis.size() is expected to be expressions.size() + 1, but tolerate mismatches.
        std::string out;
        size_t exprCount = tpl->expressions.size();
        size_t quasiCount = tpl->quasis.size();

        // Iterate through quasis and interleave expression values.
        for (size_t i = 0; i < quasiCount; ++i) {
            out += tpl->quasis[i];
            if (i < exprCount) {
                Value ev = evaluate_expression(tpl->expressions[i].get(), env);
                out += to_string_value(ev);
            }
        }

        return Value{
            out};
    }

    if (auto arrNode = dynamic_cast<ArrayExpressionNode*>(expr)) {
        auto arrVal = std::make_shared<ArrayValue>();
        arrVal->elements.reserve(arrNode->elements.size());

        for (auto& elemPtr : arrNode->elements) {
            if (!elemPtr) {
                // preserve undefined / empty slot behavior
                arrVal->elements.push_back(std::monostate{});
                continue;
            }

            // Support spread element nodes (parser should produce a SpreadElementNode)
            if (auto spread = dynamic_cast<SpreadElementNode*>(elemPtr.get())) {
                if (!spread->argument) {
                    throw std::runtime_error("Spread element missing argument at " + spread->token.loc.to_string());
                }
                Value v = evaluate_expression(spread->argument.get(), env);

                // Common behavior: only arrays are spread into array literal
                if (std::holds_alternative<ArrayPtr>(v)) {
                    ArrayPtr src = std::get<ArrayPtr>(v);
                    if (src) {
                        for (auto& e : src->elements) {
                            arrVal->elements.push_back(e);
                        }
                    }
                } else {
                    // you can change this to allow strings -> char elements, objects -> ???, etc.
                    throw std::runtime_error("Spread in array expects array value at " + spread->token.loc.to_string());
                }
                continue;
            }

            // normal (non-spread) element
            Value ev = evaluate_expression(elemPtr.get(), env);
            arrVal->elements.push_back(std::move(ev));
        }
        return Value{
            arrVal};
    }

    if (auto objNode = dynamic_cast<ObjectExpressionNode*>(expr)) {
        ObjectPtr obj = std::make_shared<ObjectValue>();

        // helper: convert FunctionExpressionNode -> FunctionDeclarationNode
        auto fnExprToDecl = [](FunctionExpressionNode* fe) -> std::shared_ptr<FunctionDeclarationNode> {
            auto declptr = std::make_shared<FunctionDeclarationNode>();
            declptr->token = fe->token;
            declptr->name = fe->name;

            // clone parameter descriptors (ParameterNode unique_ptrs)
            declptr->parameters.reserve(fe->parameters.size());
            for (const auto& pp : fe->parameters) {
                if (pp)
                    declptr->parameters.push_back(pp->clone());
                else
                    declptr->parameters.push_back(nullptr);
            }

            declptr->body.reserve(fe->body.size());
            for (const auto& s : fe->body) declptr->body.push_back(s ? s->clone() : nullptr);
            return declptr;
        };

        for (const auto& p : objNode->properties) {
            if (!p) continue;

            Value val;

            // Handle Spread
            if (p->kind == PropertyKind::Spread) {
                if (!p->value) continue;

                // Unwrap SpreadElementNode if needed
                if (auto spreadNode = dynamic_cast<SpreadElementNode*>(p->value.get())) {
                    val = evaluate_expression(spreadNode->argument.get(), env);
                } else {
                    val = evaluate_expression(p->value.get(), env);
                }

                if (!std::holds_alternative<ObjectPtr>(val)) {
                    throw std::runtime_error("Spread expects an object at " + p->token.loc.to_string());
                }

                ObjectPtr src = std::get<ObjectPtr>(val);
                if (!src) continue;

                for (const auto& kv : src->properties) {
                    obj->properties[kv.first] = kv.second;  // copy descriptor
                }
                continue;
            }

            // Determine property key
            std::string keyStr;
            if (p->computed) {
                if (!p->key) throw std::runtime_error("Computed property missing expression at " + p->token.loc.to_string());
                keyStr = to_string_value(evaluate_expression(p->key.get(), env));
            } else if (!p->key_name.empty()) {
                keyStr = p->key_name;
            } else if (p->key) {
                keyStr = to_string_value(evaluate_expression(p->key.get(), env));
            } else {
                throw std::runtime_error("Property with no key at " + p->token.loc.to_string());
            }

            // Build property value
            if (p->kind == PropertyKind::Method) {
                if (auto fe = dynamic_cast<FunctionExpressionNode*>(p->value.get())) {
                    auto declptr = fnExprToDecl(fe);
                    EnvPtr methodClosure = std::make_shared<Environment>(env);
                    Environment::Variable thisVar;
                    thisVar.value = obj;
                    thisVar.is_constant = true;
                    methodClosure->set("$", thisVar);

                    FunctionPtr fnptr = std::make_shared<FunctionValue>(declptr->name, declptr->parameters, declptr, methodClosure, declptr->token);
                    fnptr->name = keyStr;
                    val = fnptr;
                } else {
                    val = evaluate_expression(p->value.get(), env);
                }
            } else if (p->kind == PropertyKind::KeyValue) {
                val = p->value ? evaluate_expression(p->value.get(), env) : std::monostate{};
            } else {
                // Shorthand
                if (!p->key_name.empty()) {
                    if (env->has(p->key_name))
                        val = env->get(p->key_name).value;
                    else
                        val = std::monostate{};
                } else if (p->key) {
                    val = evaluate_expression(p->key.get(), env);
                } else
                    val = std::monostate{};
            }

            PropertyDescriptor desc;
            desc.value = val;
            desc.is_private = p->is_private;
            desc.is_readonly = p->is_readonly;
            desc.is_locked = p->is_locked;
            desc.token = p->token;

            obj->properties[keyStr] = std::move(desc);
        }

        return Value{
            obj};
    }

    if (auto b = dynamic_cast<BooleanLiteralNode*>(expr)) return Value{
        b->value};

    if (auto id = dynamic_cast<IdentifierNode*>(expr)) {
        if (!env) throw std::runtime_error("No environment when resolving identifier '" + id->name + "'");
        if (!env->has(id->name)) {
            throw std::runtime_error("Undefined identifier '" + id->name + "' at " + id->token.loc.to_string());
        }
        return env->get(id->name).value;
    }

    // handle the $ / this node
    if (auto self = dynamic_cast<ThisExpressionNode*>(expr)) {
        // defensive checks similar to IdentifierNode handling
        if (!env) throw std::runtime_error("No environment when resolving '$' at " + self->token.loc.to_string());

        if (env->has("$")) {
            return env->get("$").value;
        }

        // not found -> undefined this
        throw std::runtime_error("Undefined 'this' ('$') at " + self->token.loc.to_string());
    }

    // Member access: object.property (e.g., arr.idadi, arr.ongeza, str.herufi)
    if (auto mem = dynamic_cast<MemberExpressionNode*>(expr)) {
        Value objVal = evaluate_expression(mem->object.get(), env);

        if (mem->is_optional && is_nullish(objVal)) {
            return std::monostate{};
        }

        if (std::holds_alternative<ClassPtr>(objVal)) {
            ClassPtr cls = std::get<ClassPtr>(objVal);
            if (!cls || !cls->static_table) return std::monostate{};
            auto it = cls->static_table->properties.find(mem->property);
            if (it == cls->static_table->properties.end()) return std::monostate{};
            const PropertyDescriptor& desc = it->second;
            if (desc.is_private && !is_private_access_allowed(nullptr, env)) {
                // private static access rule: you may want to permit only from within class methods.
                throw std::runtime_error("Cannot access private static property '" + mem->property + "' at " + desc.token.loc.to_string());
            }
            // getters on static_table use is_readonly same way as objects
            if (desc.is_readonly && std::holds_alternative<FunctionPtr>(desc.value)) {
                FunctionPtr getter = std::get<FunctionPtr>(desc.value);
                return call_function(getter, {}, desc.token);
            }
            return desc.value;
        }

        // --- Universal properties ---
        const std::string& prop = mem->property;

        // aina -> type name
        if (prop == "aina") {
            std::string t = "unknown";  // default
            if (std::holds_alternative<std::monostate>(objVal)) t = "null";
            if (std::holds_alternative<double>(objVal))
                t = "namba";
            else if (std::holds_alternative<std::string>(objVal))
                t = "neno";
            else if (std::holds_alternative<bool>(objVal))
                t = "bool";
            else if (std::holds_alternative<ArrayPtr>(objVal))
                t = "orodha";
            else if (std::holds_alternative<FunctionPtr>(objVal))
                t = "kazi";
            else if (std::holds_alternative<ObjectPtr>(objVal))
                t = "object";
            else if (std::holds_alternative<ClassPtr>(objVal))
                t = "muundo";
            return Value{
                t};
        }

        // type-checking booleans
        if (prop == "ninamba") return Value{
            std::holds_alternative<double>(objVal)};
        if (prop == "nineno") return Value{
            std::holds_alternative<std::string>(objVal)};
        if (prop == "nibool") return Value{
            std::holds_alternative<bool>(objVal)};
        if (prop == "niorodha") return Value{
            std::holds_alternative<ArrayPtr>(objVal)};
        if (prop == "nikazi") return Value{
            std::holds_alternative<FunctionPtr>(objVal)};

        // String property 'herufi' (length)
        if (std::holds_alternative<std::string>(objVal) && mem->property == "herufi") {
            const std::string& s = std::get<std::string>(objVal);
            return Value{
                static_cast<double>(s.size())};
        }

        // String methods/properties
        if (std::holds_alternative<std::string>(objVal)) {
            const std::string s_val = std::get<std::string>(objVal);
            const std::string& prop = mem->property;

            // helper to create native function values (captures s_val and this)
            auto make_fn = [this,s_val,env,mem](std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) -> Value {
                auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    return impl(args, callEnv, token);
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:string.") + mem->property, native_impl, env, mem->token);
                return Value{
                    fn};
            };

            // herufiNdogo() -> toLowerCase
            if (prop == "herufiNdogo") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string out = s_val;
                    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    return Value{
                        out};
                });
            }

            // herufiKubwa() -> toUpperCase
            if (prop == "herufiKubwa") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string out = s_val;
                    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    return Value{
                        out};
                });
            }

            // sawazisha() -> trim()
            if (prop == "sawazisha") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    size_t a = 0, b = s_val.size();
                    while (a < b && std::isspace(static_cast<unsigned char>(s_val[a]))) ++a;
                    while (b > a && std::isspace(static_cast<unsigned char>(s_val[b - 1]))) --b;
                    return Value{
                        s_val.substr(a, b - a)};
                });
            }

            // anzaNa(prefix) -> startsWith
            if (prop == "huanzaNa") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.anzaNa needs 1 arg at " + token.loc.to_string());
                    std::string pref = to_string_value(args[0]);
                    if (pref.size() > s_val.size()) return Value{
                        false};
                    return Value{
                        s_val.rfind(pref, 0) == 0};
                });
            }

            // ishaNa(suffix) -> endsWith
            if (prop == "huishaNa") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.ishaNa needs 1 arg at " + token.loc.to_string());
                    std::string suf = to_string_value(args[0]);
                    if (suf.size() > s_val.size()) return Value{
                        false};
                    return Value{
                        s_val.compare(s_val.size() - suf.size(), suf.size(), suf) == 0};
                });
            }

            // kuna(sub) -> includes
            if (prop == "kuna") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.kuna needs 1 arg at " + token.loc.to_string());
                    std::string sub = to_string_value(args[0]);
                    return Value{
                        s_val.find(sub) != std::string::npos};
                });
            }

            // tafuta(sub, fromIndex?) -> indexOf
            if (prop == "tafuta") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.tafuta needs 1 argument at " + token.loc.to_string());
                    std::string sub = to_string_value(args[0]);
                    size_t from = 0;
                    if (args.size() >= 2) from = static_cast<size_t>(std::max(0LL, static_cast<long long>(to_number(args[1]))));
                    size_t pos = s_val.find(sub, from);
                    if (pos == std::string::npos) return Value{
                        static_cast<double>(-1)};
                    return Value{
                        static_cast<double>(pos)};
                });
            }

            // slesi(start?, end?) -> substring-like slice
            if (prop == "slesi") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    long long n = static_cast<long long>(s_val.size());
                    long long start = 0;
                    long long end = n;
                    if (args.size() >= 1) start = static_cast<long long>(to_number(args[0]));
                    if (args.size() >= 2) end = static_cast<long long>(to_number(args[1]));
                    if (start < 0) start = std::max(0LL, n + start);
                    if (end < 0) end = std::max(0LL, n + end);
                    start = std::min(std::max(0LL, start), n);
                    end = std::min(std::max(0LL, end), n);
                    return Value{
                        s_val.substr((size_t)start, (size_t)(end - start))};
                });
            }

            // badilisha(old, neu) -> replace first occurrence
            if (prop == "badilisha") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.size() < 2) throw std::runtime_error("str.badilisha needs 2 args at " + token.loc.to_string());
                    std::string oldv = to_string_value(args[0]);
                    std::string newv = to_string_value(args[1]);
                    std::string out = s_val;
                    size_t pos = out.find(oldv);
                    if (pos != std::string::npos) out.replace(pos, oldv.size(), newv);
                    return Value{
                        out};
                });
            }

            // badilishaZote(old, neu) -> replace all occurrences
            if (prop == "badilishaZote") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.size() < 2) throw std::runtime_error("str.badilishaZote needs 2 args at " + token.loc.to_string());
                    std::string oldv = to_string_value(args[0]);
                    std::string newv = to_string_value(args[1]);
                    if (oldv.empty()) return Value{
                        s_val};  // avoid infinite loop
                    std::string out;
                    size_t pos = 0, prev = 0;
                    while ((pos = s_val.find(oldv, prev)) != std::string::npos) {
                        out.append(s_val, prev, pos - prev);
                        out.append(newv);
                        prev = pos + oldv.size();
                    }
                    out.append(s_val, prev, std::string::npos);
                    return Value{
                        out};
                });
            }

            // orodhesha(separator?) -> split into ArrayPtr
            if (prop == "orodhesha") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    std::string sep;
                    bool useSep = false;
                    if (!args.empty()) {
                        sep = to_string_value(args[0]);
                        useSep = true;
                    }
                    auto out = std::make_shared<ArrayValue>();
                    if (!useSep) {
                        for (size_t i = 0; i < s_val.size(); ++i) out->elements.push_back(Value{
                            std::string(1, s_val[i])});
                        return Value{
                            out};
                    }
                    if (sep.empty()) {
                        for (size_t i = 0; i < s_val.size(); ++i) out->elements.push_back(Value{
                            std::string(1, s_val[i])});
                        return Value{
                            out};
                    }
                    size_t pos = 0, prev = 0;
                    while ((pos = s_val.find(sep, prev)) != std::string::npos) {
                        out->elements.push_back(Value{
                            s_val.substr(prev, pos - prev)});
                        prev = pos + sep.size();
                    }
                    out->elements.push_back(Value{
                        s_val.substr(prev)});
                    return Value{
                        out};
                });
            }

            // unganisha(other) -> concat and return new string
            if (prop == "unganisha") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.unganisha needs atleast one arg at " + token.loc.to_string());
                    return Value{
                        s_val + to_string_value(args[0])};
                });
            }

            // rudia(n) -> repeat string n times
            if (prop == "rudia") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.rudia needs 1 args at " + token.loc.to_string());
                    long long n = static_cast<long long>(to_number(args[0]));
                    if (n <= 0) return Value{
                        std::string()};
                    std::string out;
                    out.reserve(s_val.size() * (size_t)n);
                    for (long long i = 0; i < n; ++i) out += s_val;
                    return Value{
                        out};
                });
            }

            // herufiYa(index) -> charAt (single-char string or empty)
            if (prop == "herufiYa") {
                return make_fn([this, s_val](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("str.herufiKwa needs 1 arg at " + token.loc.to_string());
                    long long idx = static_cast<long long>(to_number(args[0]));
                    if (idx < 0 || (size_t)idx >= s_val.size()) return Value{
                        std::string()};
                    return Value{
                        std::string(1, s_val[(size_t)idx])};
                });
            }
            
            // urefu() -> returns string length (same as .herufi property)
            if (prop == "urefu") {
                return make_fn([s_val](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    return Value{static_cast<double>(s_val.size())};
                });
            }
            
            // No matching string property -> fall through to unknown property error below
        }

        // --- Number methods & properties (place this after string methods, before array methods) ---
        if (std::holds_alternative<double>(objVal)) {
            double num = std::get<double>(objVal);
            const std::string& prop = mem->property;

            // ---------- Properties ----------
            if (prop == "siSahihi") {
                return Value{
                    std::isnan(num)};
            }
            if (prop == "inf") {
                return Value{
                    !std::isfinite(num)};
            }
            if (prop == "nzima") {
                return Value{
                    std::isfinite(num) && std::floor(num) == num};
            }
            if (prop == "desimali") {
                return Value{
                    std::isfinite(num) && std::floor(num) != num};
            }
            if (prop == "chanya") {
                return Value{
                    num > 0};
            }
            if (prop == "hasi") {
                return Value{
                    num < 0};
            }
            // boolean "is" properties: odd, even, prime
            if (prop == "witiri" || prop == "shufwa" || prop == "tasa") {
                // quick guards: must be finite and integer
                if (!std::isfinite(num) || std::floor(num) != num) {
                    return Value{
                        false};
                }

                // avoid UB on cast if number too large for signed long long
                if (num > static_cast<double>(LLONG_MAX) || num < static_cast<double>(LLONG_MIN)) {
                    return Value{
                        false};
                }

                long long n = static_cast<long long>(std::llround(num));  // safe now

                if (prop == "witiri") {
                    // odd
                    return Value{
                        (n % 2) != 0};
                }

                if (prop == "shufwa") {
                    // even
                    return Value{
                        (n % 2) == 0};
                }

                // niTasa -> primality (simple trial division)
                if (prop == "tasa") {
                    if (n < 2) return Value{
                        false};
                    if (n % 2 == 0) return Value{
                        n == 2};  // even >2 => composite

                    long long limit = static_cast<long long>(std::sqrt((long double)n));
                    for (long long i = 3; i <= limit; i += 2) {
                        if (n % i == 0) return Value{
                            false};
                    }
                    return Value{
                        true};
                }
            }

            // ---------- Methods (return FunctionValue) ----------

            // abs -> n.abs()
            if (prop == "abs") {
                auto native_impl = [num](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    return Value{
                        std::fabs(num)};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.abs"), native_impl, env, mem->token);
                return Value{
                    fn};
            }

            // round / kadiria
            if (prop == "kadiria") {
                auto native_impl = [num](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    return Value{
                        static_cast<double>(std::round(num))};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.kadiria"), native_impl, env, mem->token);
                return Value{
                    fn};
            }
            if (prop == "kadiriajuu") {
                auto native_impl = [num](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    return Value{
                        static_cast<double>(std::ceil(num))};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.kadiriajuu"), native_impl, env, mem->token);
                return Value{
                    fn};
            }
            if (prop == "kadiriachini") {
                auto native_impl = [num](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& /*token*/) -> Value {
                    return Value{
                        static_cast<double>(std::floor(num))};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.kadiriachini"), native_impl, env, mem->token);
                return Value{
                    fn};
            }

            // kipeo / kipeuo: power & nth-root
            // n.kipeo(b?) => n**b  (default: square if no arg)
            // n.kipeuo(b?) => nth-root (default: square root if no arg)
            if (prop == "kipeo") {
                auto native_impl = [this,
                                       num](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        return Value{
                            num * num};  // square by default
                    }
                    double b = to_number(args[0]);
                    return Value{
                        std::pow(num, b)};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.kipeo"), native_impl, env, mem->token);
                return Value{
                    fn};
            }
            if (prop == "kipeuo") {
                auto native_impl = [this,
                                       num](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    // default: sqrt
                    if (args.empty()) {
                        if (num < 0) throw std::runtime_error("Huwezi kupata kipeuo cha pili cha thamani hasi");
                        return Value{
                            std::sqrt(num)};
                    }

                    double b = to_number(args[0]);
                    if (b == 0) throw std::runtime_error("Huwezi kugawa kwa sifuri kwenye kipeuo");
                    if (!std::isfinite(b)) throw std::runtime_error("Kiwango cha kipeuo si sahihi(inf)");

                    // guard: negative base with negative root degree -> division by zero if num==0
                    if (num == 0.0 && b < 0.0) throw std::runtime_error("Haiwezekani kupata kipeuo hasi cha sifuri");

                    // helper: decide if b is (almost) an integer
                    auto is_integer = [](double x) {
                        return std::isfinite(x) && std::fabs(x - std::round(x)) < 1e-12;
                    };

                    if (is_integer(b)) {
                        // safe integer conversion with magnitude check
                        double rb = std::round(b);
                        if (rb > static_cast<double>(LLONG_MAX) || rb < static_cast<double>(LLONG_MIN)) {
                            throw std::runtime_error("Kiwango cha kipeuo kimezidi mipaka");
                        }
                        long long ib = static_cast<long long>(rb);

                        // handle special small integer cases for numeric stability
                        if (ib == 1) return Value{
                            num};  // 1st root => itself
                        if (ib == -1) {
                            // -1 => reciprocal
                            if (num == 0.0) throw std::runtime_error("Haiwezekani kugawa kwa sifuri");
                            return Value{
                                1.0 / num};
                        }
                        if (ib == 2) {
                            // square root
                            if (num < 0) throw std::runtime_error("Huwezi kupata kipeuo cha thamani hasi");
                            return Value{
                                std::sqrt(num)};
                        }
                        if (ib == -2) {
                            if (num == 0.0) throw std::runtime_error("Haiwezekani kugawa kwa sifuri");
                            if (num < 0) throw std::runtime_error("Huwezi kupata kipeuo cha thamani hasi");
                            return Value{
                                1.0 / std::sqrt(num)};
                        }
                        if (ib == 3 || ib == -3) {
                            // cube root
                            double root = std::cbrt(std::fabs(num));
                            if (ib < 0) {
                                if (num == 0.0) throw std::runtime_error("Haiwezekani kugawa kwa sifuri");
                                root = 1.0 / root;
                            }
                            // cube root of negative is negative
                            if (num < 0) root = -root;
                            return Value{
                                root};
                        }

                        // For other integer ib:
                        bool ib_is_odd = (std::llabs(ib) % 2 == 1);
                        long long abs_ib = llabs(ib);

                        if (!ib_is_odd && num < 0) {
                            // even root of negative -> non-real
                            throw std::runtime_error("You can not calculate nth root(kipeuo cha nth cha namba) of a -ve number, when 'n' is even ");
                        }

                        // compute nth root: sign handling for odd roots
                        double result = std::pow(std::fabs(num), 1.0 / static_cast<double>(abs_ib));
                        if (num < 0) result = -result;  // odd ib => negative result allowed

                        if (ib < 0) {
                            // negative ib => reciprocal
                            if (result == 0.0) throw std::runtime_error("Haiwezekani kugawa kwa sifuri");
                            result = 1.0 / result;
                        }

                        return Value{
                            result};
                    }

                    // b is non-integer (fractional root). For real results, base must be >= 0.
                    if (num < 0) {
                        throw std::runtime_error("You can not calculate nth root(kipeuo cha nth cha namba) of -ve number(namba hasi) with a non integer(n) (that will produce a complex number — a + bi)");
                    }

                    // general fractional root: num^(1/b)
                    double res = std::pow(num, 1.0 / b);
                    return Value{
                        res};
                };

                auto fn = std::make_shared<FunctionValue>(std::string("native:number.kipeuo"), native_impl, env, mem->token);
                return Value{
                    fn};
            }

            // n.kubwa / n.ndogo -> compare n to args; single-arg returns number, multi-arg returns new array
            if (prop == "kubwa" || prop == "ndogo") {
                bool wantMax = (prop == "kubwa");

                auto native_impl = [this,
                                       num,
                                       wantMax](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) {
                        throw std::runtime_error(std::string("n.") + (wantMax ? "kubwa" : "ndogo") +
                            " needs at least 1 argument at " + token.loc.to_string());
                    }

                    if (args.size() == 1) {
                        double a = to_number(args[0]);
                        return Value{
                            wantMax ? std::max(num, a) : std::min(num, a)};
                    }

                    // multiple args -> return array of pairwise comparisons: for each arg return max/min(num, arg)
                    auto out = std::make_shared<ArrayValue>();
                    out->elements.reserve(args.size());
                    for (const auto& a : args) {
                        double v = to_number(a);
                        double res = wantMax ? std::max(num, v) : std::min(num, v);
                        out->elements.push_back(Value{
                            res});
                    }
                    return Value{
                        out};
                };

                auto fn = std::make_shared<FunctionValue>(std::string("native:number.") + prop, native_impl, env, mem->token);
                return Value{
                    fn};
            }

            // toFixed(digits?)
            if (prop == "kadiriaKwa") {
                auto native_impl = [this,
                                       num](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    int digits = 0;
                    if (!args.empty()) digits = static_cast<int>(to_number(args[0]));
                    std::ostringstream oss;
                    oss.setf(std::ios::fixed);
                    oss.precision(std::max(0, digits));
                    oss << num;
                    return Value{
                        oss.str()};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.toFixed"), native_impl, env, mem->token);
                return Value{
                    fn};
            }

            // kwaKiwango(factor) -> num * factor
            if (prop == "kwaKiwango") {
                auto native_impl = [this,
                                       num](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                    if (args.empty()) throw std::runtime_error("n.kwaKiwango needs atleast one argument at " + token.loc.to_string());
                    double factor = to_number(args[0]);
                    return Value{
                        num * factor};
                };
                auto fn = std::make_shared<FunctionValue>(std::string("native:number.kwaKiwango"), native_impl, env, mem->token);
                return Value{
                    fn};
            }

            // If none matched, fallthrough to unknown prop logic below
        }

        // Array properties & methods
        if (std::holds_alternative<ArrayPtr>(objVal)) {
            ArrayPtr arr = std::get<ArrayPtr>(objVal);

            // length property
            if (mem->property == "idadi") {
                return Value{
                    static_cast<double>(arr ? arr->elements.size() : 0)};
            }

            // Accept multiple synonyms for common operations (keeps compatibility)
            const std::string& prop = mem->property;

            // Recognized array method names
            if (prop == "urefu" || prop == "indexOf" || prop == "indexYa" || prop == "tafutaIndex" || prop == "ongeza" || prop == "toa" || prop == "ondoa" || prop == "ondoaMwanzo" ||
                prop == "ongezaMwanzo" || prop == "ingiza" || prop == "slesi" ||
                prop == "panua" || prop == "badili" || prop == "tafuta" || prop == "kuna" ||
                prop == "panga" || prop == "geuza" || prop == "futa" || prop == "chambua" || prop == "punguza" || prop == "unganisha" || prop == "ondoaZote" || prop == "pachika") {
                auto native_impl = [this,arr,prop](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
                    if (!arr) return std::monostate{};
                    
                    // urefu() -> returns array length (same as .idadi property)
                    if (prop == "urefu") {
                        return Value{static_cast<double>(arr->elements.size())};
                    }
                    
                    // push: ongeza(value...)
                    if (prop == "ongeza") {
                        if (args.empty()) throw std::runtime_error("arr.ongeza needs atleast 1 arg at " + token.loc.to_string());
                        arr->elements.insert(arr->elements.end(), args.begin(), args.end());
                        return Value{
                            static_cast<double>(arr->elements.size())};
                    }

                    // pop (from end) : toa
                    if (prop == "toa") {
                        if (arr->elements.empty()) return std::monostate{};
                        Value v = arr->elements.back();
                        arr->elements.pop_back();
                        return v;
                    }

                    // remove by value: ondoa(value) -> removes first occurrence, returns boolean
                    if (prop == "ondoa" && !args.empty()) {
                        auto it = std::find_if(arr->elements.begin(), arr->elements.end(), [&](const Value& elem) {
                            return is_equal(elem, args[0]);
                        });
                        if (it != arr->elements.end()) {
                            arr->elements.erase(it);
                            return Value{
                                true};
                        }
                        return Value{
                            false};
                    }

                    // remove all by value: ondoaZote(value) -> removes all occurrences, returns count removed
                    if (prop == "ondoaZote" && !args.empty()) {
                        size_t before = arr->elements.size();
                        arr->elements.erase(
                            std::remove_if(arr->elements.begin(), arr->elements.end(), [&](const Value& elem) {
                                return is_equal(elem, args[0]);
                            }),
                            arr->elements.end());
                        size_t removed = before - arr->elements.size();
                        return Value{
                            static_cast<double>(removed)};
                    }

                    // shift: ondoaMwanzo
                    if (prop == "ondoaMwanzo") {
                        if (arr->elements.empty()) return std::monostate{};
                        Value v = arr->elements.front();
                        arr->elements.erase(arr->elements.begin());
                        return v;
                    }

                    // unshift: ongezaMwanzo(...)
                    if (prop == "ongezaMwanzo") {
                        if (args.empty()) throw std::runtime_error("arr.ongezaMwanzo needs atleast 1 arg at " + token.loc.to_string());
                        arr->elements.insert(arr->elements.begin(), args.begin(), args.end());
                        return Value{
                            static_cast<double>(arr->elements.size())};
                    }

                    // insert(value, index)
                    if (prop == "ingiza") {
                        if (args.size() < 2) throw std::runtime_error("arr.ingiza needs atleast 2 arguments (value, index) at " + token.loc.to_string());
                        const Value& val = args[0];
                        long long idx = static_cast<long long>(to_number(args[1]));
                        if (idx < 0) idx = 0;
                        size_t uidx = static_cast<size_t>(std::min<long long>(idx, static_cast<long long>(arr->elements.size())));
                        arr->elements.insert(arr->elements.begin() + uidx, val);
                        return Value{
                            static_cast<double>(arr->elements.size())};
                    }

                    // clear: futa()
                    if (prop == "futa") {
                        arr->elements.clear();
                        return std::monostate{};
                    }

                    // extend: panua(otherArray)
                    if (prop == "panua") {
                        if (args.empty() || !std::holds_alternative<ArrayPtr>(args[0])) {
                            throw std::runtime_error("arr.panua needs an array as an argument at " + token.loc.to_string());
                        }

                        ArrayPtr other = std::get<ArrayPtr>(args[0]);

                        auto out = std::make_shared<ArrayValue>();
                        if (arr) {
                            // copy all elements of the current array
                            out->elements.insert(out->elements.end(), arr->elements.begin(), arr->elements.end());
                        }
                        if (other) {
                            // append all elements of the other array
                            out->elements.insert(out->elements.end(), other->elements.begin(), other->elements.end());
                        }

                        return Value{
                            out};
                    }

                    // reverse: geuza()
                    if (prop == "geuza") {
                        std::reverse(arr->elements.begin(), arr->elements.end());
                        return Value{
                            arr};
                    }

                    // sort: panga([comparator])
                    if (prop == "panga") {
                        if (args.empty()) {
                            // default: lexicographic by to_string_value
                            std::sort(arr->elements.begin(), arr->elements.end(), [this](const Value& A, const Value& B) {
                                return to_string_value(A) < to_string_value(B);
                            });
                        } else {
                            if (!std::holds_alternative<FunctionPtr>(args[0])) throw std::runtime_error("arr.panga expects comparator function at " + token.loc.to_string());
                            FunctionPtr cmp = std::get<FunctionPtr>(args[0]);
                            std::sort(arr->elements.begin(), arr->elements.end(), [&](const Value& A, const Value& B) {
                                Value res = call_function(cmp, {A, B}, token);
                                // comparator should return number <0,0,>0 similar to JS - we treat negative as A<B
                                return to_number(res) < 0;
                            });
                        }
                        return Value{
                            arr};
                    }

                    // indexOf: indexOf(value) -> returns index or -1
                    // indexOf with optional start and backward-search sentinel: indexOf(value, start?)
                    if (prop == "indexOf" || prop == "indexYa") {
                        if (args.empty()) throw std::runtime_error("arr.indexOf needs one argument at " + token.loc.to_string());

                        const Value& target = args[0];
                        long long n = static_cast<long long>(arr->elements.size());

                        // empty array -> not found
                        if (n == 0) return Value{
                            static_cast<double>(-1)};

                        // parse start if provided
                        long long startNum = 0;
                        bool backwardMode = false;  // when start == -1 => search backwards from last to first

                        if (args.size() >= 2) {
                            startNum = static_cast<long long>(to_number(args[1]));  // cast like other parts of your code
                            if (startNum == -1) {
                                backwardMode = true;
                            }
                        }

                        // Backward search mode: start from last element and move to index 0
                        if (backwardMode) {
                            for (long long i = n - 1; i >= 0; --i) {
                                if (is_equal(arr->elements[(size_t)i], target)) return Value{
                                    static_cast<double>(i)};
                                if (i == 0) break;  // avoid negative wrap when i is unsigned in some contexts
                            }
                            return Value{
                                static_cast<double>(-1)};
                        }

                        // Forward search mode:
                        // Normalize negative start as offset from end (like JS semantics for negative fromIndex)
                        long long startIndex = startNum;
                        if (args.size() >= 2 && startIndex < 0) {
                            startIndex = std::max(0LL, n + startIndex);
                        }

                        // If startIndex is past the end, nothing to search
                        if (startIndex >= n) return Value{
                            static_cast<double>(-1)};

                        for (long long i = startIndex; i < n; ++i) {
                            if (is_equal(arr->elements[(size_t)i], target)) return Value{
                                static_cast<double>(i)};
                        }

                        return Value{
                            static_cast<double>(-1)};
                    }

                    // find: tafuta(fn) -> returns first element matching fn(elem, index, arr)
                    if (prop == "tafuta") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0]))
                            throw std::runtime_error("arr.tafuta needs a function as an argument at " + token.loc.to_string());
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);

                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);

                            if (to_bool(res))
                                return arr->elements[i];  // return first matching element
                        }
                        return std::monostate{};  // like JS undefined when no match
                    }

                    // findIndex: tafutaKwanza(fn) -> returns index of first match or -1
                    if (prop == "tafutaIndex") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0]))
                            throw std::runtime_error("arr.tafutaIndex needs a function as argument at " + token.loc.to_string());
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);

                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);

                            if (to_bool(res))
                                return Value{
                                    static_cast<double>(i)};
                        }
                        return Value{
                            static_cast<double>(-1)};
                    }

                    // includes: kuna(value)
                    if (prop == "kuna") {
                        if (args.empty()) throw std::runtime_error("arr.kuna needs 1 args at " + token.loc.to_string());
                        for (const auto& e : arr->elements)
                            if (is_equal(e, args[0])) return Value{
                                true};
                        return Value{
                            false};
                    }

                    // slice: pachika(start?, end?) or slesi
                    if (prop == "slesi") {
                        long long n = static_cast<long long>(arr->elements.size());
                        long long start = 0;
                        long long end = n;
                        if (args.size() >= 1) start = static_cast<long long>(to_number(args[0]));
                        if (args.size() >= 2) end = static_cast<long long>(to_number(args[1]));
                        // normalize negative
                        if (start < 0) start = std::max(0LL, n + start);
                        if (end < 0) end = std::max(0LL, n + end);
                        start = std::min(std::max(0LL, start), n);
                        end = std::min(std::max(0LL, end), n);
                        auto out = std::make_shared<ArrayValue>();
                        for (long long i = start; i < end; ++i) out->elements.push_back(arr->elements[(size_t)i]);
                        return Value{
                            out};
                    }

                    // splice: pachika(start, deleteCount, ...items)
                    if (prop == "pachika") {
                        if (args.size() < 2)
                            throw std::runtime_error("arr.pachika needs at least 2 args at " + token.loc.to_string());

                        long long start = static_cast<long long>(to_number(args[0]));
                        long long delCount = static_cast<long long>(to_number(args[1]));

                        if (start < 0) start = std::max(0LL, (long long)arr->elements.size() + start);
                        start = std::min(start, (long long)arr->elements.size());
                        delCount = std::max(0LL, std::min(delCount, (long long)arr->elements.size() - start));

                        auto out = std::make_shared<ArrayValue>();
                        out->elements.insert(out->elements.end(),
                            arr->elements.begin() + start,
                            arr->elements.begin() + start + delCount);

                        // erase deleted
                        arr->elements.erase(arr->elements.begin() + start,
                            arr->elements.begin() + start + delCount);

                        // insert new items if any
                        if (args.size() > 2) {
                            arr->elements.insert(arr->elements.begin() + start, args.begin() + 2, args.end());
                        }

                        return Value{out};  // return deleted elements
                    }

                    // map: badili(fn)
                    if (prop == "badili") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0])) throw std::runtime_error("arr.badili needs a function as an argument at " + token.loc.to_string());
                        FunctionPtr mapper = std::get<FunctionPtr>(args[0]);
                        auto out = std::make_shared<ArrayValue>();
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(mapper, {arr->elements[i], Value{static_cast<double>(i)}}, token);
                            out->elements.push_back(res);
                        }
                        return Value{
                            out};
                    }

                    // filter: chambua(fn) -> returns new array with elements where fn(elem, index, arr) is truthy
                    if (prop == "chambua") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0]))
                            throw std::runtime_error("arr.chambua needs a filter function as an argument at " + token.loc.to_string());
                        FunctionPtr predicate = std::get<FunctionPtr>(args[0]);

                        auto out = std::make_shared<ArrayValue>();
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            Value res = call_function(predicate, {arr->elements[i], Value{static_cast<double>(i)}, Value{arr}}, token);
                            if (to_bool(res)) out->elements.push_back(arr->elements[i]);
                        }
                        return Value{
                            out};
                    }

                    // reduce: punguza(fn, initial?) -> reduce array to single value
                    if (prop == "punguza") {
                        if (args.empty() || !std::holds_alternative<FunctionPtr>(args[0]))
                            throw std::runtime_error("arr.punguza needs a function as an argument at " + token.loc.to_string());
                        FunctionPtr reducer = std::get<FunctionPtr>(args[0]);

                        size_t startIndex = 0;
                        Value acc;

                        if (args.size() >= 2) {
                            // initial provided
                            acc = args[1];
                            startIndex = 0;
                        } else {
                            // no initial: use first element as acc (JS-like). If empty -> error.
                            if (arr->elements.empty()) throw std::runtime_error("arr.punguza on empty array without initial at " + token.loc.to_string());
                            acc = arr->elements[0];
                            startIndex = 1;
                        }

                        for (size_t i = startIndex; i < arr->elements.size(); ++i) {
                            acc = call_function(reducer, {acc, arr->elements[i], Value{static_cast<double>(i)}}, token);
                        }
                        return acc;
                    }

                    // join: unganisha(separator?) -> returns string (elements coerced to string)
                    if (prop == "unganisha") {
                        std::string sep = ",";
                        if (!args.empty()) sep = to_string_value(args[0]);  // coerce separator to string

                        std::ostringstream oss;
                        for (size_t i = 0; i < arr->elements.size(); ++i) {
                            if (i) oss << sep;
                            oss << to_string_value(arr->elements[i]);
                        }
                        return Value{
                            oss.str()};
                    }

                    // default fallback
                    return std::monostate{};
                };

                auto fn = std::make_shared<FunctionValue>(std::string("native:array.") + prop, native_impl, env, mem->token);
                return Value{
                    fn};
            }
        }

        if (std::holds_alternative<ObjectPtr>(objVal)) {
            ObjectPtr op = std::get<ObjectPtr>(objVal);
            return get_object_property(op, mem->property, env);
        }
        // For other non-array/non-string objects, return undefined for unknown props
        throw std::runtime_error("Unknown property '" + mem->property + "' on value at " + mem->token.loc.to_string());
    }

    // Indexing: obj[index]
    if (auto idx = dynamic_cast<IndexExpressionNode*>(expr)) {
        // Evaluate receiver first.
        Value objVal = evaluate_expression(idx->object.get(), env);

        // Optional chaining: if receiver is nullish and this is an optional index,
        // short-circuit and return undefined without evaluating the index expression.
        if (idx->is_optional && is_nullish(objVal)) {
            return std::monostate{};
        }

        // Ensure index expression exists
        if (!idx->index) {
            throw std::runtime_error("Index expression missing at " + idx->token.loc.to_string());
        }

        // Now safe to evaluate the index expression.
        Value indexVal = evaluate_expression(idx->index.get(), env);

        // Array indexing uses numeric interpretation of indexVal
        if (std::holds_alternative<ArrayPtr>(objVal)) {
            ArrayPtr arr = std::get<ArrayPtr>(objVal);
            if (!arr) return std::monostate{};

            long long rawIndex = static_cast<long long>(to_number(indexVal));
            if (rawIndex < 0 || (size_t)rawIndex >= arr->elements.size()) {
                return std::monostate{};
            }
            return arr->elements[(size_t)rawIndex];
        }

        // String indexing: return single-char string (optional)
        if (std::holds_alternative<std::string>(objVal)) {
            std::string s = std::get<std::string>(objVal);
            long long rawIndex = static_cast<long long>(to_number(indexVal));
            if (rawIndex < 0 || (size_t)rawIndex >= s.size()) {
                return std::monostate{};
            }
            return Value{std::string(1, s[(size_t)rawIndex])};
        }

        // Objects: use stringified index as key, go through unified getter (privacy/getter enforced)
        if (std::holds_alternative<ObjectPtr>(objVal)) {
            ObjectPtr op = std::get<ObjectPtr>(objVal);
            if (!op) return std::monostate{};
            std::string key = to_string_value(indexVal);
            return get_object_property(op, key, env);
        }

        throw std::runtime_error("Attempted to index non-array value at " + idx->token.loc.to_string());
    }

    if (auto u = dynamic_cast<UnaryExpressionNode*>(expr)) {
        Value operand = evaluate_expression(u->operand.get(), env);
        if (u->op == "!" || u->op == "si") return Value{
            !to_bool(operand)};
        if (u->op == "-") return Value{
            -to_number(operand)};

        // new: 'aina' unary operator -> returns runtime type name string (same semantics as obj.aina)
        if (u->op == "aina") {
            std::string t = "unknown";
            if (std::holds_alternative<std::monostate>(operand)) t = "null";
            else if (std::holds_alternative<double>(operand)) t = "namba";
            else if (std::holds_alternative<std::string>(operand)) t = "neno";
            else if (std::holds_alternative<bool>(operand)) t = "bool";
            else if (std::holds_alternative<ArrayPtr>(operand)) t = "orodha";
            else if (std::holds_alternative<FunctionPtr>(operand)) t = "kazi";
            else if (std::holds_alternative<ObjectPtr>(operand)) t = "object";
            else if (std::holds_alternative<ClassPtr>(operand)) t = "muundo";
            return Value{ t };
        }

        throw std::runtime_error("Unknown unary operator '" + u->op + "' at " + u->token.loc.to_string());
    }

    if (auto b = dynamic_cast<BinaryExpressionNode*>(expr)) {
        // --- handle ++ / -- and += / -= and *= as side-effecting ops ---
        if (b->token.type == TokenType::INCREMENT ||
            b->token.type == TokenType::DECREMENT ||
            b->token.type == TokenType::PLUS_ASSIGN ||
            b->token.type == TokenType::MINUS_ASSIGN ||
            b->token.type == TokenType::TIMES_ASSIGN) {
            // Case A: left is an identifier (x++, x += ...)
            if (auto leftIdent = dynamic_cast<IdentifierNode*>(b->left.get())) {
                // Search up the environment chain to update the defining environment
                EnvPtr walk = env;
                while (walk) {
                    auto it = walk->values.find(leftIdent->name);
                    if (it != walk->values.end()) {
                        if (it->second.is_constant) {
                            throw std::runtime_error("Cannot assign to constant '" + leftIdent->name + "' at " + b->token.loc.to_string());
                        }

                        // IMPORTANT: avoid converting stored value to number until we know
                        // we're on the numeric path. For += we must first check string concat.
                        if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                            double oldv = to_number(it->second.value);
                            double newv = oldv;
                            if (b->token.type == TokenType::INCREMENT)
                                newv = oldv + 1.0;
                            else
                                newv = oldv - 1.0;
                            it->second.value = Value{newv};
                            return Value{newv};
                        }

                        if (b->token.type == TokenType::PLUS_ASSIGN) {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            // string concat if either side is string
                            if (std::holds_alternative<std::string>(it->second.value) ||
                                std::holds_alternative<std::string>(rightVal)) {
                                std::string out = to_string_value(it->second.value) + to_string_value(rightVal);
                                it->second.value = Value{out};
                                return Value{out};
                            }
                            // numeric path
                            double oldv = to_number(it->second.value);
                            double rv = to_number(rightVal);
                            double newv = oldv + rv;
                            it->second.value = Value{newv};
                            return Value{newv};
                        }

                        // other compound ops: numeric-only
                        {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            double oldv = to_number(it->second.value);
                            double rv = to_number(rightVal);
                            double newv = oldv;
                            if (b->token.type == TokenType::MINUS_ASSIGN)
                                newv = oldv - rv;
                            else if (b->token.type == TokenType::TIMES_ASSIGN)
                                newv = oldv * rv;
                            else
                                newv = oldv + rv;  // fallback numeric
                            it->second.value = Value{newv};
                            return Value{newv};
                        }
                    }
                    walk = walk->parent;
                }

                // Not found in any parent -> create in current env (same behavior as assignment).
                // For += with string RHS create a string, otherwise create numeric start value.
                if (b->token.type == TokenType::PLUS_ASSIGN) {
                    Value rightVal = evaluate_expression(b->right.get(), env);
                    Environment::Variable var;
                    if (std::holds_alternative<std::string>(rightVal)) {
                        var.value = to_string_value(rightVal);
                    } else {
                        var.value = to_number(rightVal);
                    }
                    var.is_constant = false;
                    env->set(leftIdent->name, var);
                    return var.value;
                }

                // compute start value (treat missing as 0 for numeric ops)
                double start = 0.0;
                if (b->token.type == TokenType::INCREMENT)
                    start = 1.0;
                else if (b->token.type == TokenType::DECREMENT)
                    start = -1.0;
                else if (b->token.type == TokenType::TIMES_ASSIGN) {
                    // undefined treated as 0, 0 * rhs => 0
                    start = 0.0;
                } else {
                    Value rightVal = evaluate_expression(b->right.get(), env);
                    double rv = to_number(rightVal);
                    start = (b->token.type == TokenType::MINUS_ASSIGN) ? -rv : rv;
                }
                Environment::Variable var;
                var.value = start;
                var.is_constant = false;
                env->set(leftIdent->name, var);
                return Value{
                    start};
            }

            // Case B: left is an index expression (arr[idx]++ or arr[idx] += ...)
            if (auto idx = dynamic_cast<IndexExpressionNode*>(b->left.get())) {
                // Evaluate the object expression (this will return ArrayPtr or ObjectPtr)
                Value objVal = evaluate_expression(idx->object.get(), env);
                Value indexVal = evaluate_expression(idx->index.get(), env);

                // --- ARRAY PATH ---
                if (std::holds_alternative<ArrayPtr>(objVal)) {
                    ArrayPtr arr = std::get<ArrayPtr>(objVal);
                    if (!arr) {
                        throw std::runtime_error("Cannot assign into null array at " + b->token.loc.to_string());
                    }

                    long long rawIndex = static_cast<long long>(to_number(indexVal));
                    if (rawIndex < 0) throw std::runtime_error("Negative array index not supported at " + idx->token.loc.to_string());
                    size_t uidx = static_cast<size_t>(rawIndex);
                    if (uidx >= arr->elements.size()) arr->elements.resize(uidx + 1);

                    // Avoid converting element to number until numeric path chosen.
                    if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                        double oldv = to_number(arr->elements[uidx]);
                        double newv = oldv;
                        if (b->token.type == TokenType::INCREMENT)
                            newv = oldv + 1.0;
                        else
                            newv = oldv - 1.0;
                        arr->elements[uidx] = Value{newv};
                        return Value{newv};
                    }

                    if (b->token.type == TokenType::PLUS_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        if (std::holds_alternative<std::string>(arr->elements[uidx]) ||
                            std::holds_alternative<std::string>(rightVal)) {
                            std::string out = to_string_value(arr->elements[uidx]) + to_string_value(rightVal);
                            arr->elements[uidx] = Value{out};
                            return Value{out};
                        }
                        double oldv = to_number(arr->elements[uidx]);
                        double rv = to_number(rightVal);
                        double newv = oldv + rv;
                        arr->elements[uidx] = Value{newv};
                        return Value{newv};
                    }

                    // other compound ops: numeric-only
                    {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double oldv = to_number(arr->elements[uidx]);
                        double rv = to_number(rightVal);
                        double newv = oldv;
                        if (b->token.type == TokenType::MINUS_ASSIGN)
                            newv = oldv - rv;
                        else if (b->token.type == TokenType::TIMES_ASSIGN)
                            newv = oldv * rv;
                        else
                            newv = oldv + rv;
                        arr->elements[uidx] = Value{newv};
                        return Value{newv};
                    }
                }

                // --- OBJECT PATH (obj[key]++ / obj[key] += v / obj[key] *= v) ---
                if (std::holds_alternative<ObjectPtr>(objVal)) {
                    ObjectPtr op = std::get<ObjectPtr>(objVal);
                    if (!op) {
                        throw std::runtime_error("Cannot operate on null object at " + b->token.loc.to_string());
                    }

                    // convert indexVal -> property key string
                    std::string prop = to_string_value(indexVal);

                    auto it = op->properties.find(prop);

                    // If property exists, enforce privacy/read-only rules
                    if (it != op->properties.end()) {
                        if (it->second.is_private) {
                            bool allowed = false;
                            if (env) {
                                if (env->has("$")) {
                                    Value thisVal = env->get("$").value;
                                    if (std::holds_alternative<ObjectPtr>(thisVal) && std::get<ObjectPtr>(thisVal) == op) allowed = true;
                                }
                                if (!allowed && env->has("$this")) {
                                    Value thisVal = env->get("$this").value;
                                    if (std::holds_alternative<ObjectPtr>(thisVal) && std::get<ObjectPtr>(thisVal) == op) allowed = true;
                                }
                            }
                            if (!allowed) {
                                throw std::runtime_error("Cannot assign to private property '" + prop + "' from outside at " + idx->token.loc.to_string());
                            }
                        }

                        if (it->second.is_readonly) {
                            throw std::runtime_error("Cannot assign to read-only property '" + prop + "' at " + idx->token.loc.to_string());
                        }

                        // Avoid converting to number until numeric path is chosen.
                        if (b->token.type == TokenType::INCREMENT || b->token.type == TokenType::DECREMENT) {
                            double oldv = to_number(it->second.value);
                            double newv = oldv;
                            if (b->token.type == TokenType::INCREMENT)
                                newv = oldv + 1.0;
                            else
                                newv = oldv - 1.0;
                            it->second.value = Value{newv};
                            it->second.token = idx->token;
                            return Value{newv};
                        }

                        if (b->token.type == TokenType::PLUS_ASSIGN) {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            if (std::holds_alternative<std::string>(it->second.value) ||
                                std::holds_alternative<std::string>(rightVal)) {
                                std::string out = to_string_value(it->second.value) + to_string_value(rightVal);
                                it->second.value = Value{out};
                                it->second.token = idx->token;
                                return Value{out};
                            }
                            double oldv = to_number(it->second.value);
                            double rv = to_number(rightVal);
                            double newv = oldv + rv;
                            it->second.value = Value{newv};
                            it->second.token = idx->token;
                            return Value{newv};
                        }

                        // other compound ops: numeric-only
                        {
                            Value rightVal = evaluate_expression(b->right.get(), env);
                            double oldv = to_number(it->second.value);
                            double rv = to_number(rightVal);
                            double newv = oldv;
                            if (b->token.type == TokenType::MINUS_ASSIGN)
                                newv = oldv - rv;
                            else if (b->token.type == TokenType::TIMES_ASSIGN)
                                newv = oldv * rv;
                            else
                                newv = oldv + rv;
                            it->second.value = Value{newv};
                            it->second.token = idx->token;
                            return Value{newv};
                        }
                    }

                    // Property does not exist -> create public property:
                    // If += with string RHS create string property; otherwise create numeric property
                    if (b->token.type == TokenType::PLUS_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        PropertyDescriptor desc;
                        if (std::holds_alternative<std::string>(rightVal)) {
                            desc.value = Value{to_string_value(rightVal)};
                        } else {
                            desc.value = Value{to_number(rightVal)};
                        }
                        desc.is_private = false;
                        desc.is_readonly = false;
                        desc.token = idx->token;
                        op->properties[prop] = std::move(desc);
                        return op->properties[prop].value;
                    }

                    // Create numeric property starting from 0 and apply op
                    double oldv = 0.0;
                    double newv = oldv;
                    if (b->token.type == TokenType::INCREMENT)
                        newv = oldv + 1.0;
                    else if (b->token.type == TokenType::DECREMENT)
                        newv = oldv - 1.0;
                    else if (b->token.type == TokenType::TIMES_ASSIGN) {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double rv = to_number(rightVal);
                        newv = oldv * rv;
                    } else {
                        Value rightVal = evaluate_expression(b->right.get(), env);
                        double rv = to_number(rightVal);
                        newv = oldv + ((b->token.type == TokenType::MINUS_ASSIGN) ? -rv : rv);
                    }

                    PropertyDescriptor desc;
                    desc.value = Value{
                        newv};
                    desc.is_private = false;
                    desc.is_readonly = false;
                    desc.token = idx->token;
                    op->properties[prop] = std::move(desc);
                    return Value{
                        newv};
                }

                // Fallback: not array nor object
                throw std::runtime_error("Indexed target is not an array or object at " + b->token.loc.to_string());
            }
        }
        // --- Normal binary evaluation path ---
        Value left = evaluate_expression(b->left.get(), env);
        Value right = evaluate_expression(b->right.get(), env);
        const std::string& op = b->op;

        if (op == "+") {
            // --- string concatenation if either is string ---
            if (std::holds_alternative<std::string>(left) || std::holds_alternative<std::string>(right)) {
                return Value{
                    to_string_value(left) + to_string_value(right)};
            }

            // --- array concatenation if both are arrays ---
            if (std::holds_alternative<ArrayPtr>(left) && std::holds_alternative<ArrayPtr>(right)) {
                ArrayPtr a1 = std::get<ArrayPtr>(left);
                ArrayPtr a2 = std::get<ArrayPtr>(right);

                if (!a1 || !a2) {
                    throw std::runtime_error("Cannot concatenate null arrays at " + b->token.loc.to_string());
                }

                auto result = std::make_shared<ArrayValue>();
                result->elements.reserve(a1->elements.size() + a2->elements.size());
                result->elements.insert(result->elements.end(), a1->elements.begin(), a1->elements.end());
                result->elements.insert(result->elements.end(), a2->elements.begin(), a2->elements.end());

                return Value{
                    result};
            }

            // --- fallback numeric addition ---
            return Value{
                to_number(left) + to_number(right)};
        }

        if (op == "-") return Value{
            to_number(left) - to_number(right)};
        if (op == "*") return Value{
            to_number(left) * to_number(right)};
        if (op == "/") {
            double r = to_number(right);
            if (r == 0.0) throw std::runtime_error("Division by zero at " + b->token.loc.to_string());
            return Value{
                to_number(left) / r};
        }
        if (op == "%") {
            double r = to_number(right);
            if (r == 0.0) throw std::runtime_error("Modulo by zero at " + b->token.loc.to_string());
            return Value{
                std::fmod(to_number(left), r)};
        }
        if (op == "**") return Value{
            std::pow(to_number(left), to_number(right))};

        if (op == "===") {
            return Value(to_bool(Value(is_strict_equal(left, right))));
        }
        if (op == "!==") {
            return Value(!is_strict_equal(left, right));
        }
        if (op == "==" || op == "sawa") {
            return Value{
                is_equal(left, right)};
        }
        if (op == "!=" || op == "sisawa") {
            return Value{
                !is_equal(left, right)};
        }
        if (op == ">") return Value{
            to_number(left) > to_number(right)};
        if (op == "<") return Value{
            to_number(left) < to_number(right)};
        if (op == ">=") return Value{
            to_number(left) >= to_number(right)};
        if (op == "<=") return Value{
            to_number(left) <= to_number(right)};
        if (op == "&&" || op == "na") return Value{
            to_bool(left) && to_bool(right)};
        if (op == "||" || op == "au") return Value{
            to_bool(left) || to_bool(right)};

        throw std::runtime_error("Unknown binary operator '" + op + "' at " + b->token.loc.to_string());
    }

    if (auto call = dynamic_cast<CallExpressionNode*>(expr)) {
        auto eval_args = [&](std::vector<Value>& out) {
            out.clear();
            out.reserve(call->arguments.size());
            for (auto& argPtr : call->arguments) {
                if (!argPtr) {
                    out.push_back(std::monostate{});
                    continue;
                }

                if (auto spread = dynamic_cast<SpreadElementNode*>(argPtr.get())) {
                    if (!spread->argument) {
                        throw std::runtime_error("Spread argument missing expression at " + spread->token.loc.to_string());
                    }
                    Value v = evaluate_expression(spread->argument.get(), env);

                    if (std::holds_alternative<ArrayPtr>(v)) {
                        ArrayPtr src = std::get<ArrayPtr>(v);
                        if (src) {
                            for (auto& e : src->elements) out.push_back(e);
                        }
                        continue;
                    }

                    if (std::holds_alternative<std::string>(v)) {
                        std::string s = std::get<std::string>(v);
                        for (char c : s) out.push_back(Value{
                            std::string(1, c)});
                        continue;
                    }

                    throw std::runtime_error("Spread in call expects array or string at " + spread->token.loc.to_string());
                }

                out.push_back(evaluate_expression(argPtr.get(), env));
            }
        };

        // Evaluate callee first (receiver/computed parts inside callee are handled
        // by their own node logic; those nodes must also short-circuit when optional).
        Value calleeVal = evaluate_expression(call->callee.get(), env);

        // If this was an optional call and the callee is nullish, short-circuit without
        // evaluating call arguments.
        if (call->is_optional && is_nullish(calleeVal)) {
            return std::monostate{};
        }

        if (std::holds_alternative<FunctionPtr>(calleeVal)) {
            std::vector<Value> args;
            eval_args(args);
            return call_function(std::get<FunctionPtr>(calleeVal), args, call->token);
        }
        throw std::runtime_error("Attempted to call a non-function value at " + call->token.loc.to_string());
    }

    if (auto t = dynamic_cast<TernaryExpressionNode*>(expr)) {
        // Evaluate condition first
        Value condVal = evaluate_expression(t->condition.get(), env);
        if (to_bool(condVal)) {
            // condition true → evaluate thenExpr
            return evaluate_expression(t->thenExpr.get(), env);
        } else {
            // condition false → evaluate elseExpr
            return evaluate_expression(t->elseExpr.get(), env);
        }
    }

    if (auto ln = dynamic_cast<LambdaNode*>(expr)) {
        // Convert LambdaNode to a callable FunctionValue
        auto fnDecl = std::make_shared<FunctionDeclarationNode>();

        // clone params from ln->params
        fnDecl->parameters.reserve(ln->params.size());
        for (const auto& pp : ln->params) {
            if (pp)
                fnDecl->parameters.push_back(pp->clone());
            else
                fnDecl->parameters.push_back(nullptr);
        }

        if (ln->isBlock) {
            // Clone each statement so we do not mutate the original AST node (ln)
            fnDecl->body.reserve(ln->blockBody.size());
            for (const auto& sptr : ln->blockBody) {
                fnDecl->body.push_back(sptr ? sptr->clone() : nullptr);
            }
        } else {
            // expression-lambda: wrap exprBody into a ReturnStatementNode for consistent execution
            auto retStmt = std::make_unique<ReturnStatementNode>();
            retStmt->value = ln->exprBody ? ln->exprBody->clone() : nullptr;
            fnDecl->body.push_back(std::move(retStmt));
        }

        // Construct FunctionValue from the persisted declaration. FunctionValue ctor clones again as needed.
        auto fn = std::make_shared<FunctionValue>(
            std::string("<anon>"),
            fnDecl->parameters,
            fnDecl,
            env,
            ln->token);

        return fn;
    }

    if (auto ne = dynamic_cast<NewExpressionNode*>(expr)) {
        // Evaluate callee to find class (callee is usually an IdentifierNode)
        Value calleeVal = evaluate_expression(ne->callee.get(), env);
        if (!std::holds_alternative<ClassPtr>(calleeVal)) {
            throw std::runtime_error("Attempt to instantiate non-class at " + ne->token.loc.to_string());
        }
        ClassPtr cls = std::get<ClassPtr>(calleeVal);

        // create an object instance
        auto instance = std::make_shared<ObjectValue>();

        // attach a link to its class so futa / reflection can find it
        PropertyDescriptor classLink;
        classLink.value = cls;
        classLink.is_private = true;
        classLink.token = ne->token;
        instance->properties["__class__"] = std::move(classLink);

        // Evaluate and populate instance properties (non-static) in top-down class chain
        // We want super class initializers to run before derived.
        std::vector<ClassPtr> chain;
        for (ClassPtr walk = cls; walk; walk = walk->super) chain.push_back(walk);
        std::reverse(chain.begin(), chain.end());

        for (auto& c : chain) {
            if (!c || !c->body) continue;

            // Use the environment where the class was declared for evaluating instance initializers
            // and building method closures. If (for some reason) the defining_env is null, fall back
            // to the current evaluation env to preserve previous behavior.
            EnvPtr classEnv = c->defining_env ? c->defining_env : env;

            // properties
            for (auto& p : c->body->properties) {
                if (!p) continue;
                if (p->is_static) continue;
                // evaluate initializer in a child env where '$' is bound to instance so initializers can reference $
                auto initEnv = std::make_shared<Environment>(classEnv);
                Environment::Variable thisVar;
                thisVar.value = instance;
                thisVar.is_constant = false;
                initEnv->set("$", thisVar);
                Value initVal = std::monostate{};
                if (p->value) initVal = evaluate_expression(p->value.get(), initEnv);

                PropertyDescriptor pd;
                pd.value = initVal;
                pd.is_private = p->is_private;
                pd.is_locked = p->is_locked;
                pd.is_readonly = false;
                pd.token = p->token;
                instance->properties[p->name] = std::move(pd);
            }

            // methods (non-static)
            for (auto& m : c->body->methods) {
                if (!m) continue;
                if (m->is_static) continue;

                // Skip constructors and destructors: they must remain on the ClassValue (AST)
                // and be invoked by the instantiation/delete machinery (new/super/delete).
                // Materializing them onto the instance exposes them as callable instance props
                // (e.g. ob.Base(...)) which is incorrect.
                if (m->is_constructor || m->is_destructor) continue;

                // persist a FunctionDeclarationNode like other functions do
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = m->name;
                persisted->token = m->token;

                // clone parameter descriptors from ClassMethodNode::params
                persisted->parameters.reserve(m->params.size());
                for (const auto& pp : m->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // clone body statements
                persisted->body.reserve(m->body.size());
                for (const auto& s : m->body) persisted->body.push_back(s ? s->clone() : nullptr);

                // IMPORTANT: each instance method must have a closure where '$' is bound and whose parent
                // is the class's defining environment (so free identifiers captured by the method resolve
                // to the module where the class was defined).
                EnvPtr methodClosureParent = classEnv;
                EnvPtr methodClosure = std::make_shared<Environment>(methodClosureParent);
                Environment::Variable thisVar;
                thisVar.value = instance;
                thisVar.is_constant = true;  // treat $ as constant inside methods
                methodClosure->set("$", thisVar);

                // create the FunctionValue using the methodClosure (so call_function finds '$')
                auto fn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, methodClosure, persisted->token);

                PropertyDescriptor pd;
                pd.value = fn;
                pd.is_private = m->is_private;
                pd.is_locked = m->is_locked;
                pd.is_readonly = m->is_getter;
                pd.token = m->token;
                instance->properties[m->name] = std::move(pd);
            }
        }

        // Call constructor if any on the class itself (derived class constructor should be found on cls->body->methods with is_constructor)
        if (cls->body) {
            // Find constructor method in the class (only in this class, not searching super automatically)
            ClassMethodNode* ctorNode = nullptr;
            for (auto& m : cls->body->methods) {
                if (m && m->is_constructor) {
                    ctorNode = m.get();
                    break;
                }
            }

            if (ctorNode) {
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = ctorNode->name;
                persisted->token = ctorNode->token;

                // clone parameter descriptors from ctorNode->params
                persisted->parameters.reserve(ctorNode->params.size());
                for (const auto& pp : ctorNode->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // clone body statements
                persisted->body.reserve(ctorNode->body.size());
                for (const auto& stmt : ctorNode->body) {
                    persisted->body.push_back(stmt ? stmt->clone() : nullptr);
                }

                // Create FunctionValue for constructor with closure = env (the class-decl environment)
                EnvPtr ctorClosure = cls->defining_env ? cls->defining_env : env;
                auto constructorFn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, ctorClosure, persisted->token);

                // Evaluate call arguments
                std::vector<Value> ctorArgs;
                ctorArgs.reserve(ne->arguments.size());
                for (auto& a : ne->arguments) ctorArgs.push_back(evaluate_expression(a.get(), env));

                // Call constructor with the instance as receiver so $ is available in the call frame,
                // and set current_class_context so 'super(...)' works inside constructor.
                ClassPtr saved_ctx = current_class_context;
                current_class_context = cls;
                call_function_with_receiver(constructorFn, instance, ctorArgs, persisted->token);
                current_class_context = saved_ctx;
            }
        }

        // return the created instance as a Value
        return instance;
    }

    if (auto se = dynamic_cast<SuperExpressionNode*>(expr)) {
        if (!current_class_context) {
            throw std::runtime_error("super(...) may only be called inside a constructor at " + se->token.loc.to_string());
        }
        // find the instance from env via $
        EnvPtr walk = env;
        ObjectPtr receiver = nullptr;
        while (walk) {
            auto it = walk->values.find("$");
            if (it != walk->values.end()) {
                if (std::holds_alternative<ObjectPtr>(it->second.value)) {
                    receiver = std::get<ObjectPtr>(it->second.value);
                    break;
                }
            }
            walk = walk->parent;
        }
        if (!receiver) throw std::runtime_error("No '$' found for super(...) call at " + se->token.loc.to_string());

        ClassPtr parent = current_class_context->super;
        if (!parent) return std::monostate{};  // no-op if no super

        // find parent constructor
        if (!parent->body) return std::monostate{};
        for (auto& m : parent->body->methods) {
            if (m && m->is_constructor) {
                // create FunctionValue from parent constructor and call with receiver
                auto persisted = std::make_shared<FunctionDeclarationNode>();
                persisted->name = m->name;
                persisted->token = m->token;

                // clone parameters
                persisted->parameters.reserve(m->params.size());
                for (const auto& pp : m->params) {
                    if (pp)
                        persisted->parameters.push_back(pp->clone());
                    else
                        persisted->parameters.push_back(nullptr);
                }

                // clone body
                persisted->body.reserve(m->body.size());
                for (const auto& s : m->body) persisted->body.push_back(s ? s->clone() : nullptr);

                EnvPtr parentCtorClosure = parent->defining_env ? parent->defining_env : env;
                auto parentCtor = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, parentCtorClosure, persisted->token);

                std::vector<Value> args;
                args.reserve(se->arguments.size());
                for (auto& a : se->arguments) args.push_back(evaluate_expression(a.get(), env));

                // IMPORTANT: ensure nested super(...) calls inside the parent constructor
                // resolve to the parent's super. Temporarily set current_class_context to parent.
                ClassPtr saved_ctx = current_class_context;
                current_class_context = parent;
                try {
                    Value res = call_function_with_receiver(parentCtor, receiver, args, m->token);
                    current_class_context = saved_ctx;
                    return res;
                } catch (...) {
                    current_class_context = saved_ctx;
                    throw;
                }
            }
        }
        return std::monostate{};
    }

    if (auto de = dynamic_cast<DeleteExpressionNode*>(expr)) {
        Value v = evaluate_expression(de->target.get(), env);
        if (!std::holds_alternative<ObjectPtr>(v)) return std::monostate{};
        ObjectPtr obj = std::get<ObjectPtr>(v);

        // collect evaluated args
        std::vector<Value> args;
        args.reserve(de->arguments.size());
        for (auto& aexpr : de->arguments) {
            if (!aexpr)
                args.push_back(std::monostate{});
            else
                args.push_back(evaluate_expression(aexpr.get(), env));
        }

        // call destructor if class link exists
        auto it = obj->properties.find("__class__");
        if (it != obj->properties.end() && std::holds_alternative<ClassPtr>(it->second.value)) {
            ClassPtr cls = std::get<ClassPtr>(it->second.value);
            if (cls->body) {
                for (auto& m : cls->body->methods) {
                    if (m && m->is_destructor) {
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

                        EnvPtr dtorClosure = cls->defining_env ? cls->defining_env : env;
                        auto dtorFn = std::make_shared<FunctionValue>(persisted->name, persisted->parameters, persisted, dtorClosure, persisted->token);

                        // forward evaluated args into destructor call
                        Value res = call_function_with_receiver(dtorFn, obj, args, m->token);
                        obj->properties.clear();
                        return res;
                    }
                }
            }
        }

        obj->properties.clear();
        return std::monostate{};
    }

    throw std::runtime_error("Unhandled expression node in evaluator");
}