#include "time.hpp"
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <regex>

// epoch ms now (UTC)
double epoch_ms_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count();
    return static_cast<double>(ms);
}

// tm from epoch ms (UTC)
std::tm tm_from_ms(double ms) {
    std::time_t sec = static_cast<std::time_t>(std::llround(ms / 1000.0));
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &sec);
#else
    gmtime_r(&sec, &tm);
#endif
    return tm;
}

// parse offset strings like +03:00 into seconds
static int parse_offset_seconds(const std::string &zone) {
    if (zone.empty()) return 0;
    if (zone == "UTC" || zone == "Z" || zone == "z") return 0;
    std::smatch m;
    std::regex re(R"(^([+-])(\d{1,2}):?(\d{2})$)");
    if (std::regex_match(zone, m, re)) {
        int sign = (m[1] == "+") ? 1 : -1;
        int hh = std::stoi(m[2]);
        int mm = std::stoi(m[3]);
        return sign * (hh * 3600 + mm * 60);
    }
    // unknown zone: treat as UTC (safe fallback)
    return 0;
}

// ordinal suffix for Do token
static std::string ordinal_suffix(int day) {
    int d = day % 100;
    if (d >= 11 && d <= 13) return "th";
    switch (day % 10) {
        case 1: return "st";
        case 2: return "nd";
        case 3: return "rd";
        default: return "th";
    }
}

// Very small formatter tailored to the tokens in your spec.
std::string format_time_from_ms(double ms, const std::string& fmt, const std::string& zone) {
    int offset = parse_offset_seconds(zone);
    double adjusted_ms = ms + static_cast<double>(offset) * 1000.0; // show in that zone
    std::tm t = tm_from_ms(adjusted_ms);

    std::ostringstream out;
    size_t i = 0;
    while (i < fmt.size()) {
        // try longer tokens first
        if (i + 4 <= fmt.size()) {
            std::string s4 = fmt.substr(i, 4);
            if (s4 == "YYYY") { out << (1900 + t.tm_year); i += 4; continue; }
            if (s4 == "dddd") { std::ostringstream tmp; tmp << std::put_time(&t, "%A"); out << tmp.str(); i += 4; continue; }
            if (s4 == "MMMM") { std::ostringstream tmp; tmp << std::put_time(&t, "%B"); out << tmp.str(); i += 4; continue; }
        }
        if (i + 3 <= fmt.size()) {
            std::string s3 = fmt.substr(i, 3);
            if (s3 == "MMM") { std::ostringstream tmp; tmp << std::put_time(&t, "%b"); out << tmp.str(); i += 3; continue; }
        }
        if (i + 2 <= fmt.size()) {
            std::string s2 = fmt.substr(i, 2);
            if (s2 == "MM") { out << std::setw(2) << std::setfill('0') << (t.tm_mon + 1); i += 2; continue; }
            if (s2 == "DD") { out << std::setw(2) << std::setfill('0') << t.tm_mday; i += 2; continue; }
            if (s2 == "HH") { out << std::setw(2) << std::setfill('0') << t.tm_hour; i += 2; continue; }
            if (s2 == "hh") { int h = t.tm_hour % 12; if (h == 0) h = 12; out << std::setw(2) << std::setfill('0') << h; i += 2; continue; }
            if (s2 == "mm") { out << std::setw(2) << std::setfill('0') << t.tm_min; i += 2; continue; }
            if (s2 == "ss") { out << std::setw(2) << std::setfill('0') << t.tm_sec; i += 2; continue; }
            if (s2 == "Do") { out << t.tm_mday << ordinal_suffix(t.tm_mday); i += 2; continue; }
        }
        char c = fmt[i];
        if (c == 'H') { out << t.tm_hour; ++i; continue; }
        if (c == 'h') { int hh = t.tm_hour % 12; if (hh == 0) hh = 12; out << hh; ++i; continue; }
        if (c == 'm') { out << t.tm_min; ++i; continue; }
        if (c == 's') { out << t.tm_sec; ++i; continue; }
        if (c == 'S') { out << "000"; ++i; continue; } // milliseconds placeholder (no stored ms in tm)
        if (c == 'Z') {
            int off = parse_offset_seconds(zone);
            int oh = std::abs(off) / 3600;
            int om = (std::abs(off) % 3600) / 60;
            std::ostringstream z; z << (off >= 0 ? "+" : "-") << std::setw(2) << std::setfill('0') << oh << ":" << std::setw(2) << std::setfill('0') << om;
            out << z.str();
            ++i;
            continue;
        }
        out << c;
        ++i;
    }
    return out.str();
}

// Parse a date string. If fmt provided, try to parse with limited supported patterns.
// If zone provided, interpret parsed local time in that zone (subtract offset to obtain UTC).
double parse_time_to_ms(const std::string& s, const std::string& fmt, const std::string& zone) {
    // numeric string -> treat as epoch ms
    std::smatch m;
    std::regex num_re(R"(^\s*\d+\s*$)");
    if (std::regex_match(s, m, num_re)) {
        try { long long v = std::stoll(s); return static_cast<double>(v); } catch(...) { /* fallthrough */ }
    }

    std::tm t{};
    bool ok = false;

    if (!fmt.empty()) {
        std::string mapped;
        if (fmt == "YYYY-MM-DD") mapped = "%Y-%m-%d";
        else if (fmt == "YYYY-MM-DD H:mm:ss") mapped = "%Y-%m-%d %H:%M:%S";
        else mapped = "%Y-%m-%d"; // fallback guess

        std::istringstream iss(s);
        iss >> std::get_time(&t, mapped.c_str());
        if (!iss.fail()) ok = true;
    } else {
        // try ISO-like attempts
        std::istringstream iss1(s);
        iss1 >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S");
        if (!iss1.fail()) ok = true;
        else {
            std::istringstream iss2(s);
            iss2 >> std::get_time(&t, "%Y-%m-%d %H:%M:%S");
            if (!iss2.fail()) ok = true;
            else {
                std::istringstream iss3(s);
                iss3 >> std::get_time(&t, "%Y-%m-%d");
                if (!iss3.fail()) ok = true;
            }
        }
    }

    if (!ok) throw std::runtime_error("Failed to parse date string: " + s);

    // convert tm assumed as UTC wall-clock -> time_t using timegm
#if defined(_WIN32)
    time_t tt = _mkgmtime(&t);
#else
    time_t tt = timegm(&t);
#endif
    double ms = static_cast<double>(static_cast<long long>(tt) * 1000LL);

    if (!zone.empty()) {
        int off = parse_offset_seconds(zone);
        // parsed time was wall-clock in `zone`, so convert to UTC by subtracting the offset
        ms -= static_cast<double>(off) * 1000.0;
    }
    return ms;
}

// ------------------------------------------------------------------
// init_time: register global function muda(...) which performs the free-function behaviors.
// ------------------------------------------------------------------
void init_time(EnvPtr env) {
    if (!env) return;

    auto add_fn = [&](const std::string& name,
                      std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});
        Environment::Variable var { fn, true };
        env->set(name, var);
    };

    // Implementation of muda(...):
    // - no args: epoch ms now
    // - ("ms") -> epoch ms now
    // - (formatString) -> formatted now
    // - (ms, format) -> formatted ms
    // - (dateString) -> parse -> epoch ms
    // - (dateString, format) -> parse with format
    // - (dateString, format, zone) -> parse with zone
    auto builtin_muda = [](const std::vector<Value>& args, EnvPtr /*env*/, const Token& tok) -> Value {
        // 0 args -> now ms
        if (args.empty()) {
            return epoch_ms_now();
        }

        // 1 arg cases
        if (args.size() == 1) {
            if (std::holds_alternative<std::string>(args[0])) {
                std::string s = std::get<std::string>(args[0]);
                if (s == "ms") return epoch_ms_now();
                // treat as format -> return formatted now
                return format_time_from_ms(epoch_ms_now(), s, std::string("UTC"));
            }
            if (std::holds_alternative<double>(args[0])) {
                // single number -> treat as epoch ms and return same (echo)
                return std::get<double>(args[0]);
            }
            // otherwise unsupported single type
            throw std::runtime_error("muda: unsupported argument type at " + tok.loc.to_string());
        }

        // 2 args: (ms, format) OR (dateString, format)
        if (args.size() == 2) {
            // if first is number -> format
            if (std::holds_alternative<double>(args[0]) && std::holds_alternative<std::string>(args[1])) {
                double ms = std::get<double>(args[0]);
                std::string fmt = std::get<std::string>(args[1]);
                return format_time_from_ms(ms, fmt, std::string("UTC"));
            }
            // if first is string -> parse with fmt
            if (std::holds_alternative<std::string>(args[0]) && std::holds_alternative<std::string>(args[1])) {
                std::string s = std::get<std::string>(args[0]);
                std::string fmt = std::get<std::string>(args[1]);
                double ms = parse_time_to_ms(s, fmt, "");
                return ms;
            }
            throw std::runtime_error("muda(date, fmt) expects two strings or (ms, fmt) at " + tok.loc.to_string());
        }

        // 3 args: (dateString, format, zone)
        if (args.size() >= 3) {
            if (std::holds_alternative<std::string>(args[0]) && std::holds_alternative<std::string>(args[1]) && std::holds_alternative<std::string>(args[2])) {
                std::string s = std::get<std::string>(args[0]);
                std::string fmt = std::get<std::string>(args[1]);
                std::string zone = std::get<std::string>(args[2]);
                double ms = parse_time_to_ms(s, fmt, zone);
                return ms;
            }
            throw std::runtime_error("muda(date, fmt, zone) expects string args at " + tok.loc.to_string());
        }

        return std::monostate{};
    };

    add_fn("muda", builtin_muda);
}