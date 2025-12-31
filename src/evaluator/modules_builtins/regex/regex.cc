#include <algorithm>
#include <regex>
#include <sstream>

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
                std::string("Invalid regex flag '") + c + "'. Valid flags: g, i, m, s, u",
                token.loc);
        }
    }
}

// Helper to extract pattern and flags from RegexValue or string
struct PatternInfo {
    std::string pattern;
    std::string flags;
    bool global;
    bool ignoreCase;
    bool multiline;
    std::regex compiled;
};

PatternInfo get_pattern_info(const Value& pattern_arg, const std::string& flags_arg, const Token& token) {
    PatternInfo info;

    // If it's a RegexValue, use its pattern and flags (ignore flags_arg)
    if (std::holds_alternative<RegexPtr>(pattern_arg)) {
        RegexPtr regex = std::get<RegexPtr>(pattern_arg);
        info.pattern = regex->pattern;
        info.flags = regex->flags;
        info.global = regex->global;
        info.ignoreCase = regex->ignoreCase;
        info.multiline = regex->multiline;
        info.compiled = regex->getCompiled();
        return info;
    }

    // Otherwise it's a string pattern with separate flags
    info.pattern = value_to_string_simple(pattern_arg);
    info.flags = flags_arg;
    info.global = info.flags.find('g') != std::string::npos;
    info.ignoreCase = info.flags.find('i') != std::string::npos;
    info.multiline = info.flags.find('m') != std::string::npos;

    validateFlags(info.flags, token);

    // Compile regex
    std::regex_constants::syntax_option_type opts = std::regex_constants::ECMAScript;
    if (info.ignoreCase) {
        opts |= std::regex_constants::icase;
    }

    try {
        info.compiled = std::regex(info.pattern, opts);
    } catch (const std::regex_error& e) {
        throw SwaziError("SyntaxError",
            std::string("Invalid regex pattern: ") + e.what(),
            token.loc);
    }

    return info;
}

}  // anonymous namespace

std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // ==================== NEW API (Constructor) ====================

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
            
            // Test if pattern is valid
            try {
                std::regex_constants::syntax_option_type opts = std::regex_constants::ECMAScript;
                if (flags.find('i') != std::string::npos) {
                    opts |= std::regex_constants::icase;
                }
                std::regex test(pattern, opts);
            } catch (const std::regex_error& e) {
                throw SwaziError("SyntaxError", 
                    std::string("Invalid regex pattern: ") + e.what(), 
                    token.loc);
            }
            
            auto regex = std::make_shared<RegexValue>(pattern, flags);
            return Value{regex}; }, env);

        obj->properties["regex"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // escape(str) -> escaped string for use in regex
    {
        auto fn = make_native_fn("escape", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", 
                    "regex.escape() requires a string argument", 
                    token.loc);
            }
            
            std::string str = value_to_string_simple(args[0]);
            std::string escaped;
            
            // Escape special regex characters
            const std::string special = R"(\.^$*+?()[]{}|)";
            for (char c : str) {
                if (special.find(c) != std::string::npos) {
                    escaped += '\\';
                }
                escaped += c;
            }
            
            return Value{escaped}; }, env);

        obj->properties["escape"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ==================== OLD API (Standalone Functions) ====================

    // test(str, pattern, flags?) -> bool
    {
        auto fn = make_native_fn("test", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.test() requires at least 2 arguments: str and pattern. Usage: test(str, pattern, flags?) -> bool",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);
            
            try {
                std::smatch match;
                return Value{static_cast<bool>(std::regex_search(str, match, info.compiled))};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["test"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // match(str, pattern, flags?) -> array|null
    {
        auto fn = make_native_fn("match", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.match() requires at least 2 arguments: str and pattern. Usage: match(str, pattern, flags?) -> array|null",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            try {
                if (info.global) {
                    // Global: return all matches (no capture groups)
                    auto arr = std::make_shared<ArrayValue>();
                    std::sregex_iterator it(str.begin(), str.end(), info.compiled);
                    std::sregex_iterator end;

                    for (; it != end; ++it) {
                        arr->elements.push_back(Value{it->str()});
                    }

                    return arr->elements.empty() ? Value{std::monostate{}} : Value{arr};
                } else {
                    // Non-global: return first match with capture groups
                    std::smatch match;
                    if (!std::regex_search(str, match, info.compiled)) {
                        return Value{std::monostate{}};
                    }

                    auto arr = std::make_shared<ArrayValue>();
                    for (size_t i = 0; i < match.size(); ++i) {
                        arr->elements.push_back(Value{match[i].str()});
                    }
                    return Value{arr};
                }
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["match"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // fullmatch(str, pattern, flags?) -> bool
    {
        auto fn = make_native_fn("fullmatch", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.fullmatch() requires at least 2 arguments: str and pattern. Usage: fullmatch(str, pattern, flags?) -> bool",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            try {
                std::smatch match;
                return Value{static_cast<bool>(std::regex_match(str, match, info.compiled))};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["fullmatch"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // search(str, pattern, flags?) -> number (index) or -1
    {
        auto fn = make_native_fn("search", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.search() requires at least 2 arguments: str and pattern. Usage: search(str, pattern, flags?) -> number",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            try {
                std::smatch match;
                if (std::regex_search(str, match, info.compiled)) {
                    return Value{static_cast<double>(match.position())};
                }
                return Value{-1.0};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["search"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // replace(str, pattern, replacement, flags?) -> string
    {
        auto fn = make_native_fn("replace", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError",
                    "regex.replace() requires at least 3 arguments: str, pattern, replacement. Usage: replace(str, pattern, replacement, flags?) -> string",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string replacement = value_to_string_simple(args[2]);
            std::string flags = args.size() >= 4 ? value_to_string_simple(args[3]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            try {
                if (info.global) {
                    // Replace all occurrences
                    return Value{std::regex_replace(str, info.compiled, replacement)};
                } else {
                    // Replace only first match
                    std::smatch match;
                    if (std::regex_search(str, match, info.compiled)) {
                        return Value{match.prefix().str() + replacement + match.suffix().str()};
                    }
                    return Value{str};
                }
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["replace"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // replaceAll(str, pattern, replacement, flags?) -> string
    {
        auto fn = make_native_fn("replaceAll", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("TypeError",
                    "regex.replaceAll() requires at least 3 arguments: str, pattern, replacement",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string replacement = value_to_string_simple(args[2]);
            std::string flags = args.size() >= 4 ? value_to_string_simple(args[3]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            try {
                return Value{std::regex_replace(str, info.compiled, replacement)};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["replaceAll"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // split(str, pattern, flags?) -> array
    {
        auto fn = make_native_fn("split", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.split() requires at least 2 arguments: str and pattern. Usage: split(str, pattern, flags?) -> array",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            auto arr = std::make_shared<ArrayValue>();
            
            try {
                std::sregex_token_iterator it(str.begin(), str.end(), info.compiled, -1);
                std::sregex_token_iterator end;

                for (; it != end; ++it) {
                    arr->elements.push_back(Value{it->str()});
                }

                return Value{arr};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["split"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // findall(str, pattern, flags?) -> array (always returns array, even empty)
    {
        auto fn = make_native_fn("findall", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.findall() requires at least 2 arguments: str and pattern",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);
            
            auto arr = std::make_shared<ArrayValue>();

            try {
                std::sregex_iterator it(str.begin(), str.end(), info.compiled);
                std::sregex_iterator end;

                for (; it != end; ++it) {
                    // If there are capture groups, return array of groups
                    if (it->size() > 1) {
                        auto groups = std::make_shared<ArrayValue>();
                        for (size_t i = 0; i < it->size(); ++i) {
                            groups->elements.push_back(Value{(*it)[i].str()});
                        }
                        arr->elements.push_back(Value{groups});
                    } else {
                        // No capture groups, just the match
                        arr->elements.push_back(Value{it->str()});
                    }
                }

                return Value{arr};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["findall"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // count(str, pattern, flags?) -> number (count of matches)
    {
        auto fn = make_native_fn("count", [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError",
                    "regex.count() requires at least 2 arguments: str and pattern",
                    token.loc);
            }

            std::string str = value_to_string_simple(args[0]);
            std::string flags = args.size() >= 3 ? value_to_string_simple(args[2]) : "";
            
            PatternInfo info = get_pattern_info(args[1], flags, token);

            try {
                std::sregex_iterator it(str.begin(), str.end(), info.compiled);
                std::sregex_iterator end;
                size_t count = std::distance(it, end);
                return Value{static_cast<double>(count)};
            } catch (const std::regex_error& e) {
                throw SwaziError("RegexError", std::string("Regex error: ") + e.what(), token.loc);
            } }, env);
        obj->properties["count"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}
