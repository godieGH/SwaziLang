#pragma once

#include <chrono>
#include <ctime>
#include <functional>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "ast.hpp"
#include "token.hpp"

// Forward declaration
class Environment;

// NOTE: Do NOT include Scheduler.hpp or Frame.hpp here to avoid include cycles.
// Forward-declare Scheduler so Evaluator can keep a pointer.
class Scheduler;

// BufferValue Forward-declarations
struct BufferValue;
using BufferPtr = std::shared_ptr<BufferValue>;

struct FileValue;
using FilePtr = std::shared_ptr<FileValue>;

// Helper function for date validation
inline bool is_valid_date(int year, int month, int day) {
    if (month < 1 || month > 12) return false;
    if (day < 1) return false;

    static const int days_in_month[] =
        {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    int max_day = days_in_month[month - 1];

    // Leap year check for February
    if (month == 2) {
        bool is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (is_leap) max_day = 29;
    }

    return day <= max_day;
}

struct BufferValue {
    std::vector<uint8_t> data;  // Raw bytes

    // Optionally adding encoding info if we want to support
    // conversions to/from strings with specific encodings
    std::string encoding;  // "utf8", "latin1", "binary", etc.
};
struct FileValue {
    std::string path;
    std::string mode;  // "r", "w", "a", "r+", "w+", "a+"
    bool is_open = false;
    bool is_binary = false;  // track if opened in binary mode

// Platform-specific file handle
#ifdef _WIN32
    void* handle = nullptr;  // HANDLE on Windows
#else
    int fd = -1;  // file descriptor on Unix
#endif

    // Buffered I/O state
    std::vector<uint8_t> buffer;
    size_t buffer_pos = 0;
    size_t file_pos = 0;  // logical position in file

    // Error tracking
    std::string last_error;

    ~FileValue() {
        // RAII: close on destruction if still open
        if (is_open) {
            close_internal();
        }
    }
    void close_internal();  // defined in file.cpp
};

// Forward-declare GeneratorValue used for generator objects at runtime.
struct GeneratorValue;
using GeneratorPtr = std::shared_ptr<GeneratorValue>;

// Our language's value types
struct FunctionValue;
using FunctionPtr = std::shared_ptr<FunctionValue>;

// Environment
using EnvPtr = std::shared_ptr<Environment>;

// Forward-declare ArrayValue so Value can hold a pointer to it (avoids recursive-instantiation issues)
struct ArrayValue;
using ArrayPtr = std::shared_ptr<ArrayValue>;

struct ClassValue;
using ClassPtr = std::shared_ptr<ClassValue>;

struct ObjectValue;
using ObjectPtr = std::shared_ptr<ObjectValue>;

struct CallFrame;
using CallFramePtr = std::shared_ptr<CallFrame>;

struct PromiseValue;
using PromisePtr = std::shared_ptr<PromiseValue>;

struct HoleValue {};

struct RangeValue {
    int start;
    int end;
    size_t step;
    int cur;
    bool inclusive;
    bool increasing;

    // Constructor
    RangeValue(int s, int e, size_t st = 1, bool inc = false)
        : start(s), end(e), step(st), cur(s), inclusive(inc) {
        if (step == 0) step = 1;    // step cannot be 0
        increasing = start <= end;  // infer direction
    }

    // Returns true if there is a next value
    bool hasNext() const {
        if (increasing) {
            return inclusive ? cur <= end : cur < end;
        } else {
            return inclusive ? cur >= end : cur > end;
        }
    }

    // Return current value and advance
    int next() {
        int val = cur;
        if (increasing) {
            cur += (int)step;
        } else {
            cur -= (int)step;
        }
        return val;
    }
};
using RangePtr = std::shared_ptr<RangeValue>;

struct DateTimeValue;
using DateTimePtr = std::shared_ptr<DateTimeValue>;

using Value = std::variant<
    std::monostate,
    double,
    std::string,
    bool,
    FunctionPtr,
    HoleValue,
    ArrayPtr,
    ObjectPtr,
    ClassPtr,
    PromisePtr,
    GeneratorPtr,
    BufferPtr,
    FilePtr,
    RangePtr,
    DateTimePtr>;

struct DateTimeValue {
    std::string literalText;
    int year, month, day;
    int hour, minute, second;
    uint32_t fractionalNanoseconds;
    DateTimePrecision precision;
    int32_t tzOffsetSeconds;
    bool isUTC;
    uint64_t epochNanoseconds;

    DateTimeValue() = default;

    DateTimeValue(const DateTimeLiteralNode* node) {
        literalText = node->literalText;
        year = node->year;
        month = node->month;
        day = node->day;
        hour = node->hour;
        minute = node->minute;
        second = node->second;
        fractionalNanoseconds = node->fractionalNanoseconds;
        precision = node->precision;
        tzOffsetSeconds = node->tzOffsetSeconds;
        isUTC = node->isUTC;
        epochNanoseconds = node->epochNanoseconds;
    }

    // Recompute calendar fields from epochNanoseconds
    void recompute_calendar_fields() {
        using namespace std::chrono;

        // Work directly with nanoseconds to preserve precision
        // Convert to seconds for time_t, keep fractional part separate
        int64_t total_nanos = static_cast<int64_t>(epochNanoseconds);

        // Adjust for timezone offset (convert offset to nanoseconds)
        int64_t tz_offset_nanos = static_cast<int64_t>(tzOffsetSeconds) * 1'000'000'000LL;

        // FIX: local wall-clock = UTC instant + tz offset (not minus)
        int64_t adjusted_nanos = total_nanos + tz_offset_nanos;

        // Split into seconds and fractional nanoseconds
        int64_t total_seconds = adjusted_nanos / 1'000'000'000LL;
        int64_t frac_nanos = adjusted_nanos % 1'000'000'000LL;

        // Handle negative fractional part
        if (frac_nanos < 0) {
            total_seconds -= 1;
            frac_nanos += 1'000'000'000LL;
        }

        // Convert seconds to time_t for calendar decomposition
        std::time_t tt = static_cast<std::time_t>(total_seconds);

        // Get broken-down time (UTC)
        std::tm* tm_ptr = std::gmtime(&tt);
        if (!tm_ptr) {
            throw std::runtime_error("Failed to convert epoch to calendar time");
        }

        year = tm_ptr->tm_year + 1900;
        month = tm_ptr->tm_mon + 1;
        day = tm_ptr->tm_mday;
        hour = tm_ptr->tm_hour;
        minute = tm_ptr->tm_min;
        second = tm_ptr->tm_sec;

        // Store the fractional nanoseconds (preserves full precision)
        fractionalNanoseconds = static_cast<uint32_t>(frac_nanos);
    }

    // Update epochNanoseconds from calendar fields (useful for manual field edits)
    void recompute_epoch_from_fields() {
        std::tm timeStruct = {};
        timeStruct.tm_year = year - 1900;
        timeStruct.tm_mon = month - 1;
        timeStruct.tm_mday = day;
        timeStruct.tm_hour = hour;
        timeStruct.tm_min = minute;
        timeStruct.tm_sec = second;
        timeStruct.tm_isdst = -1;

// Convert to time_t interpreting as UTC
#ifdef _WIN32
        std::time_t tt = _mkgmtime(&timeStruct);
#else
        std::time_t tt = timegm(&timeStruct);
#endif

        if (tt == -1) {
            throw std::runtime_error("Invalid date/time fields");
        }

        // tt is now UTC epoch seconds
        // Subtract timezone offset to get the actual UTC instant
        // (fields are in local timezone, we need UTC epoch)
        using namespace std::chrono;
        auto tp = system_clock::from_time_t(tt);
        tp -= seconds(tzOffsetSeconds);

        // Convert to nanoseconds
        auto since_epoch = tp.time_since_epoch();
        auto nanos = duration_cast<nanoseconds>(since_epoch).count();

        epochNanoseconds = static_cast<uint64_t>(nanos) + fractionalNanoseconds;
    }

    // Subtract another datetime (returns milliseconds difference)
    double subtract_datetime(const DateTimeValue& other) const {
        int64_t diffNanos = static_cast<int64_t>(epochNanoseconds) -
            static_cast<int64_t>(other.epochNanoseconds);
        return static_cast<double>(diffNanos) / 1000000.0;
    }

    // Format using strftime-style format string
    std::string format(const std::string& fmt) const {
        std::tm timeStruct = {};
        timeStruct.tm_year = year - 1900;
        timeStruct.tm_mon = month - 1;
        timeStruct.tm_mday = day;
        timeStruct.tm_hour = hour;
        timeStruct.tm_min = minute;
        timeStruct.tm_sec = second;
        timeStruct.tm_isdst = -1;

        // Compute day of week (0=Sunday)
        std::time_t tt = std::mktime(&timeStruct);
        std::tm* tm_with_wday = std::localtime(&tt);
        if (tm_with_wday) {
            timeStruct.tm_wday = tm_with_wday->tm_wday;
            timeStruct.tm_yday = tm_with_wday->tm_yday;
        }

        std::string processedFmt = fmt;

        // Handle custom format codes not in standard strftime

        // %f - microseconds (6 digits)
        size_t pos = 0;
        while ((pos = processedFmt.find("%f", pos)) != std::string::npos) {
            uint32_t micros = fractionalNanoseconds / 1000;
            char buf[7];
            std::snprintf(buf, sizeof(buf), "%06u", micros);
            processedFmt.replace(pos, 2, buf);
            pos += 6;
        }

        // %z - timezone offset (+HHMM or -HHMM)
        pos = 0;
        while ((pos = processedFmt.find("%z", pos)) != std::string::npos) {
            int offsetSec = tzOffsetSeconds;
            char sign = (offsetSec >= 0) ? '+' : '-';
            offsetSec = std::abs(offsetSec);
            int hrs = offsetSec / 3600;
            int mins = (offsetSec % 3600) / 60;
            char buf[6];
            std::snprintf(buf, sizeof(buf), "%c%02d%02d", sign, hrs, mins);
            processedFmt.replace(pos, 2, buf);
            pos += 5;
        }

        // %Z - timezone name (use "UTC" if isUTC, otherwise offset string)
        pos = 0;
        while ((pos = processedFmt.find("%Z", pos)) != std::string::npos) {
            std::string tzName = isUTC ? "UTC" : "GMT";
            if (!isUTC && tzOffsetSeconds != 0) {
                int offsetSec = tzOffsetSeconds;
                char sign = (offsetSec >= 0) ? '+' : '-';
                offsetSec = std::abs(offsetSec);
                int hrs = offsetSec / 3600;
                int mins = (offsetSec % 3600) / 60;
                char buf[10];
                std::snprintf(buf, sizeof(buf), "GMT%c%02d:%02d", sign, hrs, mins);
                tzName = buf;
            }
            processedFmt.replace(pos, 2, tzName);
            pos += tzName.length();
        }

        // Standard strftime
        char buffer[512];
        size_t len = std::strftime(buffer, sizeof(buffer), processedFmt.c_str(), &timeStruct);

        if (len == 0 && !processedFmt.empty()) {
            // Buffer too small or invalid format
            return fmt;  // Return original format as fallback
        }

        return std::string(buffer, len);
    }

    // Update literalText to reflect current calendar fields
    void update_literal_text() {
        std::ostringstream oss;
        oss << std::setfill('0');
        oss << std::setw(4) << year << "-"
            << std::setw(2) << month << "-"
            << std::setw(2) << day << "T"
            << std::setw(2) << hour << ":"
            << std::setw(2) << minute << ":"
            << std::setw(2) << second;

        // Add fractional seconds if non-zero
        if (fractionalNanoseconds > 0) {
            // Determine precision from original precision or compute from nanoseconds
            uint32_t micros = fractionalNanoseconds / 1000;
            uint32_t millis = micros / 1000;

            if (precision == DateTimePrecision::MILLISECOND) {
                oss << "." << std::setw(3) << millis;
            } else if (precision == DateTimePrecision::MICROSECOND) {
                oss << "." << std::setw(6) << micros;
            } else if (precision == DateTimePrecision::NANOSECOND) {
                oss << "." << std::setw(9) << fractionalNanoseconds;
            }
        }

        // Add timezone
        if (isUTC) {
            oss << "Z";
        } else if (tzOffsetSeconds != 0) {
            int offsetSec = tzOffsetSeconds;
            char sign = (offsetSec >= 0) ? '+' : '-';
            offsetSec = std::abs(offsetSec);
            int hrs = offsetSec / 3600;
            int mins = (offsetSec % 3600) / 60;
            oss << sign << std::setw(2) << hrs << ":" << std::setw(2) << mins;
        }

        literalText = oss.str();
    }

    // Add days (calendar arithmetic - handles month/year boundaries)
    std::shared_ptr<DateTimeValue> addDays(int days) const {
        auto newDt = std::make_shared<DateTimeValue>(*this);

        // Simple approach: convert days to seconds and add to epoch
        int64_t secondsToAdd = static_cast<int64_t>(days) * 86400LL;
        int64_t nanosToAdd = secondsToAdd * 1'000'000'000LL;

        if (nanosToAdd >= 0) {
            newDt->epochNanoseconds += static_cast<uint64_t>(nanosToAdd);
        } else {
            uint64_t absNanos = static_cast<uint64_t>(-nanosToAdd);
            if (newDt->epochNanoseconds >= absNanos) {
                newDt->epochNanoseconds -= absNanos;
            } else {
                newDt->epochNanoseconds = 0;
            }
        }

        newDt->recompute_calendar_fields();
        newDt->update_literal_text();
        return newDt;
    }

    // Add months (calendar arithmetic - handles year boundaries and varying month lengths)
    std::shared_ptr<DateTimeValue> addMonths(int months) const {
        auto newDt = std::make_shared<DateTimeValue>(*this);

        int totalMonths = newDt->month + months;
        int yearAdjust = 0;

        // Handle positive overflow
        while (totalMonths > 12) {
            totalMonths -= 12;
            yearAdjust++;
        }

        // Handle negative overflow
        while (totalMonths < 1) {
            totalMonths += 12;
            yearAdjust--;
        }

        newDt->year += yearAdjust;
        newDt->month = totalMonths;

        // Clamp day to valid range for new month (e.g., Jan 31 + 1 month = Feb 28/29)
        while (!is_valid_date(newDt->year, newDt->month, newDt->day)) {
            newDt->day--;
            if (newDt->day < 1) {
                throw std::runtime_error("Date calculation error in addMonths");
            }
        }

        newDt->recompute_epoch_from_fields();
        newDt->update_literal_text();
        return newDt;
    }

    // Add years (calendar arithmetic)
    std::shared_ptr<DateTimeValue> addYears(int years) const {
        auto newDt = std::make_shared<DateTimeValue>(*this);
        newDt->year += years;

        // Handle Feb 29 on leap year -> non-leap year
        if (newDt->month == 2 && newDt->day == 29) {
            bool is_leap = (newDt->year % 4 == 0 && newDt->year % 100 != 0) ||
                (newDt->year % 400 == 0);
            if (!is_leap) {
                newDt->day = 28;  // Clamp to Feb 28
            }
        }

        newDt->recompute_epoch_from_fields();
        newDt->update_literal_text();
        return newDt;
    }

    // Add hours (instant arithmetic)
    std::shared_ptr<DateTimeValue> addHours(double hours) const {
        return addSeconds(hours * 3600.0);
    }

    // Add minutes (instant arithmetic)
    std::shared_ptr<DateTimeValue> addMinutes(double minutes) const {
        return addSeconds(minutes * 60.0);
    }

    // Add seconds (instant arithmetic)
    std::shared_ptr<DateTimeValue> addSeconds(double seconds) const {
        auto newDt = std::make_shared<DateTimeValue>(*this);

        int64_t nanosToAdd = static_cast<int64_t>(seconds * 1'000'000'000.0);

        if (nanosToAdd >= 0) {
            newDt->epochNanoseconds += static_cast<uint64_t>(nanosToAdd);
        } else {
            uint64_t absNanos = static_cast<uint64_t>(-nanosToAdd);
            if (newDt->epochNanoseconds >= absNanos) {
                newDt->epochNanoseconds -= absNanos;
            } else {
                newDt->epochNanoseconds = 0;
            }
        }

        newDt->recompute_calendar_fields();
        newDt->update_literal_text();
        return newDt;
    }

    // Add milliseconds (instant arithmetic)
    std::shared_ptr<DateTimeValue> addMillis(double millis) const {
        auto newDt = std::make_shared<DateTimeValue>(*this);

        int64_t nanosToAdd = static_cast<int64_t>(millis * 1'000'000.0);

        if (nanosToAdd >= 0) {
            newDt->epochNanoseconds += static_cast<uint64_t>(nanosToAdd);
        } else {
            uint64_t absNanos = static_cast<uint64_t>(-nanosToAdd);
            if (newDt->epochNanoseconds >= absNanos) {
                newDt->epochNanoseconds -= absNanos;
            } else {
                newDt->epochNanoseconds = 0;
            }
        }

        newDt->recompute_calendar_fields();
        newDt->update_literal_text();
        return newDt;
    }

    // Subtract methods (just negate and add)
    std::shared_ptr<DateTimeValue> subtractDays(int days) const {
        return addDays(-days);
    }

    std::shared_ptr<DateTimeValue> subtractMonths(int months) const {
        return addMonths(-months);
    }

    std::shared_ptr<DateTimeValue> subtractYears(int years) const {
        return addYears(-years);
    }

    std::shared_ptr<DateTimeValue> subtractHours(double hours) const {
        return addHours(-hours);
    }

    std::shared_ptr<DateTimeValue> subtractMinutes(double minutes) const {
        return addMinutes(-minutes);
    }

    std::shared_ptr<DateTimeValue> subtractSeconds(double seconds) const {
        return addSeconds(-seconds);
    }

    std::shared_ptr<DateTimeValue> subtractMillis(double millis) const {
        return addMillis(-millis);
    }

    // Set timezone (returns new DateTime with same instant, different zone)
    std::shared_ptr<DateTimeValue> setZone(const std::string& zone) const {
        auto newDt = std::make_shared<DateTimeValue>(*this);

        // Parse zone string
        if (zone == "UTC" || zone == "Z") {
            newDt->isUTC = true;
            newDt->tzOffsetSeconds = 0;
        } else if (zone.length() >= 3 && (zone[0] == '+' || zone[0] == '-')) {
            // Parse offset: +HH:MM, +HHMM, or +HH
            char sign = zone[0];
            std::string offset_str = zone.substr(1);

            int hours = 0;
            int minutes = 0;

            // Remove colons
            size_t colon_pos = offset_str.find(':');
            if (colon_pos != std::string::npos) {
                offset_str.erase(colon_pos, 1);
            }

            // Parse hours and minutes
            if (offset_str.length() >= 2) {
                hours = std::stoi(offset_str.substr(0, 2));
            }
            if (offset_str.length() >= 4) {
                minutes = std::stoi(offset_str.substr(2, 2));
            }

            // Validate
            if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
                throw std::runtime_error("Invalid timezone offset: hours must be 0-23, minutes 0-59");
            }

            newDt->tzOffsetSeconds = (hours * 3600 + minutes * 60);
            if (sign == '-') {
                newDt->tzOffsetSeconds = -newDt->tzOffsetSeconds;
            }
            newDt->isUTC = (newDt->tzOffsetSeconds == 0);
        } else {
            throw std::runtime_error("Invalid timezone format. Use 'UTC', '+HH:MM', '+HHMM', or '+HH'");
        }

        // Recompute calendar fields for new timezone
        newDt->recompute_calendar_fields();
        newDt->update_literal_text();
        return newDt;
    }
};

struct PropertyDescriptor {
    Value value;
    bool is_private = false;
    bool is_readonly = false;
    bool is_locked = false;
    Token token;
};

struct ObjectValue {
    std::unordered_map<std::string,
        PropertyDescriptor>
        properties;
    bool is_frozen = false;

    // When true this ObjectValue is a proxy for an Environment (live view).
    // Reads/writes/enumeration should forward to `proxy_env->values`.
    // This is used by the builtin globals() to expose a live global/module env.
    bool is_env_proxy = false;
    EnvPtr proxy_env = nullptr;
};

struct PromiseValue {
    enum class State { PENDING,
        FULFILLED,
        REJECTED };
    State state = State::PENDING;
    Value result;  // fulfilled value or rejection reason

    // Continuations to run when resolved
    std::vector<std::function<void(Value)>> then_callbacks;
    std::vector<std::function<void(Value)>> catch_callbacks;

    // NEW: whether this promise has an attached handler (then/catch). Used for unhandled rejection detection.
    bool handled = false;

    // (optional) you can add a small marker if you want to avoid printing multiple times
    bool unhandled_reported = false;

    // NEW: ensure we only schedule the "unhandled check" microtask once per rejection
    bool unhandled_check_scheduled = false;

    // NEW: parent link for chained promises. When a promise A is created by calling B.then(...),
    // set A->parent = B so we can walk ancestors and mark them handled when a downstream handler
    // is attached.
    std::weak_ptr<PromiseValue> parent;
};

struct GeneratorValue {
    enum class State { SuspendedStart,
        SuspendedYield,
        Executing,
        Completed };
    CallFramePtr frame;
    State state = State::SuspendedStart;
    bool is_done = false;
};

struct ArrayValue {
    std::vector<Value> elements;
};

// Function value: closure with parameters, body, and defining environment
struct FunctionValue {
    std::string name;
    std::vector<std::shared_ptr<ParameterNode>> parameters;
    std::shared_ptr<FunctionDeclarationNode> body;
    EnvPtr closure;
    Token token;
    bool is_async = false;
    bool is_generator = false;
    bool is_native = false;
    std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> native_impl;

    FunctionValue(
        const std::string& nm,
        const std::vector<std::unique_ptr<ParameterNode>>& params,
        const std::shared_ptr<FunctionDeclarationNode>& b,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            parameters(),  // default-initialize parameters, we'll fill below
                            body(b),
                            closure(env),
                            token(tok),
                            is_async(b ? b->is_async : false),
                            is_generator(b ? b->is_generator : false),
                            is_native(false) {
        parameters.reserve(params.size());
        for (const auto& p : params) {
            if (p) {
                auto cloned = p->clone();
                parameters.emplace_back(std::shared_ptr<ParameterNode>(cloned.release()));
            } else {
                parameters.emplace_back(nullptr);
            }
        }
    }

    FunctionValue(
        const std::string& nm,
        const std::vector<std::shared_ptr<ParameterNode>>& params,
        const std::shared_ptr<FunctionDeclarationNode>& b,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            parameters(params),
                            body(b),
                            closure(env),
                            token(tok),
                            is_async(b ? b->is_async : false),
                            is_generator(b ? b->is_generator : false),
                            is_native(false) {
    }

    FunctionValue(
        const std::string& nm,
        std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl,
        const EnvPtr& env,
        const Token& tok) : name(nm),
                            parameters(),
                            body(nullptr),
                            closure(env),
                            token(tok),
                            is_async(false),
                            is_generator(false),
                            is_native(true),
                            native_impl(std::move(impl)) {
    }
};

// Environment with lexical parent pointer
class Environment : public std::enable_shared_from_this<Environment> {
   public:
    Environment(EnvPtr parent = nullptr) : parent(parent) {
    }

    struct Variable {
        Value value;
        bool is_constant = false;
    };

    // map from name -> Variable
    std::unordered_map<std::string,
        Variable>
        values;
    EnvPtr parent;

    // check if name exists in this environment or any parent
    bool has(const std::string& name) const;

    // get reference to variable (searches up the chain). Throws if not found.
    Variable& get(const std::string& name);

    // set variable in the current environment (creates or replaces)
    void set(const std::string& name, const Variable& var);
};

struct LoopControl {
    bool did_break = false;
    bool did_continue = false;
};

struct SuspendExecution : public std::exception {
    // Exception used to short-circuit evaluation when an async await suspends the current frame.
    // It's not an error — the executor will catch it, keep the frame on the stack and return.
    const char* what() const noexcept override { return "Execution suspended for await"; }
};
// Exception thrown to indicate a generator `yield` — carries the yielded value.
struct GeneratorYield : public std::exception {
    Value value;
    explicit GeneratorYield(const Value& v) : value(v) {}
    const char* what() const noexcept override { return "Generator yielded"; }
};

struct GeneratorReturn : public std::exception {
    Value value;
    explicit GeneratorReturn(const Value& v) : value(v) {}
    const char* what() const noexcept override { return "Generator return/close"; }
};

// Evaluator
class Evaluator {
   public:
    Evaluator();
    ~Evaluator();
    // Evaluate whole program (caller must ensure ProgramNode lifetime covers evaluation)
    void evaluate(ProgramNode* program);

    // For RELP print on the go.
    Value evaluate_expression(ExpressionNode* expr);
    std::string value_to_string(const Value& v);
    bool is_void(const Value& v);
    static std::string cerr_colored(const std::string& s);

    void set_entry_point(const std::string& filename);
    void set_cli_args(const std::vector<std::string>& args);

    // Accessor for the scheduler (non-owning for now).
    Scheduler* scheduler() { return scheduler_.get(); }

    // Public wrapper that lets native builtins synchronously invoke interpreter-callable functions.
    // This is a thin public forwarder to the private call_function implementation.
    Value invoke_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken);

    void fulfill_promise(PromisePtr p, const Value& value);
    void reject_promise(PromisePtr p, const Value& reason);
    void report_unhandled_rejection(PromisePtr p);

    // NEW: Mark promise and all ancestors as handled (walk parent links).
    void mark_promise_and_ancestors_handled(PromisePtr p);

   private:
    EnvPtr global_env;
    EnvPtr main_module_env;
    EnvPtr repl_env;

    std::vector<std::string> cli_args;

    ClassPtr current_class_context = nullptr;

    // Scheduler instance used to host microtasks/macrotasks and future frame continuations.
    // Initialized in constructor (Phase 0). Using unique_ptr to avoid problems with header inclusion order.
    std::unique_ptr<Scheduler> scheduler_;
    std::vector<CallFramePtr> call_stack_;

    std::vector<CallFramePtr> suspended_frames_;

    // call frame helpers
    void push_frame(CallFramePtr f);
    void pop_frame();
    CallFramePtr current_frame();
    void execute_frame_until_await_or_return(CallFramePtr frame, PromisePtr promise);
    void execute_frame_until_return(CallFramePtr frame);

    void add_suspended_frame(CallFramePtr f);
    void remove_suspended_frame(CallFramePtr f);

    void populate_module_metadata(EnvPtr env, const std::string& resolved_path, const std::string& module_name, bool is_main);

    // Module loader records for caching and circular dependency handling.
    struct ModuleRecord {
        enum class State {
            Loading,
            Loaded
        };
        State state = State::Loading;
        ObjectPtr exports = nullptr;  // object holding exported properties
        EnvPtr module_env = nullptr;  // environment used while evaluating module
        std::string path;             // canonical filesystem path used as cache key
    };

    // map canonical module path -> ModuleRecord (shared_ptr)
    std::unordered_map<std::string,
        std::shared_ptr<ModuleRecord>>
        module_cache;

    // Import API: load module by specifier (relative path like "./file" or "./dir/file.sl"),
    // returns the module's exports object (ObjectPtr). 'requesterTok' is used to resolve
    // relative paths with respect to the importing file; for REPL/unknown filename use cwd.
    ObjectPtr import_module(const std::string& module_spec, const Token& requesterTok, EnvPtr requesterEnv);

    // Helper: resolve a module specifier to an existing filesystem path (tries spec, then .sl/.swz)
    std::string resolve_module_path(const std::string& module_spec, const std::string& requester_filename, const Token& tok);

    // Expression & statement evaluators. Pass the environment explicitly for lexical scoping.
    Value evaluate_expression(ExpressionNode* expr, EnvPtr env);
    Value call_function(FunctionPtr fn, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken);
    Value call_function_with_receiver(FunctionPtr fn, ObjectPtr receiver, const std::vector<Value>& args, EnvPtr caller_env, const Token& callToken);
    void evaluate_statement(StatementNode* stmt, EnvPtr env, Value* return_value = nullptr, bool* did_return = nullptr, LoopControl* lc = nullptr);

    // helpers: conversions and formatting
    std::string type_name(const Value& v);
    double to_number(const Value& v, Token token = {});
    std::string to_string_value(const Value& v, bool no_color = false);
    bool to_bool(const Value& v);
    void bind_pattern_to_value(ExpressionNode* pattern, const Value& value, EnvPtr env, bool is_constant, const Token& declToken);

    // value equality helper (deep for arrays, tolerant for mixed number/string)
    bool is_equal(const Value& a, const Value& b);
    // strict equality: no coercion — types must match and values compared directly
    bool is_strict_equal(const Value& a, const Value& b);

    inline bool is_nullish(const Value& v) const {
        return std::holds_alternative<std::monostate>(v);
    }

    Value get_object_property(ObjectPtr obj, const std::string& key, EnvPtr env, const Token& accessToken);
    void set_object_property(ObjectPtr obj, const std::string& key, const Value& val, EnvPtr env, const Token& assignToken);

    std::string print_value(
        const Value& v,
        int depth = 0,
        std::unordered_set<const ObjectValue*> visited = {},
        std::unordered_set<const ArrayValue*> arrvisited = {});

    std::string print_object(
        ObjectPtr obj,
        int indent = 0,
        std::unordered_set<const ObjectValue*> visited = {});

    bool is_private_access_allowed(ObjectPtr obj, EnvPtr env);

    void run_event_loop();
    void schedule_callback(FunctionPtr cb, const std::vector<Value>& args);

    Value resume_generator(GeneratorPtr gen, const Value& arg, bool is_return, bool is_throw, bool& done);
    void execute_frame_until_yield_or_return(CallFramePtr frame, Value* out_yielded_value, bool* did_return, Value* return_value);
};

TokenLocation build_location_from_value(const Value& v, const TokenLocation& defaultLoc);
std::string _type_name(const Value&);