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

// time.now() -> DateTimeValue
static Value native_time_now(const std::vector<Value>&, EnvPtr, const Token&) {
    return Value{create_current_datetime()};
}

// time.date(...args) -> DateTimeValue
static Value native_time_date(const std::vector<Value>& args, EnvPtr, const Token& token) {
    if (args.empty()) {
        throw SwaziError("TypeError",
            "time.date requires at least one argument. "
            "Usage: time.date(year, month, day, [hour, minute, second, fractionalNanos, tzOffset]) "
            "or time.date(ms/ns, [\"ms\"|\"ns\"])",
            token.loc);
    }

    // Case 1: Single numeric argument - epoch time
    if (args.size() == 1 && std::holds_alternative<double>(args[0])) {
        double value = std::get<double>(args[0]);
        // Default to milliseconds
        return Value{create_datetime_from_ms(value)};
    }

    // Case 2: Two arguments with second being "ms" or "ns" - explicit epoch time
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

    // Case 3: Component form - year, month, day are required
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

    return Value{create_datetime_from_components(
        year, month, day,
        hour, minute, second,
        fractional_nanos,
        tz_offset_seconds,
        is_utc,
        precision)};
}

// Export factory
std::shared_ptr<ObjectValue> make_time_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // time.now() -> DateTimeValue
    {
        auto fn = std::make_shared<FunctionValue>(
            "time.now",
            native_time_now,
            env,
            Token{});
        obj->properties["now"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // time.date(...args) -> DateTimeValue
    {
        auto fn = std::make_shared<FunctionValue>(
            "time.date",
            native_time_date,
            env,
            Token{});
        obj->properties["date"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    return obj;
}