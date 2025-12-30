#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"
#include "uv.h"

#define _POSIX_C_SOURCE 200809L
#include <fnmatch.h>

// Platform-specific glob support
#if defined(__unix__) || defined(__APPLE__) || defined(__ANDROID__)
#include <glob.h>
#define HAVE_POSIX_GLOB 1
#endif

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#else
#include <processthreadsapi.h>
#include <shlwapi.h>  // For PathMatchSpec on Windows
#include <windows.h>
#pragma comment(lib, "shlwapi.lib")
#endif

namespace fs = std::filesystem;

// Cross-platform glob pattern matching with proper ** support
static bool matches_pattern(const std::string& text, const std::string& pattern) {
    // Handle special case: ** (matches everything recursively)
    if (pattern == "**" || pattern == "**/*") {
        return true;
    }

    // Check if pattern contains **/ (recursive match)
    size_t double_star = pattern.find("**/");
    if (double_star != std::string::npos) {
        // Pattern like "node_modules/**" or "**/test/**"
        std::string prefix = pattern.substr(0, double_star);
        std::string suffix = pattern.substr(double_star + 3);  // Skip "**/

        // Check if text starts with prefix
        if (!prefix.empty() && text.find(prefix) != 0) {
            return false;
        }

        // If there's a suffix, check if any part of remaining path matches
        if (!suffix.empty()) {
            size_t start = prefix.length();
            while (start < text.length()) {
                std::string remaining = text.substr(start);
                if (matches_pattern(remaining, suffix)) {
                    return true;
                }
                // Move to next directory separator
                size_t next_sep = text.find_first_of("/\\", start + 1);
                if (next_sep == std::string::npos) break;
                start = next_sep + 1;
            }
            return false;
        }
        return true;  // Pattern ends with **, matches everything after prefix
    }

#if HAVE_POSIX_GLOB
    // Use fnmatch on POSIX systems
    return fnmatch(pattern.c_str(), text.c_str(), FNM_PERIOD | FNM_PATHNAME) == 0;
#elif defined(_WIN32)
    // Use PathMatchSpec on Windows
    return PathMatchSpecA(text.c_str(), pattern.c_str()) == TRUE;
#else
    // Fallback: simple wildcard matching
    size_t p = 0, t = 0;
    size_t star = std::string::npos, match = 0;

    while (t < text.length()) {
        if (p < pattern.length() && (pattern[p] == '?' || pattern[p] == text[t])) {
            p++;
            t++;
        } else if (p < pattern.length() && pattern[p] == '*') {
            star = p++;
            match = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
        } else {
            return false;
        }
    }

    while (p < pattern.length() && pattern[p] == '*') p++;
    return p == pattern.length();
#endif
}
// ============= FS.WATCH IMPLEMENTATION (libuv-based) =============

// Track active watchers for event loop integration
static std::atomic<int> g_active_fs_watchers{0};

// Watcher state
struct FsWatcher {
    uv_fs_event_t* handle = nullptr;
    std::string path;
    bool recursive;
    FunctionPtr callback;
    std::atomic<bool> closed{false};
    long long id;

    // Debouncing support
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_event_times;
    std::mutex debounce_mutex;
    int debounce_ms = 100;  // default 100ms debounce

    // Filter support
    std::vector<std::string> ignore_patterns;
    std::vector<std::string> include_patterns;
    bool has_include_filter = false;

    std::atomic<bool> paused{false};

    bool should_emit_event(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(debounce_mutex);
        auto now = std::chrono::steady_clock::now();
        auto it = last_event_times.find(filepath);

        if (it != last_event_times.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed < debounce_ms) {
                return false;  // Too soon, skip this event
            }
        }

        last_event_times[filepath] = now;
        return true;
    }

    bool should_watch_file(const std::string& filepath) {
        // Get relative path from the watch root
        std::string relative_path = filepath;
        if (filepath.find(path) == 0 && filepath.length() > path.length()) {
            relative_path = filepath.substr(path.length());
            // Remove leading slash
            while (!relative_path.empty() && (relative_path[0] == '/' || relative_path[0] == '\\')) {
                relative_path = relative_path.substr(1);
            }
        }

        // Also get just the filename
        std::string filename = fs::path(filepath).filename().string();

        // If include patterns exist, file must match at least one
        if (has_include_filter && !include_patterns.empty()) {
            bool matches = false;
            for (const auto& pattern : include_patterns) {
                // Try relative path first (for patterns like "src/**/*.js")
                // Then try filename only (for patterns like "*.js")
                if (matches_pattern(relative_path, pattern) ||
                    matches_pattern(filename, pattern)) {
                    matches = true;
                    break;
                }
            }
            if (!matches) return false;
        }

        // Check ignore patterns (same priority: relative path, then filename)
        for (const auto& pattern : ignore_patterns) {
            if (matches_pattern(relative_path, pattern) ||
                matches_pattern(filename, pattern)) {
                return false;
            }
        }

        return true;
    }
    ~FsWatcher() {
        if (handle && !closed.load()) {
            closed.store(true);
            uv_fs_event_stop(handle);
            // Don't close here - let the close callback handle cleanup
        }
    }
};

static std::mutex g_fs_watchers_mutex;
static std::unordered_map<long long, std::shared_ptr<FsWatcher>> g_fs_watchers;
static std::atomic<long long> g_next_watcher_id{1};

// Callback invoked by libuv when file system events occur
static void fs_event_callback(uv_fs_event_t* handle, const char* filename, int events, int status) {
    FsWatcher* watcher = static_cast<FsWatcher*>(handle->data);
    if (!watcher || watcher->closed.load() || !watcher->callback) {
        return;
    }

    if (watcher->paused.load()) {
        return;  // Skip event while paused
    }

    if (status != 0) {
        return;
    }

    // Build full path FIRST
    std::string full_path = watcher->path;
    if (filename && filename[0] != '\0') {
        full_path = (fs::path(watcher->path) / filename).string();
    }

    // Debounce check
    if (!watcher->should_emit_event(full_path)) {
        return;  // Skip duplicate event
    }

    // Filter check
    if (!watcher->should_watch_file(full_path)) {
        return;  // Filtered out
    }

    // Determine event type with better heuristics
    std::string event_type;
    bool file_exists = fs::exists(full_path);

    if (events & UV_RENAME) {
        // UV_RENAME can mean: created, deleted, or renamed
        if (file_exists) {
            event_type = "add";  // File was created or renamed TO this name
        } else {
            event_type = "unlink";  // File was deleted or renamed FROM this name
        }
    } else if (events & UV_CHANGE) {
        event_type = "change";  // File content changed
    } else {
        event_type = "change";  // Fallback
    }

    // Build event object
    auto event = std::make_shared<ObjectValue>();
    Token tok;
    tok.loc = TokenLocation("<fs>", 0, 0, 0);

    event->properties["type"] = PropertyDescriptor{Value{event_type}, false, false, true, tok};
    event->properties["path"] = PropertyDescriptor{Value{full_path}, false, false, true, tok};

    if (filename && filename[0] != '\0') {
        event->properties["name"] = PropertyDescriptor{Value{std::string(filename)}, false, false, true, tok};
    } else {
        event->properties["name"] = PropertyDescriptor{Value{std::monostate{}}, false, false, true, tok};
    }

    // Schedule callback on event loop
    CallbackPayload* payload = new CallbackPayload(watcher->callback, {Value{event}});
    enqueue_callback_global(static_cast<void*>(payload));
}

// Helper to coerce Value -> string
static std::string value_to_string_simple(const Value& v) {
    if (std::holds_alternative<std::string>(v)) return std::get<std::string>(v);
    if (std::holds_alternative<double>(v)) {
        std::ostringstream ss;
        ss << std::get<double>(v);
        return ss.str();
    }
    if (std::holds_alternative<bool>(v)) return std::get<bool>(v) ? "kweli" : "sikweli";
    return std::string();
}

// Helper: create a native FunctionValue from a lambda
template <typename F>
static FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    auto fn = std::make_shared<FunctionValue>(name, native_impl, env, Token());
    return fn;
}

// Helper function to build stat object from path
// use_lstat: if true, use symlink_status (don't follow symlinks)
static Value build_stat_object(const std::string& path, bool use_lstat, const Token& token) {
    auto obj = std::make_shared<ObjectValue>();

    try {
        if (!std::filesystem::exists(path)) {
            obj->properties["exists"] = PropertyDescriptor{Value{false}, false, false, true, Token()};
            return Value{obj};
        }

        // Get filesystem status (follow or don't follow symlinks)
        auto status = use_lstat ? std::filesystem::symlink_status(path) : std::filesystem::status(path);
        bool isFile = std::filesystem::is_regular_file(status);
        bool isDir = std::filesystem::is_directory(status);
        bool isSymlink = std::filesystem::is_symlink(path);

        // Unix-specific: detect sockets, FIFOs, devices
        bool isSocket = false;
        bool isFifo = false;
        bool isBlockDevice = false;
        bool isCharDevice = false;

        uintmax_t size = 0;
        if (isFile) {
            size = std::filesystem::file_size(path);
        }

        // Get timestamps
        std::string mtime;
        std::string ctime;
        std::string atime;

        // Modified time (cross-platform)
        try {
            auto ftime = std::filesystem::last_write_time(path);
            auto st = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now());
            std::time_t tt = std::chrono::system_clock::to_time_t(st);
            std::tm tm{};
#ifdef _MSC_VER
            gmtime_s(&tm, &tt);
#else
            gmtime_r(&tt, &tm);
#endif
            char buf[64];
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            mtime = buf;
        } catch (...) {
            mtime = "";
        }

#ifndef _WIN32
        // Unix: use stat() to get all timestamps + file type details
        struct stat st;
        bool stat_success = use_lstat ? (lstat(path.c_str(), &st) == 0) : (stat(path.c_str(), &st) == 0);

        if (stat_success) {
            // Detect special file types
            isSocket = S_ISSOCK(st.st_mode);
            isFifo = S_ISFIFO(st.st_mode);
            isBlockDevice = S_ISBLK(st.st_mode);
            isCharDevice = S_ISCHR(st.st_mode);

            std::tm tm{};
            char buf[64];

            // Access time
#ifdef __APPLE__
            std::time_t at = st.st_atimespec.tv_sec;
#else
            std::time_t at = st.st_atime;
#endif
            gmtime_r(&at, &tm);
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            atime = buf;

            // Birth time (creation)
#ifdef __APPLE__
            std::time_t bt = st.st_birthtimespec.tv_sec;
            gmtime_r(&bt, &tm);
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            ctime = buf;
#elif defined(__linux__)
            std::time_t ct = st.st_ctime;
            gmtime_r(&ct, &tm);
            std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
            ctime = buf;
#else
            ctime = "";
#endif
        }
#else
        // Windows: use GetFileTime
        HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            FILETIME ftCreate, ftAccess, ftWrite;
            if (GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
                auto convert_filetime = [](const FILETIME& ft) -> std::string {
                    ULARGE_INTEGER uli;
                    uli.LowPart = ft.dwLowDateTime;
                    uli.HighPart = ft.dwHighDateTime;

                    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
                    uint64_t timestamp = (uli.QuadPart - EPOCH_DIFF) / 10000000ULL;

                    std::time_t tt = static_cast<std::time_t>(timestamp);
                    std::tm tm{};
                    gmtime_s(&tm, &tt);
                    char buf[64];
                    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                    return buf;
                };

                ctime = convert_filetime(ftCreate);
                atime = convert_filetime(ftAccess);
            }
            CloseHandle(hFile);
        }
#endif

        // Permissions
        auto perms = status.permissions();
        uint32_t perms_raw = static_cast<uint32_t>(perms);

        std::string perms_summary;
        perms_summary += ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) ? "r" : "-";
        perms_summary += ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none) ? "w" : "-";
        perms_summary += ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? "x" : "-";
        perms_summary += ((perms & std::filesystem::perms::group_read) != std::filesystem::perms::none) ? "r" : "-";
        perms_summary += ((perms & std::filesystem::perms::group_write) != std::filesystem::perms::none) ? "w" : "-";
        perms_summary += ((perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none) ? "x" : "-";
        perms_summary += ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none) ? "r" : "-";
        perms_summary += ((perms & std::filesystem::perms::others_write) != std::filesystem::perms::none) ? "w" : "-";
        perms_summary += ((perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none) ? "x" : "-";

        // Determine file type string
        std::string type_str;
        if (isFile)
            type_str = "file";
        else if (isDir)
            type_str = "directory";
        else if (isSymlink)
            type_str = "symlink";
        else if (isSocket)
            type_str = "socket";
        else if (isFifo)
            type_str = "fifo";
        else if (isBlockDevice)
            type_str = "block-device";
        else if (isCharDevice)
            type_str = "char-device";
        else
            type_str = "other";

        // Build standard properties
        obj->properties["exists"] = PropertyDescriptor{Value{true}, false, false, true, Token()};
        obj->properties["type"] = PropertyDescriptor{Value{type_str}, false, false, true, Token()};
        obj->properties["size"] = PropertyDescriptor{Value{static_cast<double>(size)}, false, false, true, Token()};

        obj->properties["mtime"] = PropertyDescriptor{Value{mtime}, false, false, true, Token()};
        obj->properties["ctime"] = PropertyDescriptor{Value{ctime}, false, false, true, Token()};
        obj->properties["atime"] = PropertyDescriptor{Value{atime}, false, false, true, Token()};

        obj->properties["permissions"] = PropertyDescriptor{Value{perms_summary}, false, false, true, Token()};
        obj->properties["mode"] = PropertyDescriptor{Value{static_cast<double>(perms_raw)}, false, false, true, Token()};

        // Deprecated boolean fields (for compatibility)
        obj->properties["isFile"] = PropertyDescriptor{Value{isFile}, false, false, true, Token()};
        obj->properties["isDir"] = PropertyDescriptor{Value{isDir}, false, false, true, Token()};
        obj->properties["isSymlink"] = PropertyDescriptor{Value{isSymlink}, false, false, true, Token()};

        // New boolean fields for special types
        obj->properties["isSocket"] = PropertyDescriptor{Value{isSocket}, false, false, true, Token()};
        obj->properties["isFifo"] = PropertyDescriptor{Value{isFifo}, false, false, true, Token()};
        obj->properties["isBlockDevice"] = PropertyDescriptor{Value{isBlockDevice}, false, false, true, Token()};
        obj->properties["isCharDevice"] = PropertyDescriptor{Value{isCharDevice}, false, false, true, Token()};

#ifndef _WIN32
        // Unix: raw platform data
        if (stat_success) {
            auto raw_obj = std::make_shared<ObjectValue>();
            raw_obj->properties["dev"] = PropertyDescriptor{Value{static_cast<double>(st.st_dev)}, false, false, true, Token()};
            raw_obj->properties["ino"] = PropertyDescriptor{Value{static_cast<double>(st.st_ino)}, false, false, true, Token()};
            raw_obj->properties["nlink"] = PropertyDescriptor{Value{static_cast<double>(st.st_nlink)}, false, false, true, Token()};
            raw_obj->properties["uid"] = PropertyDescriptor{Value{static_cast<double>(st.st_uid)}, false, false, true, Token()};
            raw_obj->properties["gid"] = PropertyDescriptor{Value{static_cast<double>(st.st_gid)}, false, false, true, Token()};
            raw_obj->properties["rdev"] = PropertyDescriptor{Value{static_cast<double>(st.st_rdev)}, false, false, true, Token()};
            raw_obj->properties["blksize"] = PropertyDescriptor{Value{static_cast<double>(st.st_blksize)}, false, false, true, Token()};
            raw_obj->properties["blocks"] = PropertyDescriptor{Value{static_cast<double>(st.st_blocks)}, false, false, true, Token()};

            obj->properties["raw"] = PropertyDescriptor{Value{raw_obj}, false, false, true, Token()};
        }
#else
        // Windows: file attributes
        DWORD attrs = GetFileAttributesA(path.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            auto raw_obj = std::make_shared<ObjectValue>();
            raw_obj->properties["attributes"] = PropertyDescriptor{Value{static_cast<double>(attrs)}, false, false, true, Token()};
            raw_obj->properties["hidden"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_HIDDEN) != 0}, false, false, true, Token()};
            raw_obj->properties["system"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_SYSTEM) != 0}, false, false, true, Token()};
            raw_obj->properties["archive"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_ARCHIVE) != 0}, false, false, true, Token()};
            raw_obj->properties["readonly"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_READONLY) != 0}, false, false, true, Token()};
            raw_obj->properties["compressed"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_COMPRESSED) != 0}, false, false, true, Token()};
            raw_obj->properties["encrypted"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_ENCRYPTED) != 0}, false, false, true, Token()};

            obj->properties["raw"] = PropertyDescriptor{Value{raw_obj}, false, false, true, Token()};
        }
#endif

        return Value{obj};
    } catch (const std::filesystem::filesystem_error& e) {
        throw SwaziError("FilesystemError", std::string("fs.stat failed: ") + e.what(), token.loc);
    }
}

// ============= MAIN EXPORTS FUNCTION =============

std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();

    // fs.readFile(path, options?) -> string | Buffer
    // options can be: { encoding: "utf8" | "binary" | null, flag: "r" }
    // If encoding is null or "binary", returns Buffer. Otherwise returns string.
    {
        auto fn = make_native_fn("fs.readFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.readFile requires a path as an argument. Usage: readFile(path, options?) -> string | Buffer", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            
            // Parse options
            std::string encoding = "utf8";  // default
            if (args.size() >= 2) {
                if (std::holds_alternative<std::string>(args[1])) {
                    // Simple string encoding
                    encoding = std::get<std::string>(args[1]);
                } else if (std::holds_alternative<ObjectPtr>(args[1])) {
                    // Options object
                    ObjectPtr opts = std::get<ObjectPtr>(args[1]);
                    auto enc_it = opts->properties.find("encoding");
                    if (enc_it != opts->properties.end()) {
                        if (std::holds_alternative<std::monostate>(enc_it->second.value)) {
                            encoding = "binary";  // null means binary
                        } else if (std::holds_alternative<std::string>(enc_it->second.value)) {
                            encoding = std::get<std::string>(enc_it->second.value);
                        }
                    }
                }
            }
            
            // Read file in binary mode always
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open()) {
                std::string reason = std::strerror(errno);
                throw SwaziError("IOError", "Failed to open file: " + path + " — " + reason, token.loc);
            }
            
            // Read all bytes
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
            
            // Return based on encoding
            if (encoding == "binary" || encoding == "null") {
                auto buf = std::make_shared<BufferValue>();
                buf->data = std::move(data);
                buf->encoding = "binary";
                return Value{buf};
            } else {
                // Convert to string (utf8 or other encoding)
                return Value{std::string(data.begin(), data.end())};
            } }, env);
        obj->properties["readFile"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // fs.writeFile(path, content, options?) -> bool
    // content can be string or Buffer
    // options can be: { encoding: "utf8", flag: "w", mode: 0o666 }
    // If content is Buffer, writes in binary mode automatically
    {
        auto fn = make_native_fn("fs.writeFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.writeFile requires two arguments: path and content, and an optional options object. Usage: fs.writeFile(path, content, options?) -> bool", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            
            // Determine if we're writing buffer or string
            bool is_buffer = std::holds_alternative<BufferPtr>(args[1]);
            std::string encoding = "utf8";
            std::string flag = "w";
            
            // Parse options
            if (args.size() >= 3) {
                if (std::holds_alternative<std::string>(args[2])) {
                    // Simple string encoding (legacy support)
                    encoding = std::get<std::string>(args[2]);
                } else if (std::holds_alternative<ObjectPtr>(args[2])) {
                    ObjectPtr opts = std::get<ObjectPtr>(args[2]);
                    auto enc_it = opts->properties.find("encoding");
                    if (enc_it != opts->properties.end() && std::holds_alternative<std::string>(enc_it->second.value)) {
                        encoding = std::get<std::string>(enc_it->second.value);
                    }
                    auto flag_it = opts->properties.find("flag");
                    if (flag_it != opts->properties.end() && std::holds_alternative<std::string>(flag_it->second.value)) {
                        flag = std::get<std::string>(flag_it->second.value);
                    }
                }
            }
            
            // Determine open mode
            std::ios_base::openmode mode = std::ios::binary;
            if (flag == "a" || flag == "a+") {
                mode |= std::ios::app;
            } else if (flag == "r+") {
                mode |= std::ios::in | std::ios::out;
            }
            // "w" is default (truncate)
            
            std::ofstream out(path, mode);
            if (!out.is_open()) {
                throw SwaziError("IOError", "Failed to open file for writing: " + path, token.loc);
            }
            
            if (is_buffer) {
                // Write buffer directly as binary
                BufferPtr buf = std::get<BufferPtr>(args[1]);
                out.write(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
            } else {
                // Write string (respect encoding if needed)
                std::string content = value_to_string_simple(args[1]);
                out.write(content.data(), content.size());
            }
            
            return Value{true}; }, env);
        obj->properties["writeFile"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // exists(path) -> bool
    {
        auto fn = make_native_fn("fs.exists", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.exists requires a path as an argument. Usage: exists(path) -> bool", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            return Value{ std::filesystem::exists(path) }; }, env);
        obj->properties["exists"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // listDir(path) -> array of entries (strings)
    {
        auto fn = make_native_fn("fs.listDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            std::string path = ".";
            if (!args.empty()) path = value_to_string_simple(args[0]);
            auto arr = std::make_shared<ArrayValue>();
            try {
                for (auto &p : std::filesystem::directory_iterator(path)) {
                    arr->elements.push_back(Value{ p.path().filename().string() });
                }
            } catch (...) {
                // return empty array on error
            }
            return Value{ arr }; }, env);
        obj->properties["listDir"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // copy(src, dest, [overwrite=false]) -> bool
    {
        auto fn = make_native_fn("fs.copy", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw SwaziError("RuntimeError", "fs.copy requires at least two arguments: src and dest, and an optional overwrite flag. Usage: copy(src, dest, [overwrite=false]) -> bool", token.loc);
            };
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = false;
            if (args.size() >= 3) overwrite = std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;
            try {
                std::filesystem::copy_options opts = std::filesystem::copy_options::none;
                if (overwrite) opts = static_cast<std::filesystem::copy_options>(opts | std::filesystem::copy_options::overwrite_existing);
                std::filesystem::copy(src, dest, opts);
                return Value{ true };
            } catch (const std::filesystem::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.copy failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["copy"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // move(src, dest, [overwrite=false]) -> bool
    {
        auto fn = make_native_fn("fs.move", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
              throw SwaziError("RuntimeError", "fs.move requires two arguments: src and dest, and an optional overwrite flag. Usage: move(src, dest, [overwrite=false]) -> bool", token.loc);
            }
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = false;
            if (args.size() >= 3) overwrite = std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;
            try {
                if (std::filesystem::exists(dest)) {
                    if (!overwrite) return Value{ false };
                    std::filesystem::remove_all(dest);
                }
                std::filesystem::rename(src, dest);
                return Value{ true };
            } catch (const std::filesystem::filesystem_error &e) {
                // fallback to copy+remove for cross-device moves
                try {
                    std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive);
                    std::filesystem::remove_all(src);
                    return Value{ true };
                } catch (const std::filesystem::filesystem_error &e2) {
                    throw SwaziError("FilesystemError", std::string("fs.move failed: ") + e.what() + " / " + e2.what(), token.loc);
                }
            } }, env);
        obj->properties["move"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // remove(path) -> bool  (files or directories; directories removed recursively)
    {
        auto fn = make_native_fn("fs.remove", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.remove requires a path as argument. Usage: remove(path) -> bool  (files or directories; directories removed recursively)", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            try {
                if (!std::filesystem::exists(path)) return Value{ false };
                std::uintmax_t removed = std::filesystem::remove_all(path);
                return Value{ removed > 0 };
            } catch (const std::filesystem::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.remove failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["remove"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // makeDir(path, [recursive=true]) -> bool (does not error if dir already exists)
    {
        auto fn = make_native_fn("fs.makeDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
              throw SwaziError("RuntimeError", "fs.makeDir requires a dir path as an argument and an optional recursive flag. Usage: makeDir(path, [recursive=true]) -> bool (does not error if dir already exists)", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            bool recursive = true;
            if (args.size() >= 2) recursive = std::holds_alternative<bool>(args[1]) ? std::get<bool>(args[1]) : true;
            try {
                if (std::filesystem::exists(path)) return Value{ std::filesystem::is_directory(path) };
                bool ok = recursive ? std::filesystem::create_directories(path) : std::filesystem::create_directory(path);
                return Value{ ok };
            } catch (const std::filesystem::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.makeDir failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["makeDir"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // stat(path) -> object
    {
        auto fn = make_native_fn("fs.stat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.stat requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            return build_stat_object(path, false, token); }, env);
        obj->properties["stat"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // lstat(path) -> object (doesn't follow symlinks)
    {
        auto fn = make_native_fn("fs.lstat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.lstat requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            return build_stat_object(path, true, token); }, env);
        obj->properties["lstat"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.chmod(path, mode) -> bool
    {
        auto fn = make_native_fn("fs.chmod", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.chmod requires a path and a numeric mode.", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            if (!std::holds_alternative<double>(args[1])) {
                throw SwaziError("TypeError", "fs.chmod: mode must be a number (e.g., 0o755).", token.loc);
            }
            
            uint32_t mode = static_cast<uint32_t>(std::get<double>(args[1]));
            try {
                // Cast the numeric mode to filesystem permissions
                fs::permissions(path, static_cast<fs::perms>(mode));
                return Value{ true };
            } catch (const fs::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.chmod failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["chmod"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.symlink(target, linkPath) -> bool
    {
        auto fn = make_native_fn("fs.symlink", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.symlink requires a target and a link path.", token.loc);
            }
            std::string target = value_to_string_simple(args[0]);
            std::string linkPath = value_to_string_simple(args[1]);
            try {
                // Determine if target is a directory to use the correct filesystem call
                if (fs::is_directory(target)) {
                    fs::create_directory_symlink(target, linkPath);
                } else {
                    fs::create_symlink(target, linkPath);
                }
                return Value{ true };
            } catch (const fs::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.symlink failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["symlink"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.chown(path, uid, gid) -> bool
    {
        auto fn = make_native_fn("fs.chown", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3) throw SwaziError("RuntimeError", "fs.chown requires path, uid, and gid.", token.loc);

            std::string path = value_to_string_simple(args[0]);
            int uid = static_cast<int>(std::get<double>(args[1]));
            int gid = static_cast<int>(std::get<double>(args[2]));

#ifndef _WIN32
            if (chown(path.c_str(), uid, gid) != 0) {
                throw SwaziError("SystemError", "chown failed: " + std::string(strerror(errno)), token.loc);
            }
            return Value{true};
#else
            throw SwaziError("NotSupportedError", "fs.chown is not implemented on Windows.", token.loc);
#endif
        },
            env);
        obj->properties["chown"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.readlink(path) -> string
    {
        auto fn = make_native_fn("fs.readlink", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) throw SwaziError("RuntimeError", "fs.readlink requires a path.", token.loc);
            
            std::string path = value_to_string_simple(args[0]);
            try {
                return Value{ fs::read_symlink(path).string() };
            } catch (const fs::filesystem_error &e) {
                throw SwaziError("FilesystemError", std::string("fs.readlink failed: ") + e.what(), token.loc);
            } }, env);
        obj->properties["readlink"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.access(path, mode?) -> bool
    {
        auto fn = make_native_fn("fs.access", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.access requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            int mode = 0;  // F_OK by default (file exists)

            if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                mode = static_cast<int>(std::get<double>(args[1]));
            }

#ifndef _WIN32
            return Value{::access(path.c_str(), mode) == 0};
#else
            // Windows limited support
            if (mode == 0) {
                return Value{fs::exists(path)};
            }
            return Value{false};  // Other modes not reliably supported on Windows
#endif
        },
            env);
        obj->properties["access"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.mkfifo(path, mode?) -> bool
    {
        auto fn = make_native_fn("fs.mkfifo", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.mkfifo requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);

#ifndef _WIN32
            // Default mode: 0666 (rw-rw-rw-)
            mode_t mode = 0666;
            if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                mode = static_cast<mode_t>(std::get<double>(args[1]));
            }

            // Create the FIFO
            if (mkfifo(path.c_str(), mode) == 0) {
                return Value{true};
            } else {
                std::string err = std::strerror(errno);
                throw SwaziError("FilesystemError",
                    std::string("fs.mkfifo failed: ") + err, token.loc);
            }
#else
            throw SwaziError("NotSupportedError",
                "fs.mkfifo is not supported on Windows (use named pipes instead)", token.loc);
#endif
        },
            env);
        obj->properties["mkfifo"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.setTimes(path, atime, mtime) -> bool
    {
        auto fn = make_native_fn("fs.setTimes", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 3) {
                throw SwaziError("RuntimeError",
                    "fs.setTimes requires three arguments: path, atime, and mtime. "
                    "Pass null for either time to leave it unchanged.",
                    token.loc);
            }

            std::string path = value_to_string_simple(args[0]);

            // Check file exists first - strict requirement
            if (!fs::exists(path)) {
                throw SwaziError("IOError",
                    "fs.setTimes: file does not exist: " + path,
                    token.loc);
            }

            // Helper to convert Value to time_t
            auto value_to_time_t = [&token](const Value& v) -> std::time_t {
                if (std::holds_alternative<double>(v)) {
                    // Unix timestamp in MILLISECONDS (convert to seconds)
                    double millis = std::get<double>(v);
                    return static_cast<std::time_t>(millis / 1000.0);
                } else if (std::holds_alternative<DateTimePtr>(v)) {
                    // DateTime object - convert epochNanoseconds to seconds
                    DateTimePtr dt = std::get<DateTimePtr>(v);
                    return static_cast<std::time_t>(dt->epochNanoseconds / 1'000'000'000ULL);
                } else {
                    throw SwaziError("TypeError",
                        "fs.setTimes: time must be a number (milliseconds since epoch), DateTime object, or null.",
                        token.loc);
                }
            };

            // Parse atime
            bool set_atime = !std::holds_alternative<std::monostate>(args[1]);
            std::time_t atime = 0;
            if (set_atime) {
                atime = value_to_time_t(args[1]);
            }

            // Parse mtime
            bool set_mtime = !std::holds_alternative<std::monostate>(args[2]);
            std::time_t mtime = 0;
            if (set_mtime) {
                mtime = value_to_time_t(args[2]);
            }

            // If both null, it's a no-op
            if (!set_atime && !set_mtime) {
                return Value{true};
            }

#ifndef _WIN32
            // If only one is set, we need to read the current value for the other
            if (!set_atime || !set_mtime) {
                struct stat st;
                if (stat(path.c_str(), &st) != 0) {
                    throw SwaziError("SystemError",
                        "fs.setTimes: failed to read current timestamps: " + std::string(std::strerror(errno)),
                        token.loc);
                }
                if (!set_atime) atime = st.st_atime;
                if (!set_mtime) mtime = st.st_mtime;
            }

            // Use utimes() on Unix
            struct timeval times[2];
            times[0].tv_sec = atime;
            times[0].tv_usec = 0;
            times[1].tv_sec = mtime;
            times[1].tv_usec = 0;

            if (utimes(path.c_str(), times) != 0) {
                std::string err = std::strerror(errno);
                throw SwaziError("SystemError",
                    "fs.setTimes failed: " + err,
                    token.loc);
            }
            return Value{true};
#else
            // Windows implementation using SetFileTime
            HANDLE hFile = CreateFileA(
                path.c_str(),
                FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS,
                NULL);

            if (hFile == INVALID_HANDLE_VALUE) {
                throw SwaziError("SystemError",
                    "fs.setTimes: failed to open file for attribute modification",
                    token.loc);
            }

            // If only one is set, read current values
            if (!set_atime || !set_mtime) {
                FILETIME ftCreate, ftAccess, ftWrite;
                if (!GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
                    CloseHandle(hFile);
                    throw SwaziError("SystemError",
                        "fs.setTimes: failed to read current timestamps",
                        token.loc);
                }

                // Convert FILETIME to time_t
                auto filetime_to_unix = [](const FILETIME& ft) -> std::time_t {
                    ULARGE_INTEGER uli;
                    uli.LowPart = ft.dwLowDateTime;
                    uli.HighPart = ft.dwHighDateTime;
                    const uint64_t EPOCH_DIFF = 116444736000000000ULL;
                    uint64_t timestamp = (uli.QuadPart - EPOCH_DIFF) / 10000000ULL;
                    return static_cast<std::time_t>(timestamp);
                };

                if (!set_atime) atime = filetime_to_unix(ftAccess);
                if (!set_mtime) mtime = filetime_to_unix(ftWrite);
            }

            // Convert Unix timestamp to Windows FILETIME
            auto unix_to_filetime = [](std::time_t t) -> FILETIME {
                const uint64_t EPOCH_DIFF = 116444736000000000ULL;
                uint64_t temp = (static_cast<uint64_t>(t) * 10000000ULL) + EPOCH_DIFF;
                FILETIME ft;
                ft.dwLowDateTime = static_cast<DWORD>(temp);
                ft.dwHighDateTime = static_cast<DWORD>(temp >> 32);
                return ft;
            };

            FILETIME ft_atime = unix_to_filetime(atime);
            FILETIME ft_mtime = unix_to_filetime(mtime);

            BOOL result = SetFileTime(hFile, NULL, &ft_atime, &ft_mtime);
            CloseHandle(hFile);

            if (!result) {
                throw SwaziError("SystemError",
                    "fs.setTimes: SetFileTime failed",
                    token.loc);
            }
            return Value{true};
#endif
        },
            env);
        obj->properties["setTimes"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // fs.ensureFile(path, mode?) -> bool
    {
        auto fn = make_native_fn("fs.ensureFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", 
                    "fs.ensureFile requires a path argument. Usage: ensureFile(path, mode?) -> bool", 
                    token.loc);
            }
            
            std::string path = value_to_string_simple(args[0]);
            
            // Parse mode (only used if creating)
            uint32_t mode = 0666;  // Default: rw-rw-rw- (subject to umask)
            if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                mode = static_cast<uint32_t>(std::get<double>(args[1]));
            }
            
            try {
                // Check if path exists
                if (fs::exists(path)) {
                    // Path exists - verify it's a regular file
                    auto status = fs::status(path);
                    
                    if (fs::is_directory(status)) {
                        throw SwaziError("FilesystemError", 
                            "fs.ensureFile: path exists but is a directory: " + path, 
                            token.loc);
                    }
                    
                    if (fs::is_symlink(path)) {
                        throw SwaziError("FilesystemError", 
                            "fs.ensureFile: path exists but is a symlink: " + path, 
                            token.loc);
                    }
                    
                    if (!fs::is_regular_file(status)) {
                        throw SwaziError("FilesystemError", 
                            "fs.ensureFile: path exists but is not a regular file: " + path, 
                            token.loc);
                    }
                    
                    // File exists and is regular - idempotent success, no mutation
                    return Value{false};  // false = already existed
                }
                
                // File doesn't exist - need to create it
                // First check parent directory exists
                fs::path file_path(path);
                fs::path parent = file_path.parent_path();
                
                if (!parent.empty() && !fs::exists(parent)) {
                    throw SwaziError("FilesystemError", 
                        "fs.ensureFile: parent directory does not exist: " + parent.string(), 
                        token.loc);
                }
                
                // Create the file
                std::ofstream file(path, std::ios::binary);
                if (!file.is_open()) {
                    throw SwaziError("IOError", 
                        "fs.ensureFile: failed to create file: " + path, 
                        token.loc);
                }
                file.close();

                // Set permissions if on Unix
#ifndef _WIN32
                if (chmod(path.c_str(), mode) != 0) {
                    // File created but permission setting failed
                    // Don't roll back - file exists now
                    std::string err = std::strerror(errno);
                    throw SwaziError("SystemError", 
                        "fs.ensureFile: file created but chmod failed: " + err, 
                        token.loc);
                }
#endif
                
                return Value{true};  // true = created new file
                
            } catch (const fs::filesystem_error& e) {
                throw SwaziError("FilesystemError", 
                    std::string("fs.ensureFile failed: ") + e.what(), 
                    token.loc);
            } }, env);
        obj->properties["ensureFile"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // ============= fs.watch() =============
    {
        auto fn = make_native_fn("fs.watch", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
    if (args.size() < 2) {
        throw SwaziError("RuntimeError", 
            "fs.watch requires path and callback. Usage: fs.watch(path, options?, callback)", 
            token.loc);
    }
    
    std::string path = value_to_string_simple(args[0]);
    bool recursive = false;
    int debounce_ms = 100;  // default
    std::vector<std::string> ignore_patterns;
    std::vector<std::string> include_patterns;
    bool has_include_filter = false;
    FunctionPtr callback;
    
    // Parse arguments: (path, callback) or (path, options, callback)
    if (args.size() == 2) {
        if (!std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "Second argument must be a callback function", token.loc);
        }
        callback = std::get<FunctionPtr>(args[1]);
    } else if (args.size() >= 3) {
        // Parse options
        if (std::holds_alternative<ObjectPtr>(args[1])) {
            ObjectPtr opts = std::get<ObjectPtr>(args[1]);
            
            // Parse recursive
            auto rec_it = opts->properties.find("recursive");
            if (rec_it != opts->properties.end() && std::holds_alternative<bool>(rec_it->second.value)) {
                recursive = std::get<bool>(rec_it->second.value);
            }
            
            // Parse debounce
            auto debounce_it = opts->properties.find("debounce");
            if (debounce_it != opts->properties.end() && std::holds_alternative<double>(debounce_it->second.value)) {
                debounce_ms = static_cast<int>(std::get<double>(debounce_it->second.value));
            }
            
            // Parse ignore patterns
            auto ignore_it = opts->properties.find("ignore");
            if (ignore_it != opts->properties.end()) {
                if (std::holds_alternative<std::string>(ignore_it->second.value)) {
                    ignore_patterns.push_back(std::get<std::string>(ignore_it->second.value));
                } else if (std::holds_alternative<ArrayPtr>(ignore_it->second.value)) {
                    ArrayPtr arr = std::get<ArrayPtr>(ignore_it->second.value);
                    for (const auto& elem : arr->elements) {
                        if (std::holds_alternative<std::string>(elem)) {
                            ignore_patterns.push_back(std::get<std::string>(elem));
                        }
                    }
                }
            }
            
            // Parse include patterns
            auto include_it = opts->properties.find("include");
            if (include_it != opts->properties.end()) {
                has_include_filter = true;
                if (std::holds_alternative<std::string>(include_it->second.value)) {
                    include_patterns.push_back(std::get<std::string>(include_it->second.value));
                } else if (std::holds_alternative<ArrayPtr>(include_it->second.value)) {
                    ArrayPtr arr = std::get<ArrayPtr>(include_it->second.value);
                    for (const auto& elem : arr->elements) {
                        if (std::holds_alternative<std::string>(elem)) {
                            include_patterns.push_back(std::get<std::string>(elem));
                        }
                    }
                }
            }
            
            // Validate: can't have both include and ignore
            if (has_include_filter && !ignore_patterns.empty()) {
                throw SwaziError("RuntimeError", 
                    "fs.watch: Cannot use both 'include' and 'ignore' options together. Use only one.",
                    token.loc);
            }
        }
        
        if (!std::holds_alternative<FunctionPtr>(args[2])) {
            throw SwaziError("TypeError", "Third argument must be a callback function", token.loc);
        }
        callback = std::get<FunctionPtr>(args[2]);
    }
    
    // Verify path exists
    if (!fs::exists(path)) {
        throw SwaziError("IOError", "Watch path does not exist: " + path, token.loc);
    }
    
    if (!fs::is_directory(path)) {
        throw SwaziError("IOError", "Watch path must be a directory: " + path, token.loc);
    }
    
    // Get event loop
    uv_loop_t* loop = scheduler_get_loop();
    if (!loop) {
        throw SwaziError("RuntimeError", "No event loop available for fs.watch", token.loc);
    }
    
    // Create watcher
    auto watcher = std::make_shared<FsWatcher>();
    watcher->path = path;
    watcher->recursive = recursive;
    watcher->callback = callback;
    watcher->id = g_next_watcher_id.fetch_add(1);
    watcher->debounce_ms = debounce_ms;
    watcher->ignore_patterns = ignore_patterns;
    watcher->include_patterns = include_patterns;
    watcher->has_include_filter = has_include_filter;
    
    // Store watcher before initializing libuv
    {
        std::lock_guard<std::mutex> lock(g_fs_watchers_mutex);
        g_fs_watchers[watcher->id] = watcher;
    }
    
    // Initialize on loop thread
    scheduler_run_on_loop([watcher, path, recursive, loop]() {
        // Allocate handle
        watcher->handle = new uv_fs_event_t;
        watcher->handle->data = watcher.get();
        
        // Initialize handle
        int r = uv_fs_event_init(loop, watcher->handle);
        if (r != 0) {
            delete watcher->handle;
            watcher->handle = nullptr;
            std::lock_guard<std::mutex> lock(g_fs_watchers_mutex);
            g_fs_watchers.erase(watcher->id);
            return;
        }
        
        // Start watching
        unsigned int flags = 0;
        if (recursive) {
            flags |= UV_FS_EVENT_RECURSIVE;
        }
        
        r = uv_fs_event_start(watcher->handle, fs_event_callback, path.c_str(), flags);
        if (r != 0) {
            uv_close((uv_handle_t*)watcher->handle, [](uv_handle_t* h) {
                delete (uv_fs_event_t*)h;
            });
            watcher->handle = nullptr;
            std::lock_guard<std::mutex> lock(g_fs_watchers_mutex);
            g_fs_watchers.erase(watcher->id);
            return;
        }
        
        // Successfully started - increment work counter
        g_active_fs_watchers.fetch_add(1);
    });
    
    // Return control object
    auto control = std::make_shared<ObjectValue>();
    Token ctok;
    ctok.loc = TokenLocation("<fs>", 0, 0, 0);
    
    // close() method
    auto close_fn = make_native_fn("watcher.close", [watcher](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (watcher->closed.exchange(true)) {
            return std::monostate{};
        }
        
        scheduler_run_on_loop([watcher]() {
            if (watcher->handle) {
                uv_fs_event_stop(watcher->handle);
                
                uv_close((uv_handle_t*)watcher->handle, [](uv_handle_t* h) {
                    FsWatcher* w = static_cast<FsWatcher*>(h->data);
                    if (w) {
                        g_active_fs_watchers.fetch_sub(1);
                        
                        {
                            std::lock_guard<std::mutex> lock(g_fs_watchers_mutex);
                            g_fs_watchers.erase(w->id);
                        }
                    }
                    delete (uv_fs_event_t*)h;
                });
                
                watcher->handle = nullptr;
            }
        });
        
        return std::monostate{};
    }, nullptr);
    control->properties["close"] = PropertyDescriptor{close_fn, false, false, false, ctok};
    
        // pause() method
    auto pause_fn = make_native_fn("watcher.pause", [watcher](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        watcher->paused.store(true);
        return std::monostate{};
    }, nullptr);
    control->properties["pause"] = PropertyDescriptor{pause_fn, false, false, false, ctok};
    
    // resume() method
    auto resume_fn = make_native_fn("watcher.resume", [watcher](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        watcher->paused.store(false);
        return std::monostate{};
    }, nullptr);
    control->properties["resume"] = PropertyDescriptor{resume_fn, false, false, false, ctok};
    
    // isPaused() method (optional but useful)
    auto is_paused_fn = make_native_fn("watcher.isPaused", [watcher](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{watcher->paused.load()};
    }, nullptr);
    control->properties["isPaused"] = PropertyDescriptor{is_paused_fn, false, false, false, ctok};
        
    return Value{control}; }, env);
        obj->properties["watch"] = PropertyDescriptor{fn, false, false, false, Token()};
    }

    // ============= fs.promises API =============
    {
        // Create promises sub-object for async versions
        auto promises_obj = std::make_shared<ObjectValue>();

        // promises.readFile(path, options?) -> Promise<string | Buffer>
        {
            auto fn = make_native_fn("fs.promises.readFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("RuntimeError", "fs.promises.readFile requires a path argument", token.loc);
                }
                std::string path = value_to_string_simple(args[0]);
                
                // Parse options
                std::string encoding = "utf8";
                if (args.size() >= 2) {
                    if (std::holds_alternative<std::string>(args[1])) {
                        encoding = std::get<std::string>(args[1]);
                    } else if (std::holds_alternative<ObjectPtr>(args[1])) {
                        ObjectPtr opts = std::get<ObjectPtr>(args[1]);
                        auto enc_it = opts->properties.find("encoding");
                        if (enc_it != opts->properties.end()) {
                            if (std::holds_alternative<std::monostate>(enc_it->second.value)) {
                                encoding = "binary";
                            } else if (std::holds_alternative<std::string>(enc_it->second.value)) {
                                encoding = std::get<std::string>(enc_it->second.value);
                            }
                        }
                    }
                }

                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;

                // Schedule async read on loop thread
                scheduler_run_on_loop([promise, path, encoding]() {
                    try {
                        std::ifstream in(path, std::ios::binary);
                        if (!in.is_open()) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Failed to open file: ") + path};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        // Read all bytes
                        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                                 std::istreambuf_iterator<char>());
                        
                        // Return based on encoding
                        Value result;
                        if (encoding == "binary" || encoding == "null") {
                            auto buf = std::make_shared<BufferValue>();
                            buf->data = std::move(data);
                            buf->encoding = "binary";
                            result = Value{buf};
                        } else {
                            result = Value{std::string(data.begin(), data.end())};
                        }
                        
                        // Fulfill promise
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = result;
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Read error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });

                return Value{promise}; }, env);
            promises_obj->properties["readFile"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.writeFile(path, content, options?) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.writeFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.size() < 2) {
                    throw SwaziError("RuntimeError", "fs.promises.writeFile requires path and content arguments", token.loc);
                }
                std::string path = value_to_string_simple(args[0]);
                
                // Capture content (could be string or buffer)
                Value content = args[1];
                bool is_buffer = std::holds_alternative<BufferPtr>(content);
                
                std::string encoding = "utf8";
                std::string flag = "w";
                
                // Parse options
                if (args.size() >= 3) {
                    if (std::holds_alternative<std::string>(args[2])) {
                        encoding = std::get<std::string>(args[2]);
                    } else if (std::holds_alternative<ObjectPtr>(args[2])) {
                        ObjectPtr opts = std::get<ObjectPtr>(args[2]);
                        auto enc_it = opts->properties.find("encoding");
                        if (enc_it != opts->properties.end() && std::holds_alternative<std::string>(enc_it->second.value)) {
                            encoding = std::get<std::string>(enc_it->second.value);
                        }
                        auto flag_it = opts->properties.find("flag");
                        if (flag_it != opts->properties.end() && std::holds_alternative<std::string>(flag_it->second.value)) {
                            flag = std::get<std::string>(flag_it->second.value);
                        }
                    }
                }

                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;

                scheduler_run_on_loop([promise, path, content, is_buffer, flag]() {
                    try {
                        std::ios_base::openmode mode = std::ios::binary;
                        if (flag == "a" || flag == "a+") {
                            mode |= std::ios::app;
                        } else if (flag == "r+") {
                            mode |= std::ios::in | std::ios::out;
                        }
                        
                        std::ofstream out(path, mode);
                        if (!out.is_open()) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Failed to open file for writing: ") + path};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        if (is_buffer) {
                            BufferPtr buf = std::get<BufferPtr>(content);
                            out.write(reinterpret_cast<const char*>(buf->data.data()), buf->data.size());
                        } else {
                            std::string str = value_to_string_simple(content);
                            out.write(str.data(), str.size());
                        }
                        
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{true};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Write error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });

                return Value{promise}; }, env);
            promises_obj->properties["writeFile"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.exists(path) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.exists", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.exists requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                bool exists = std::filesystem::exists(path);
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{exists};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["exists"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.listDir(path) -> Promise<array>
        {
            auto fn = make_native_fn("fs.promises.listDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            std::string path = args.empty() ? "." : value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                try {
                    auto arr = std::make_shared<ArrayValue>();
                    for (auto& p : std::filesystem::directory_iterator(path)) {
                        arr->elements.push_back(Value{p.path().filename().string()});
                    }
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{arr};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::exception& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("List dir error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["listDir"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.copy(src, dest, [overwrite=false]) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.copy", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.promises.copy requires src and dest arguments", token.loc);
            }
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = args.size() >= 3 && std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, src, dest, overwrite]() {
                try {
                    std::filesystem::copy_options opts = std::filesystem::copy_options::none;
                    if (overwrite) opts = static_cast<std::filesystem::copy_options>(opts | std::filesystem::copy_options::overwrite_existing);
                    std::filesystem::copy(src, dest, opts);
                    
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{true};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("Copy error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["copy"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.move(src, dest, [overwrite=false]) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.move", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("RuntimeError", "fs.promises.move requires src and dest arguments", token.loc);
            }
            std::string src = value_to_string_simple(args[0]);
            std::string dest = value_to_string_simple(args[1]);
            bool overwrite = args.size() >= 3 && std::holds_alternative<bool>(args[2]) ? std::get<bool>(args[2]) : false;

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, src, dest, overwrite]() {
                try {
                    if (std::filesystem::exists(dest)) {
                        if (!overwrite) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Destination exists and overwrite is false")};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        std::filesystem::remove_all(dest);
                    }
                    std::filesystem::rename(src, dest);
                    
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{true};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error&) {
                    // Fallback: copy + remove for cross-device moves
                    try {
                        std::filesystem::copy(src, dest, std::filesystem::copy_options::recursive);
                        std::filesystem::remove_all(src);
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{true};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Move error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["move"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.remove(path) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.remove", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.remove requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path]() {
                try {
                    if (!std::filesystem::exists(path)) {
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{false};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                        return;
                    }
                    std::uintmax_t removed = std::filesystem::remove_all(path);
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{removed > 0};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("Remove error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["remove"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.makeDir(path, [recursive=true]) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.makeDir", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("RuntimeError", "fs.promises.makeDir requires a path argument", token.loc);
            }
            std::string path = value_to_string_simple(args[0]);
            bool recursive = args.size() >= 2 && std::holds_alternative<bool>(args[1]) ? std::get<bool>(args[1]) : true;

            auto promise = std::make_shared<PromiseValue>();
            promise->state = PromiseValue::State::PENDING;

            scheduler_run_on_loop([promise, path, recursive]() {
                try {
                    if (std::filesystem::exists(path)) {
                        bool is_dir = std::filesystem::is_directory(path);
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{is_dir};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                        return;
                    }
                    bool ok = recursive ? std::filesystem::create_directories(path) : std::filesystem::create_directory(path);
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{ok};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                } catch (const std::filesystem::filesystem_error& e) {
                    promise->state = PromiseValue::State::REJECTED;
                    promise->result = Value{std::string("MakeDir error: ") + e.what()};
                    for (auto& cb : promise->catch_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                }
            });

            return Value{promise}; }, env);
            promises_obj->properties["makeDir"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.stat(path) -> Promise<object>
        {
            auto fn = make_native_fn("fs.promises.stat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("RuntimeError", "fs.promises.stat requires a path argument", token.loc);
                }
                std::string path = value_to_string_simple(args[0]);
        
                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;
        
                scheduler_run_on_loop([promise, path, token]() {
                    try {
                        Value result = build_stat_object(path, false, token);
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = result;
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Stat error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });
        
                return Value{promise}; }, env);
            promises_obj->properties["stat"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.lstat(path) -> Promise<object>
        {
            auto fn = make_native_fn("fs.promises.lstat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("RuntimeError", "fs.promises.lstat requires a path argument", token.loc);
                }
                std::string path = value_to_string_simple(args[0]);
        
                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;
        
                scheduler_run_on_loop([promise, path, token]() {
                    try {
                        Value result = build_stat_object(path, true, token);
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = result;
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("Stat error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });
        
                return Value{promise}; }, env);
            promises_obj->properties["lstat"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.chmod(path, mode) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.chmod", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("RuntimeError", "fs.promises.chmod requires a path and a numeric mode.", token.loc);
        }
        std::string path = value_to_string_simple(args[0]);
        if (!std::holds_alternative<double>(args[1])) {
            throw SwaziError("TypeError", "fs.promises.chmod: mode must be a number (e.g., 0o755).", token.loc);
        }
        
        uint32_t mode = static_cast<uint32_t>(std::get<double>(args[1]));
        
        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        scheduler_run_on_loop([promise, path, mode]() {
            try {
                fs::permissions(path, static_cast<fs::perms>(mode));
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{true};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            } catch (const fs::filesystem_error& e) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Chmod error: ") + e.what()};
                for (auto& cb : promise->catch_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            }
        });

        return Value{promise}; }, env);
            promises_obj->properties["chmod"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.symlink(target, linkPath) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.symlink", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("RuntimeError", "fs.promises.symlink requires a target and a link path.", token.loc);
        }
        std::string target = value_to_string_simple(args[0]);
        std::string linkPath = value_to_string_simple(args[1]);
        
        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        scheduler_run_on_loop([promise, target, linkPath]() {
            try {
                if (fs::is_directory(target)) {
                    fs::create_directory_symlink(target, linkPath);
                } else {
                    fs::create_symlink(target, linkPath);
                }
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{true};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            } catch (const fs::filesystem_error& e) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Symlink error: ") + e.what()};
                for (auto& cb : promise->catch_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            }
        });

        return Value{promise}; }, env);
            promises_obj->properties["symlink"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.readlink(path) -> Promise<string>
        {
            auto fn = make_native_fn("fs.promises.readlink", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) throw SwaziError("RuntimeError", "fs.promises.readlink requires a path.", token.loc);
        
        std::string path = value_to_string_simple(args[0]);
        
        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        scheduler_run_on_loop([promise, path]() {
            try {
                std::string target = fs::read_symlink(path).string();
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{target};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            } catch (const fs::filesystem_error& e) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Readlink error: ") + e.what()};
                for (auto& cb : promise->catch_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            }
        });

        return Value{promise}; }, env);
            promises_obj->properties["readlink"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.chown(path, uid, gid) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.chown", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.size() < 3) throw SwaziError("RuntimeError", "fs.promises.chown requires path, uid, and gid.", token.loc);

        std::string path = value_to_string_simple(args[0]);
        int uid = static_cast<int>(std::get<double>(args[1]));
        int gid = static_cast<int>(std::get<double>(args[2]));

        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        scheduler_run_on_loop([promise, path, uid, gid]() {
#ifndef _WIN32
            if (chown(path.c_str(), uid, gid) != 0) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Chown failed: ") + strerror(errno)};
                for (auto& cb : promise->catch_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
                return;
            }
            promise->state = PromiseValue::State::FULFILLED;
            promise->result = Value{true};
            for (auto& cb : promise->then_callbacks) {
                try { cb(promise->result); } catch(...) {}
            }
#else
            promise->state = PromiseValue::State::REJECTED;
            promise->result = Value{std::string("fs.promises.chown is not supported on Windows")};
            for (auto& cb : promise->catch_callbacks) {
                try { cb(promise->result); } catch(...) {}
            }
#endif
        });

        return Value{promise}; }, env);
            promises_obj->properties["chown"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.access(path, mode?) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.access", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("RuntimeError", "fs.promises.access requires a path argument", token.loc);
        }
        std::string path = value_to_string_simple(args[0]);
        int mode = 0;
        
        if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
            mode = static_cast<int>(std::get<double>(args[1]));
        }
        
        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        scheduler_run_on_loop([promise, path, mode]() {
#ifndef _WIN32
            bool accessible = ::access(path.c_str(), mode) == 0;
            promise->state = PromiseValue::State::FULFILLED;
            promise->result = Value{accessible};
            for (auto& cb : promise->then_callbacks) {
                try { cb(promise->result); } catch(...) {}
            }
#else
            if (mode == 0) {
                bool exists = fs::exists(path);
                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{exists};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            } else {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Access mode checking not fully supported on Windows")};
                for (auto& cb : promise->catch_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            }
#endif
        });

        return Value{promise}; }, env);
            promises_obj->properties["access"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.mkfifo(path, mode?) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.mkfifo", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
              if (args.empty()) {
                  throw SwaziError("RuntimeError", "fs.promises.mkfifo requires a path argument", token.loc);
              }
              std::string path = value_to_string_simple(args[0]);
              
              mode_t mode = 0666;
              if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                  mode = static_cast<mode_t>(std::get<double>(args[1]));
              }
              
              auto promise = std::make_shared<PromiseValue>();
              promise->state = PromiseValue::State::PENDING;
      
              scheduler_run_on_loop([promise, path, mode]() {
#ifndef _WIN32
                  if (mkfifo(path.c_str(), mode) == 0) {
                      promise->state = PromiseValue::State::FULFILLED;
                      promise->result = Value{true};
                      for (auto& cb : promise->then_callbacks) {
                          try { cb(promise->result); } catch(...) {}
                      }
                  } else {
                      promise->state = PromiseValue::State::REJECTED;
                      promise->result = Value{std::string("mkfifo error: ") + std::strerror(errno)};
                      for (auto& cb : promise->catch_callbacks) {
                          try { cb(promise->result); } catch(...) {}
                      }
                  }
#else
                  promise->state = PromiseValue::State::REJECTED;
                  promise->result = Value{std::string("mkfifo not supported on Windows")};
                  for (auto& cb : promise->catch_callbacks) {
                      try { cb(promise->result); } catch(...) {}
                  }
#endif
              });
      
              return Value{promise}; }, env);
            promises_obj->properties["mkfifo"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.setTimes(path, atime, mtime) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.setTimes", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.size() < 3) {
                    throw SwaziError("RuntimeError", 
                        "fs.promises.setTimes requires three arguments: path, atime, and mtime.", 
                        token.loc);
                }
                
                std::string path = value_to_string_simple(args[0]);
                Value atime_val = args[1];
                Value mtime_val = args[2];
                
                // Helper to convert Value to time_t
                auto value_to_time_t = [](const Value& v) -> std::time_t {
                    if (std::holds_alternative<double>(v)) {
                        double millis = std::get<double>(v);
                        return static_cast<std::time_t>(millis / 1000.0);
                    } else if (std::holds_alternative<DateTimePtr>(v)) {
                        DateTimePtr dt = std::get<DateTimePtr>(v);
                        return static_cast<std::time_t>(dt->epochNanoseconds / 1'000'000'000ULL);
                    } else {
                        throw std::runtime_error("Invalid time type");
                    }
                };
                
                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;
                
                scheduler_run_on_loop([promise, path, atime_val, mtime_val, value_to_time_t]() {
                    try {
                        if (!fs::exists(path)) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("File does not exist: ") + path};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        bool set_atime = !std::holds_alternative<std::monostate>(atime_val);
                        bool set_mtime = !std::holds_alternative<std::monostate>(mtime_val);
                        
                        if (!set_atime && !set_mtime) {
                            promise->state = PromiseValue::State::FULFILLED;
                            promise->result = Value{true};
                            for (auto& cb : promise->then_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        std::time_t atime = 0;
                        std::time_t mtime = 0;
                        
                        if (set_atime) atime = value_to_time_t(atime_val);
                        if (set_mtime) mtime = value_to_time_t(mtime_val);

#ifndef _WIN32
                        // If only one is set, read current values
                        if (!set_atime || !set_mtime) {
                            struct stat st;
                            if (stat(path.c_str(), &st) != 0) {
                                promise->state = PromiseValue::State::REJECTED;
                                promise->result = Value{std::string("Failed to read current timestamps: ") + std::strerror(errno)};
                                for (auto& cb : promise->catch_callbacks) {
                                    try { cb(promise->result); } catch(...) {}
                                }
                                return;
                            }
                            if (!set_atime) atime = st.st_atime;
                            if (!set_mtime) mtime = st.st_mtime;
                        }
                        
                        struct timeval times[2];
                        times[0].tv_sec = atime;
                        times[0].tv_usec = 0;
                        times[1].tv_sec = mtime;
                        times[1].tv_usec = 0;
                        
                        if (utimes(path.c_str(), times) != 0) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("utimes failed: ") + std::strerror(errno)};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
#else
                        // Windows implementation
                        HANDLE hFile = CreateFileA(path.c_str(), FILE_WRITE_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS, NULL);
                        
                        if (hFile == INVALID_HANDLE_VALUE) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Failed to open file")};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        if (!set_atime || !set_mtime) {
                            FILETIME ftCreate, ftAccess, ftWrite;
                            if (!GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
                                CloseHandle(hFile);
                                promise->state = PromiseValue::State::REJECTED;
                                promise->result = Value{std::string("Failed to read current timestamps")};
                                for (auto& cb : promise->catch_callbacks) {
                                    try { cb(promise->result); } catch(...) {}
                                }
                                return;
                            }
                            
                            auto filetime_to_unix = [](const FILETIME& ft) -> std::time_t {
                                ULARGE_INTEGER uli;
                                uli.LowPart = ft.dwLowDateTime;
                                uli.HighPart = ft.dwHighDateTime;
                                const uint64_t EPOCH_DIFF = 116444736000000000ULL;
                                uint64_t timestamp = (uli.QuadPart - EPOCH_DIFF) / 10000000ULL;
                                return static_cast<std::time_t>(timestamp);
                            };
                            
                            if (!set_atime) atime = filetime_to_unix(ftAccess);
                            if (!set_mtime) mtime = filetime_to_unix(ftWrite);
                        }
                        
                        auto unix_to_filetime = [](std::time_t t) -> FILETIME {
                            const uint64_t EPOCH_DIFF = 116444736000000000ULL;
                            uint64_t temp = (static_cast<uint64_t>(t) * 10000000ULL) + EPOCH_DIFF;
                            FILETIME ft;
                            ft.dwLowDateTime = static_cast<DWORD>(temp);
                            ft.dwHighDateTime = static_cast<DWORD>(temp >> 32);
                            return ft;
                        };
                        
                        FILETIME ft_atime = unix_to_filetime(atime);
                        FILETIME ft_mtime = unix_to_filetime(mtime);
                        SetFileTime(hFile, NULL, &ft_atime, &ft_mtime);
                        CloseHandle(hFile);
#endif
                        
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{true};
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("setTimes error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });
                
                return Value{promise}; }, env);
            promises_obj->properties["setTimes"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // promises.ensureFile(path, mode?) -> Promise<bool>
        {
            auto fn = make_native_fn("fs.promises.ensureFile", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
                if (args.empty()) {
                    throw SwaziError("RuntimeError", 
                        "fs.promises.ensureFile requires a path argument", 
                        token.loc);
                }
                
                std::string path = value_to_string_simple(args[0]);
                uint32_t mode = 0666;
                
                if (args.size() >= 2 && std::holds_alternative<double>(args[1])) {
                    mode = static_cast<uint32_t>(std::get<double>(args[1]));
                }
                
                auto promise = std::make_shared<PromiseValue>();
                promise->state = PromiseValue::State::PENDING;
                
                scheduler_run_on_loop([promise, path, mode]() {
                    try {
                        if (fs::exists(path)) {
                            auto status = fs::status(path);
                            
                            if (fs::is_directory(status)) {
                                promise->state = PromiseValue::State::REJECTED;
                                promise->result = Value{std::string("Path is a directory: ") + path};
                                for (auto& cb : promise->catch_callbacks) {
                                    try { cb(promise->result); } catch(...) {}
                                }
                                return;
                            }
                            
                            if (fs::is_symlink(path)) {
                                promise->state = PromiseValue::State::REJECTED;
                                promise->result = Value{std::string("Path is a symlink: ") + path};
                                for (auto& cb : promise->catch_callbacks) {
                                    try { cb(promise->result); } catch(...) {}
                                }
                                return;
                            }
                            
                            if (!fs::is_regular_file(status)) {
                                promise->state = PromiseValue::State::REJECTED;
                                promise->result = Value{std::string("Path is not a regular file: ") + path};
                                for (auto& cb : promise->catch_callbacks) {
                                    try { cb(promise->result); } catch(...) {}
                                }
                                return;
                            }
                            
                            promise->state = PromiseValue::State::FULFILLED;
                            promise->result = Value{false};  // Already existed
                            for (auto& cb : promise->then_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        fs::path file_path(path);
                        fs::path parent = file_path.parent_path();
                        
                        if (!parent.empty() && !fs::exists(parent)) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Parent directory does not exist: ") + parent.string()};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        
                        std::ofstream file(path, std::ios::binary);
                        if (!file.is_open()) {
                            promise->state = PromiseValue::State::REJECTED;
                            promise->result = Value{std::string("Failed to create file: ") + path};
                            for (auto& cb : promise->catch_callbacks) {
                                try { cb(promise->result); } catch(...) {}
                            }
                            return;
                        }
                        file.close();

#ifndef _WIN32
                        chmod(path.c_str(), mode);
#endif
                        
                        promise->state = PromiseValue::State::FULFILLED;
                        promise->result = Value{true};  // Created
                        for (auto& cb : promise->then_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    } catch (const std::exception& e) {
                        promise->state = PromiseValue::State::REJECTED;
                        promise->result = Value{std::string("ensureFile error: ") + e.what()};
                        for (auto& cb : promise->catch_callbacks) {
                            try { cb(promise->result); } catch(...) {}
                        }
                    }
                });
                
                return Value{promise}; }, env);
            promises_obj->properties["ensureFile"] = PropertyDescriptor{fn, false, false, false, Token()};
        }

        // Attach promises sub-object to main fs object
        auto fn = make_native_fn("fs.promises", [promises_obj](const std::vector<Value>& /*args*/, EnvPtr /*callEnv*/, const Token& token) -> Value { return Value{promises_obj}; }, env);
        obj->properties["promises"] = PropertyDescriptor{fn, false, true, true, Token()};
    }

    // fs.constants - fundamental filesystem constants
    {
        auto constants = std::make_shared<ObjectValue>();

        // File access modes (for open/access)
        constants->properties["F_OK"] = PropertyDescriptor{Value{0.0}, false, false, true, Token()};  // File exists
        constants->properties["R_OK"] = PropertyDescriptor{Value{4.0}, false, false, true, Token()};  // File readable
        constants->properties["W_OK"] = PropertyDescriptor{Value{2.0}, false, false, true, Token()};  // File writable
        constants->properties["X_OK"] = PropertyDescriptor{Value{1.0}, false, false, true, Token()};  // File executable

        // File open flags
        constants->properties["O_RDONLY"] = PropertyDescriptor{Value{0.0}, false, false, true, Token()};
        constants->properties["O_WRONLY"] = PropertyDescriptor{Value{1.0}, false, false, true, Token()};
        constants->properties["O_RDWR"] = PropertyDescriptor{Value{2.0}, false, false, true, Token()};
        constants->properties["O_CREAT"] = PropertyDescriptor{Value{64.0}, false, false, true, Token()};
        constants->properties["O_EXCL"] = PropertyDescriptor{Value{128.0}, false, false, true, Token()};
        constants->properties["O_TRUNC"] = PropertyDescriptor{Value{512.0}, false, false, true, Token()};
        constants->properties["O_APPEND"] = PropertyDescriptor{Value{1024.0}, false, false, true, Token()};

        // File type constants (from stat mode)
        constants->properties["S_IFMT"] = PropertyDescriptor{Value{0170000.0}, false, false, true, Token()};   // Bit mask for file type
        constants->properties["S_IFREG"] = PropertyDescriptor{Value{0100000.0}, false, false, true, Token()};  // Regular file
        constants->properties["S_IFDIR"] = PropertyDescriptor{Value{0040000.0}, false, false, true, Token()};  // Directory
        constants->properties["S_IFLNK"] = PropertyDescriptor{Value{0120000.0}, false, false, true, Token()};  // Symbolic link

        // File mode bits (permissions)
        constants->properties["S_IRWXU"] = PropertyDescriptor{Value{0700.0}, false, false, true, Token()};  // Owner read/write/execute
        constants->properties["S_IRUSR"] = PropertyDescriptor{Value{0400.0}, false, false, true, Token()};  // Owner read
        constants->properties["S_IWUSR"] = PropertyDescriptor{Value{0200.0}, false, false, true, Token()};  // Owner write
        constants->properties["S_IXUSR"] = PropertyDescriptor{Value{0100.0}, false, false, true, Token()};  // Owner execute

        constants->properties["S_IRWXG"] = PropertyDescriptor{Value{0070.0}, false, false, true, Token()};  // Group read/write/execute
        constants->properties["S_IRGRP"] = PropertyDescriptor{Value{0040.0}, false, false, true, Token()};  // Group read
        constants->properties["S_IWGRP"] = PropertyDescriptor{Value{0020.0}, false, false, true, Token()};  // Group write
        constants->properties["S_IXGRP"] = PropertyDescriptor{Value{0010.0}, false, false, true, Token()};  // Group execute

        constants->properties["S_IRWXO"] = PropertyDescriptor{Value{0007.0}, false, false, true, Token()};  // Others read/write/execute
        constants->properties["S_IROTH"] = PropertyDescriptor{Value{0004.0}, false, false, true, Token()};  // Others read
        constants->properties["S_IWOTH"] = PropertyDescriptor{Value{0002.0}, false, false, true, Token()};  // Others write
        constants->properties["S_IXOTH"] = PropertyDescriptor{Value{0001.0}, false, false, true, Token()};  // Others execute

        obj->properties["constants"] = PropertyDescriptor{Value{constants}, false, false, true, Token()};
    }

    // fs.glob(pattern, options?) -> array of paths
    // Options: { cwd: ".", absolute: false, onlyFiles: false, onlyDirectories: false, deep: Infinity }
    {
        auto fn = make_native_fn("fs.glob", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
    if (args.empty()) {
        throw SwaziError("RuntimeError", "fs.glob requires a pattern argument. Usage: glob(pattern, options?) -> [matches]", token.loc);
    }
    
    std::string pattern = value_to_string_simple(args[0]);
    std::string cwd = ".";
    bool absolute = false;
    bool onlyFiles = false;
    bool onlyDirectories = false;
    
    // Parse options
    if (args.size() >= 2 && std::holds_alternative<ObjectPtr>(args[1])) {
        ObjectPtr opts = std::get<ObjectPtr>(args[1]);
        
        auto cwd_it = opts->properties.find("cwd");
        if (cwd_it != opts->properties.end() && std::holds_alternative<std::string>(cwd_it->second.value)) {
            cwd = std::get<std::string>(cwd_it->second.value);
        }
        
        auto abs_it = opts->properties.find("absolute");
        if (abs_it != opts->properties.end() && std::holds_alternative<bool>(abs_it->second.value)) {
            absolute = std::get<bool>(abs_it->second.value);
        }
        
        auto files_it = opts->properties.find("onlyFiles");
        if (files_it != opts->properties.end() && std::holds_alternative<bool>(files_it->second.value)) {
            onlyFiles = std::get<bool>(files_it->second.value);
        }
        
        auto dirs_it = opts->properties.find("onlyDirectories");
        if (dirs_it != opts->properties.end() && std::holds_alternative<bool>(dirs_it->second.value)) {
            onlyDirectories = std::get<bool>(dirs_it->second.value);
        }
    }
    
    auto result = std::make_shared<ArrayValue>();
    
    try {
        // Check if pattern has ** (globstar - recursive wildcard)
        bool has_globstar = (pattern.find("**") != std::string::npos);
        
        if (!has_globstar) {
            // Non-recursive glob - use filesystem iteration with exact depth matching
            // Split pattern into directory parts and filename part
            fs::path pattern_path(pattern);
            fs::path search_base = fs::path(cwd);
            
            // Build the search path by processing non-wildcard directory components
            std::vector<std::string> parts;
            for (const auto& part : pattern_path) {
                parts.push_back(part.string());
            }
            
            // Recursive function to match each level
            std::function<void(const fs::path&, size_t)> match_level;
            match_level = [&](const fs::path& current_dir, size_t part_idx) {
                if (part_idx >= parts.size()) return;
                
                std::string current_pattern = parts[part_idx];
                bool is_last = (part_idx == parts.size() - 1);
                
                try {
                    for (auto& entry : fs::directory_iterator(current_dir)) {
                        std::string name = entry.path().filename().string();
                        
                        // Check if this entry matches the current pattern part
                        if (matches_pattern(name, current_pattern)) {
                            if (is_last) {
                                // Last part - this is a match
                                if (onlyFiles && !entry.is_regular_file()) continue;
                                if (onlyDirectories && !entry.is_directory()) continue;
                                
                                std::string path_to_add = absolute ? 
                                    fs::absolute(entry.path()).string() : 
                                    fs::relative(entry.path(), cwd).string();
                                result->elements.push_back(Value{path_to_add});
                            } else {
                                // Not last part - continue matching if it's a directory
                                if (entry.is_directory()) {
                                    match_level(entry.path(), part_idx + 1);
                                }
                            }
                        }
                    }
                } catch (const fs::filesystem_error&) {
                    // Skip directories we can't read
                }
            };
            
            match_level(search_base, 0);
            
        } else {
            // Has ** - recursive glob
            std::function<void(const fs::path&)> walk;
            walk = [&](const fs::path& dir) {
                try {
                    for (auto& entry : fs::directory_iterator(dir)) {
                        std::string relative_path = fs::relative(entry.path(), cwd).string();
                        
                        // Normalize path separators
                        std::replace(relative_path.begin(), relative_path.end(), '\\', '/');
                        
                        // Check if matches pattern
                        if (matches_pattern(relative_path, pattern)) {
                            if (onlyFiles && !entry.is_regular_file()) continue;
                            if (onlyDirectories && !entry.is_directory()) continue;
                            
                            std::string path_to_add = absolute ? 
                                fs::absolute(entry.path()).string() : 
                                relative_path;
                            result->elements.push_back(Value{path_to_add});
                        }
                        
                        // Always recurse for ** patterns
                        if (entry.is_directory()) {
                            walk(entry.path());
                        }
                    }
                } catch (const fs::filesystem_error&) {
                    // Skip directories we can't read
                }
            };
            
            walk(fs::path(cwd));
        }
        
    } catch (const fs::filesystem_error& e) {
        // Return empty array on error
    }
    
    return Value{result}; }, env);
        obj->properties["glob"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    return obj;
}

bool fs_has_active_work() {
    return g_active_fs_watchers.load() > 0;
}