#include <re2/re2.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

#include "SwaziError.hpp"
#include "evaluator.hpp"

namespace {

// Helper: convert Value -> string
std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Helper: create native function
template <typename F>
FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    return std::make_shared<FunctionValue>(name, native_impl, env, Token());
}

// Validate regex flags
void validateFlags(const std::string& flags, const Token& token) {
    for (char c : flags) {
        if (c != 'g' && c != 'i' && c != 'm' && c != 's' && c != 'u') {
            throw SwaziError("SyntaxError",
                std::string("Invalid regex flag '") + c +
                    "'. Valid flags: g (global), i (ignoreCase), m (multiline), s (dotAll), u (unicode)",
                token.loc);
        }
    }
}

// Create a match result object with proper structure
ObjectPtr createMatchResult(
    const std::vector<re2::StringPiece>& groups,
    size_t matchPos,
    const std::string& input,
    const std::vector<std::string>& groupNames,
    const std::map<std::string, int>& nameToIndex,
    const Token& token) {
    auto result = std::make_shared<ObjectValue>();

    // Add numeric indices: "0" (full match), "1", "2", etc. (capture groups)
    for (size_t i = 0; i < groups.size(); ++i) {
        std::string captured = std::string(groups[i].data(), groups[i].size());
        result->properties[std::to_string(i)] = PropertyDescriptor{
            Value{captured}, false, false, true, token};
    }

    // Add metadata
    result->properties["index"] = PropertyDescriptor{
        Value{static_cast<double>(matchPos)}, false, false, true, token};

    result->properties["input"] = PropertyDescriptor{
        Value{input}, false, false, true, token};

    result->properties["length"] = PropertyDescriptor{
        Value{static_cast<double>(groups[0].length())}, false, false, true, token};

    // Add named groups object (if any named groups exist)
    if (!groupNames.empty()) {
        auto groupsObj = std::make_shared<ObjectValue>();

        for (const auto& [name, idx] : nameToIndex) {
            if (idx >= 0 && static_cast<size_t>(idx) < groups.size()) {
                groupsObj->properties[name] = PropertyDescriptor{
                    Value{std::string(groups[idx].data(), groups[idx].size())},
                    false, false, true, token};
            }
        }

        result->properties["groups"] = PropertyDescriptor{
            Value{groupsObj}, false, false, true, token};
    } else {
        result->properties["groups"] = PropertyDescriptor{
            Value{std::monostate{}}, false, false, true, token};
    }

    return result;
}

}  // anonymous namespace

std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // ==================== Constructor ====================

    // regex(pattern, flags?) -> RegexValue
    {
        auto fn = make_native_fn("regex", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("TypeError", 
                        "regex() requires a pattern string. Usage: regex(pattern, flags?)", 
                        token.loc);
                }
                
                std::string pattern = value_to_string_simple(args[0]);
                std::string flags = args.size() >= 2 ? value_to_string_simple(args[1]) : "";
                
                validateFlags(flags, token);
                
                // Create regex (validation happens in getCompiled())
                auto regex = std::make_shared<RegexValue>(pattern, flags);
                
                // Force compilation to catch errors early
                try {
                    regex->getCompiled();
                } catch (const std::exception& e) {
                    throw SwaziError("SyntaxError", 
                        std::string("Invalid regex pattern: ") + e.what(), 
                        token.loc);
                }
                
                return Value{regex}; }, env);

        obj->properties["regex"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ==================== Utility Functions ====================

    // escape(str) -> escaped string for use in regex
    {
        auto fn = make_native_fn("escape", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("TypeError", 
                        "regex.escape() requires a string argument", 
                        token.loc);
                }
                
                std::string str = value_to_string_simple(args[0]);
                return Value{re2::RE2::QuoteMeta(str)}; }, env);

        obj->properties["escape"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // isValid(pattern, flags?) -> bool
    {
        auto fn = make_native_fn("isValid", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
                if (args.empty()) {
                    return Value{false};
                }

                std::string pattern = value_to_string_simple(args[0]);
                std::string flags = args.size() >= 2 ? value_to_string_simple(args[1]) : "";

                try {
                    auto regex = std::make_shared<RegexValue>(pattern, flags);
                    regex->getCompiled();
                    return Value{true};
                } catch (...) {
                    return Value{false};
                } }, env);

        obj->properties["isValid"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}