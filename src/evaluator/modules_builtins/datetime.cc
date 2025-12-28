#include <chrono>
#include <ctime>

#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

// Helper: Get current time as DateTimeValue with nanosecond precision
static DateTimePtr create_current_datetime() {
    using namespace std::chrono;

    auto now = system_clock::now();
    auto nanos = duration_cast<nanoseconds>(now.time_since_epoch()).count();

    auto dt = std::make_shared<DateTimeValue>();
    dt->epochNanoseconds = static_cast<uint64_t>(nanos);
    dt->fractionalNanoseconds = static_cast<uint32_t>(nanos % 1'000'000'000LL);
    dt->isUTC = true;
    dt->tzOffsetSeconds = 0;
    dt->precision = DateTimePrecision::NANOSECOND;

    // Compute calendar fields
    dt->recompute_calendar_fields();
    dt->update_literal_text();

    return dt;
}

// Helper: Create DateTimeValue from epoch milliseconds
static DateTimePtr create_datetime_from_ms(double ms) {
    auto dt = std::make_shared<DateTimeValue>();

    uint64_t nanos = static_cast<uint64_t>(ms * 1'000'000.0);
    dt->epochNanoseconds = nanos;
    dt->fractionalNanoseconds = static_cast<uint32_t>(nanos % 1'000'000'000LL);
    dt->isUTC = true;
    dt->tzOffsetSeconds = 0;
    dt->precision = DateTimePrecision::MILLISECOND;

    dt->recompute_calendar_fields();
    dt->update_literal_text();

    return dt;
}

// Helper: Create DateTimeValue from epoch nanoseconds
static DateTimePtr create_datetime_from_ns(uint64_t ns) {
    auto dt = std::make_shared<DateTimeValue>();

    dt->epochNanoseconds = ns;
    dt->fractionalNanoseconds = static_cast<uint32_t>(ns % 1'000'000'000LL);
    dt->isUTC = true;
    dt->tzOffsetSeconds = 0;
    dt->precision = DateTimePrecision::NANOSECOND;

    dt->recompute_calendar_fields();
    dt->update_literal_text();

    return dt;
}

// Helper: Create DateTimeValue from components
static DateTimePtr create_datetime_from_components(
    int year, int month, int day,
    int hour, int minute, int second,
    uint32_t fractional_nanos,
    int32_t tz_offset_seconds,
    bool is_utc,
    DateTimePrecision precision) {
    auto dt = std::make_shared<DateTimeValue>();

    dt->year = year;
    dt->month = month;
    dt->day = day;
    dt->hour = hour;
    dt->minute = minute;
    dt->second = second;
    dt->fractionalNanoseconds = fractional_nanos;
    dt->tzOffsetSeconds = tz_offset_seconds;
    dt->isUTC = is_utc;
    dt->precision = precision;

    // Compute epoch from fields
    dt->recompute_epoch_from_fields();
    dt->update_literal_text();

    return dt;
}

// Helper: Parse ISO 8601 datetime string
static DateTimePtr parse_iso_datetime(const std::string& iso_str, const Token& token) {
    // Basic ISO 8601 format: YYYY-MM-DDTHH:MM:SS[.fff][Z|±HH:MM]

    auto dt = std::make_shared<DateTimeValue>();
    dt->literalText = iso_str;

    // Use regex or manual parsing
    // For simplicity, I'll show manual parsing approach

    size_t pos = 0;

    // Parse date: YYYY-MM-DD
    if (iso_str.length() < 10) {
        throw SwaziError("ValueError",
            "Invalid ISO datetime string: too short", token.loc);
    }

    try {
        dt->year = std::stoi(iso_str.substr(0, 4));
        if (iso_str[4] != '-') throw std::invalid_argument("Expected '-'");
        dt->month = std::stoi(iso_str.substr(5, 2));
        if (iso_str[7] != '-') throw std::invalid_argument("Expected '-'");
        dt->day = std::stoi(iso_str.substr(8, 2));
        pos = 10;

        // Optional time part
        if (pos < iso_str.length() && (iso_str[pos] == 'T' || iso_str[pos] == ' ')) {
            pos++;  // skip 'T' or space

            if (pos + 8 > iso_str.length()) {
                throw std::invalid_argument("Invalid time format");
            }

            // Parse time: HH:MM:SS
            dt->hour = std::stoi(iso_str.substr(pos, 2));
            if (iso_str[pos + 2] != ':') throw std::invalid_argument("Expected ':'");
            dt->minute = std::stoi(iso_str.substr(pos + 3, 2));
            if (iso_str[pos + 5] != ':') throw std::invalid_argument("Expected ':'");
            dt->second = std::stoi(iso_str.substr(pos + 6, 2));
            pos += 8;

            // Optional fractional seconds
            if (pos < iso_str.length() && iso_str[pos] == '.') {
                pos++;  // skip '.'
                size_t frac_start = pos;
                while (pos < iso_str.length() && std::isdigit(iso_str[pos])) {
                    pos++;
                }

                std::string frac_str = iso_str.substr(frac_start, pos - frac_start);
                if (!frac_str.empty()) {
                    // Pad or truncate to 9 digits (nanoseconds)
                    frac_str.resize(9, '0');
                    dt->fractionalNanoseconds = std::stoul(frac_str);

                    // Determine precision
                    size_t frac_len = pos - frac_start;
                    if (frac_len <= 3) {
                        dt->precision = DateTimePrecision::MILLISECOND;
                    } else if (frac_len <= 6) {
                        dt->precision = DateTimePrecision::MICROSECOND;
                    } else {
                        dt->precision = DateTimePrecision::NANOSECOND;
                    }
                }
            } else {
                dt->precision = DateTimePrecision::SECOND;
            }

            // Optional timezone
            if (pos < iso_str.length()) {
                if (iso_str[pos] == 'Z') {
                    dt->isUTC = true;
                    dt->tzOffsetSeconds = 0;
                } else if (iso_str[pos] == '+' || iso_str[pos] == '-') {
                    char sign = iso_str[pos];
                    pos++;

                    // Need at least 2 characters for hours
                    if (pos + 2 > iso_str.length()) {
                        throw std::invalid_argument("Invalid timezone format");
                    }

                    int tz_hours = std::stoi(iso_str.substr(pos, 2));
                    pos += 2;

                    int tz_mins = 0;

                    // Check if there's a colon (e.g., +05:30) or more digits (e.g., +0530)
                    if (pos < iso_str.length()) {
                        if (iso_str[pos] == ':') {
                            // Format: +HH:MM
                            pos++;  // skip ':'
                            if (pos + 2 > iso_str.length()) {
                                throw std::invalid_argument("Invalid timezone format");
                            }
                            tz_mins = std::stoi(iso_str.substr(pos, 2));
                            pos += 2;
                        } else if (std::isdigit(iso_str[pos]) && pos + 2 <= iso_str.length()) {
                            // Format: +HHMM
                            tz_mins = std::stoi(iso_str.substr(pos, 2));
                            pos += 2;
                        }
                        // else: just +HH format, minutes remain 0
                    }

                    // Validate timezone offset
                    if (tz_hours < 0 || tz_hours > 23) {
                        throw SwaziError("ValueError",
                            "Invalid timezone hour: " + std::to_string(tz_hours) + " (must be 0-23)",
                            token.loc);
                    }
                    if (tz_mins < 0 || tz_mins > 59) {
                        throw SwaziError("ValueError",
                            "Invalid timezone minute: " + std::to_string(tz_mins) + " (must be 0-59)",
                            token.loc);
                    }

                    dt->tzOffsetSeconds = (tz_hours * 3600 + tz_mins * 60);
                    if (sign == '-') dt->tzOffsetSeconds = -dt->tzOffsetSeconds;
                    dt->isUTC = (dt->tzOffsetSeconds == 0);
                }
            } else {
                // No timezone specified - assume UTC
                dt->isUTC = true;
                dt->tzOffsetSeconds = 0;
            }
        } else {
            // Date only, set time to midnight UTC
            dt->hour = 0;
            dt->minute = 0;
            dt->second = 0;
            dt->fractionalNanoseconds = 0;
            dt->isUTC = true;
            dt->tzOffsetSeconds = 0;
            dt->precision = DateTimePrecision::SECOND;
        }

    } catch (const std::exception& e) {
        throw SwaziError("ValueError",
            "Failed to parse ISO datetime string '" + iso_str + "': " + e.what(),
            token.loc);
    }

    // Validate ranges
    if (dt->month < 1 || dt->month > 12) {
        throw SwaziError("ValueError",
            "Invalid month: " + std::to_string(dt->month), token.loc);
    }
    if (dt->day < 1 || dt->day > 31) {
        throw SwaziError("ValueError",
            "Invalid day: " + std::to_string(dt->day), token.loc);
    }
    if (dt->hour < 0 || dt->hour > 23) {
        throw SwaziError("ValueError",
            "Invalid hour: " + std::to_string(dt->hour), token.loc);
    }
    if (dt->minute < 0 || dt->minute > 59) {
        throw SwaziError("ValueError",
            "Invalid minute: " + std::to_string(dt->minute), token.loc);
    }
    if (dt->second < 0 || dt->second > 59) {
        throw SwaziError("ValueError",
            "Invalid second: " + std::to_string(dt->second), token.loc);
    }

    // Validate date (after parsing year, month, day)
    if (!is_valid_date(dt->year, dt->month, dt->day)) {
        throw SwaziError("ValueError",
            "Invalid date: " + std::to_string(dt->year) + "-" +
                std::to_string(dt->month) + "-" + std::to_string(dt->day),
            token.loc);
    }

    // Compute epoch from fields
    dt->recompute_epoch_from_fields();

    return dt;
}

// datetime.now() -> DateTimeValue
static Value native_datetime_now(const std::vector<Value>&, EnvPtr, const Token&) {
    return Value{create_current_datetime()};
}

// datetime.date(...args) -> DateTimeValue
static Value native_datetime_date(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError",
            "time.date requires at least one argument. "
            "Usage: time.date(isoString) "
            "or time.date(year, month, day, [hour, minute, second, fractionalNanos, tzOffset]) "
            "or time.date(ms/ns, [\"ms\"|\"ns\"])",
            token.loc);
    }

    // Case 1: Single string argument - ISO datetime string
    if (args.size() == 1 && std::holds_alternative<std::string>(args[0])) {
        std::string iso_str = std::get<std::string>(args[0]);
        return Value{parse_iso_datetime(iso_str, token)};
    }

    // Case 2: Single numeric argument - epoch time (milliseconds)
    if (args.size() == 1 && std::holds_alternative<double>(args[0])) {
        double value = std::get<double>(args[0]);
        return Value{create_datetime_from_ms(value)};
    }

    // Case 3: Two arguments with second being "ms" or "ns" - explicit epoch time
    if (args.size() == 2 &&
        std::holds_alternative<double>(args[0]) &&
        std::holds_alternative<std::string>(args[1])) {
        double value = std::get<double>(args[0]);
        std::string unit = std::get<std::string>(args[1]);

        if (unit == "ms") {
            return Value{create_datetime_from_ms(value)};
        } else if (unit == "ns") {
            uint64_t ns = static_cast<uint64_t>(value);
            return Value{create_datetime_from_ns(ns)};
        } else {
            throw SwaziError("ValueError",
                "Unit must be \"ms\" or \"ns\", got: " + unit,
                token.loc);
        }
    }

    // Case 4: Component form - year, month, day are required
    if (args.size() < 3) {
        throw SwaziError("TypeError",
            "time.date requires at least 3 arguments (year, month, day) for component form",
            token.loc);
    }

    // Extract required components
    if (!std::holds_alternative<double>(args[0]) ||
        !std::holds_alternative<double>(args[1]) ||
        !std::holds_alternative<double>(args[2])) {
        throw SwaziError("TypeError",
            "year, month, and day must be numbers",
            token.loc);
    }

    int year = static_cast<int>(std::get<double>(args[0]));
    int month = static_cast<int>(std::get<double>(args[1]));
    int day = static_cast<int>(std::get<double>(args[2]));

    // Extract optional components
    int hour = 0;
    int minute = 0;
    int second = 0;
    uint32_t fractional_nanos = 0;
    int32_t tz_offset_seconds = 0;
    bool is_utc = true;
    DateTimePrecision precision = DateTimePrecision::SECOND;

    if (args.size() >= 4 && std::holds_alternative<double>(args[3])) {
        hour = static_cast<int>(std::get<double>(args[3]));
    }

    if (args.size() >= 5 && std::holds_alternative<double>(args[4])) {
        minute = static_cast<int>(std::get<double>(args[4]));
    }

    if (args.size() >= 6 && std::holds_alternative<double>(args[5])) {
        second = static_cast<int>(std::get<double>(args[5]));
    }

    if (args.size() >= 7 && std::holds_alternative<double>(args[6])) {
        fractional_nanos = static_cast<uint32_t>(std::get<double>(args[6]));
        // Determine precision from fractional nanoseconds
        if (fractional_nanos > 0) {
            if (fractional_nanos < 1'000'000) {
                precision = DateTimePrecision::NANOSECOND;
            } else if (fractional_nanos < 1'000'000'000) {
                precision = DateTimePrecision::MICROSECOND;
            } else {
                precision = DateTimePrecision::MILLISECOND;
            }
        }
    }

    if (args.size() >= 8 && std::holds_alternative<double>(args[7])) {
        tz_offset_seconds = static_cast<int32_t>(std::get<double>(args[7]));
        is_utc = (tz_offset_seconds == 0);
    }

    // Validate ranges
    if (month < 1 || month > 12) {
        throw SwaziError("ValueError",
            "month must be between 1 and 12, got: " + std::to_string(month),
            token.loc);
    }

    if (day < 1 || day > 31) {
        throw SwaziError("ValueError",
            "day must be between 1 and 31, got: " + std::to_string(day),
            token.loc);
    }

    if (hour < 0 || hour > 23) {
        throw SwaziError("ValueError",
            "hour must be between 0 and 23, got: " + std::to_string(hour),
            token.loc);
    }

    if (minute < 0 || minute > 59) {
        throw SwaziError("ValueError",
            "minute must be between 0 and 59, got: " + std::to_string(minute),
            token.loc);
    }

    if (second < 0 || second > 59) {
        throw SwaziError("ValueError",
            "second must be between 0 and 59, got: " + std::to_string(second),
            token.loc);
    }

    if (fractional_nanos >= 1'000'000'000) {
        throw SwaziError("ValueError",
            "fractionalNanoseconds must be less than 1,000,000,000 (1 second), got: " +
                std::to_string(fractional_nanos),
            token.loc);
    }

    // Validate date components together
    if (!is_valid_date(year, month, day)) {
        throw SwaziError("ValueError",
            "Invalid date: " + std::to_string(year) + "-" +
                std::to_string(month) + "-" + std::to_string(day) +
                " (e.g., Feb 30 doesn't exist)",
            token.loc);
    }

    return Value{create_datetime_from_components(
        year, month, day,
        hour, minute, second,
        fractional_nanos,
        tz_offset_seconds,
        is_utc,
        precision)};
}

// Export factory
std::shared_ptr<ObjectValue> make_datetime_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // datetime.now() -> DateTimeValue
    {
        auto fn = std::make_shared<FunctionValue>(
            "time.now",
            native_datetime_now,
            env,
            Token{});
        obj->properties["now"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // datetime.date(...args) -> DateTimeValue
    {
        auto fn = std::make_shared<FunctionValue>(
            "time.date",
            native_datetime_date,
            env,
            Token{});
        obj->properties["date"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}