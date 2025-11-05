#pragma once
#include <string>

#include "token.hpp"

class SwaziError : public std::runtime_error {
   public:
    SwaziError(const std::string& type,
        const std::string& message,
        const TokenLocation& loc) : std::runtime_error(format_message(type, message, loc)) {}

   private:
    static std::string format_message(const std::string& type,
        const std::string& message,
        const TokenLocation& loc) {
        return type + " at " + loc.to_string() + "\n" +
            message + "\n" +
            " --> Traced at:\n" +
            loc.get_line_trace();
    }
};