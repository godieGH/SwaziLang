#pragma once
#include "ast.hpp"
#include <memory>
#include <string>
#include <iostream>
#include <iomanip>
#include <sstream>


// JSON escape helper
static std::string json_escape(const std::string &s) {
    std::ostringstream o;
    for (char c : s) {
        switch (c) {
            case '\"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\b': o << "\\b";  break;
            case '\f': o << "\\f";  break;
            case '\n': o << "\\n";  break;
            case '\r': o << "\\r";  break;
            case '\t': o << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    o << "\\u"
                      << std::hex << std::setw(4) << std::setfill('0') << (int)(unsigned char)c
                      << std::dec << std::setw(0);
                } else {
                    o << c;
                }
        }
    }
    return o.str();
}

// Forward declare so recursion compiles
std::string trycatch_to_json(const TryCatchNode& node, int indent = 2);

// Convert a single statement to JSON (handles nested TryCatchNodes recursively)
static std::string stmt_to_json(const std::unique_ptr<StatementNode>& s, int indent = 2) {
    if (!s) return "null";

    // If the statement is itself a TryCatchNode, recurse and return the JSON object
    if (auto tc = dynamic_cast<TryCatchNode*>(s.get())) {
        return trycatch_to_json(*tc, indent);
    }

    // Fallback: use to_string() of the statement and emit it as a JSON string
    // (safe because we escape it)
    std::string repr = s->to_string();
    return "\"" + json_escape(repr) + "\"";
}

// Helper to indent lines
static std::string indent_str(int level) {
    return std::string(level, ' ');
}

// Main serializer for TryCatchNode
std::string trycatch_to_json(const TryCatchNode& node, int indent) {
    std::ostringstream out;
    std::string ind = indent_str(indent);
    std::string ind2 = indent_str(indent + 2);

    out << "{\n";

    // Node type
    out << ind2 << "\"nodeType\": \"TryCatch\",\n";

    // Token: we include token type as integer (you can map it to names if you want),
    // lexeme and location string. Replace .lexeme or .loc.to_string() if your Token uses different names.
    out << ind2 << "\"token\": {\n";
    out << ind2 << "  \"type\": " << static_cast<int>(node.token.type) << ",\n";
    out << ind2 << "  \"value\": \"" << json_escape(node.token.value) << "\",\n";
    out << ind2 << "  \"location\": \"" << json_escape(node.token.loc.to_string()) << "\"\n";
    out << ind2 << "},\n";

    // error variable
    out << ind2 << "\"errorVar\": \"" << json_escape(node.errorVar) << "\",\n";

    // try block
    out << ind2 << "\"tryBlock\": [\n";
    for (size_t i = 0; i < node.tryBlock.size(); ++i) {
        out << ind2 << "  " << stmt_to_json(node.tryBlock[i], indent + 4);
        if (i + 1 < node.tryBlock.size()) out << ",";
        out << "\n";
    }
    out << ind2 << "],\n";

    // catch block
    out << ind2 << "\"catchBlock\": [\n";
    for (size_t i = 0; i < node.catchBlock.size(); ++i) {
        out << ind2 << "  " << stmt_to_json(node.catchBlock[i], indent + 4);
        if (i + 1 < node.catchBlock.size()) out << ",";
        out << "\n";
    }
    out << ind2 << "],\n";

    // finally block (may be empty)
    out << ind2 << "\"finallyBlock\": [\n";
    for (size_t i = 0; i < node.finallyBlock.size(); ++i) {
        out << ind2 << "  " << stmt_to_json(node.finallyBlock[i], indent + 4);
        if (i + 1 < node.finallyBlock.size()) out << ",";
        out << "\n";
    }
    out << ind2 << "]\n";

    out << ind << "}";
    return out.str();
}


