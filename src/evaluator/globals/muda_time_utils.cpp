#include "token.hpp"
#include "SwaziError.hpp"
#include "muda_time_utils.hpp"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>

#if !defined(_WIN32)
#include <time.h> // strptime, timegm
#endif

using namespace std;

// A few permissive ISO-like parses: epoch (digits), YYYY-MM-DD[ H:MM[:SS]]
double parse_iso_like_local(const std::string &s, Token token) {
    // numeric epoch ms?
    bool allDigits = !s.empty() && (std::all_of(s.begin(), s.end(), [](char c){
        return std::isdigit((unsigned char)c) || c=='+' || c=='-' || c=='.';
    }));
    if (allDigits) {
        try {
            long long v = std::stoll(s);
            return static_cast<double>(v);
        } catch (...) { /* fallthrough */ }
    }

    int year=0, mon=0, day=0, hour=0, min=0, sec=0;
    if (sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d", &year,&mon,&day,&hour,&min,&sec) >= 3) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = mon - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
#if defined(_WIN32)
        time_t tt = _mkgmtime(&tm);
#else
        time_t tt = timegm(&tm);
#endif
        return static_cast<double>(static_cast<long long>(tt) * 1000LL);
    }
    if (sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d", &year,&mon,&day,&hour,&min) >= 3) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = mon - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = 0;
#if defined(_WIN32)
        time_t tt = _mkgmtime(&tm);
#else
        time_t tt = timegm(&tm);
#endif
        return static_cast<double>(static_cast<long long>(tt) * 1000LL);
    }
    if (sscanf(s.c_str(), "%4d-%2d-%2d", &year,&mon,&day) == 3) {
        std::tm tm = {};
        tm.tm_year = year - 1900;
        tm.tm_mon = mon - 1;
        tm.tm_mday = day;
#if defined(_WIN32)
        time_t tt = _mkgmtime(&tm);
#else
        time_t tt = timegm(&tm);
#endif
        return static_cast<double>(static_cast<long long>(tt) * 1000LL);
    }
    throw SwaziError("RuntimeError", std::string("Unrecognized date string: ") + s, token.loc);
}

// parse with user-provided format through strptime (POSIX) or std::get_time on Windows
double parse_date_string_with_format_local(const std::string &input, const std::string &userFmt, Token token) {
    // map user tokens to strptime tokens, simple mapping
    auto convert_user_fmt_to_strptime = [](const std::string &fmt)->std::string {
        std::string out = fmt;
        auto replace_all = [&](const std::string &from, const std::string &to) {
            size_t pos = 0;
            while ((pos = out.find(from, pos)) != std::string::npos) {
                out.replace(pos, from.size(), to);
                pos += to.size();
            }
        };
        replace_all("YYYY", "%Y");
        replace_all("YY", "%y");
        replace_all("MMMM", "%B");
        replace_all("MMM", "%b");
        replace_all("MM", "%m");
        replace_all("DD", "%d");
        replace_all("Do", "%d");
        replace_all("H", "%H");
        replace_all("HH", "%H");
        replace_all("mm", "%M");
        replace_all("ss", "%S");
        return out;
    };

    std::string fmt = convert_user_fmt_to_strptime(userFmt);
    std::tm tm = {};
#if !defined(_WIN32)
    // POSIX: use strptime which is widely available on Linux/Android
    char *res = strptime(input.c_str(), fmt.c_str(), &tm);
    if (!res) {
        throw SwaziError("RuntimeError", std::string("Failed to parse date '") + input + "' with format '" + userFmt + "'", token.loc);
    }
    // convert to epoch ms (UTC)
#if defined(_WIN32)
    time_t tt = _mkgmtime(&tm);
#else
    time_t tt = timegm(&tm);
#endif
    return static_cast<double>(static_cast<long long>(tt) * 1000LL);
#else
    // Windows: fallback to std::get_time
    std::istringstream iss(input);
    iss >> std::get_time(&tm, fmt.c_str());
    if (iss.fail()) {
        throw SwaziError("RuntimeError", std::string("Failed to parse date '") + input + "' with format '" + userFmt + "'", token.loc);
    }
#if defined(_WIN32)
    time_t tt = _mkgmtime(&tm);
#else
    time_t tt = timegm(&tm);
#endif
    return static_cast<double>(static_cast<long long>(tt) * 1000LL);
#endif
}

// Convert a Value to epoch milliseconds (accepts number or parsable string)
double value_to_ms_or_throw(const Value& v, Token token) {
    if (std::holds_alternative<double>(v)) return std::get<double>(v);
    if (std::holds_alternative<std::string>(v)) {
        const std::string &s = std::get<std::string>(v);
        // try numeric string first
        try {
            long long val = std::stoll(s);
            return static_cast<double>(val);
        } catch (...) {}
        // if not numeric, try permissive ISO-like parse
        return parse_iso_like_local(s, token);
    }
    throw SwaziError("RuntimeError", "Expected numeric epoch ms or parsable date string", token.loc);
}