#include <algorithm>
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

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <processthreadsapi.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

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

// ============= FS.WATCH IMPLEMENTATION =============

struct FileWatcher {
    std::string path;
    bool recursive;
    FunctionPtr callback;
    std::thread watch_thread;
    std::atomic<bool> should_stop{false};

    // Track file states
    struct FileState {
        fs::file_time_type last_write;
        uintmax_t size;
        bool exists;
    };
    std::unordered_map<std::string, FileState> file_states;

    FileWatcher(const std::string& p, bool rec, FunctionPtr cb)
        : path(p), recursive(rec), callback(cb) {}
};

static std::vector<std::shared_ptr<FileWatcher>> active_watchers;
static std::mutex watchers_mutex;

// Background thread function for watching
static void watch_thread_func(std::shared_ptr<FileWatcher> watcher) {
    auto get_file_state = [](const fs::path& p) -> FileWatcher::FileState {
        FileWatcher::FileState state;
        try {
            if (fs::exists(p)) {
                state.exists = true;
                state.last_write = fs::last_write_time(p);
                if (fs::is_regular_file(p)) {
                    state.size = fs::file_size(p);
                } else {
                    state.size = 0;
                }
            } else {
                state.exists = false;
                state.size = 0;
            }
        } catch (...) {
            state.exists = false;
            state.size = 0;
        }
        return state;
    };

    // Initial scan
    try {
        if (watcher->recursive) {
            for (auto& entry : fs::recursive_directory_iterator(watcher->path, fs::directory_options::skip_permission_denied)) {
                if (fs::is_regular_file(entry)) {
                    watcher->file_states[entry.path().string()] = get_file_state(entry.path());
                }
            }
        } else {
            for (auto& entry : fs::directory_iterator(watcher->path, fs::directory_options::skip_permission_denied)) {
                if (fs::is_regular_file(entry)) {
                    watcher->file_states[entry.path().string()] = get_file_state(entry.path());
                }
            }
        }
    } catch (...) {
        // Ignore initial scan errors
    }

    // Watch loop
    while (!watcher->should_stop) {
        try {
            std::unordered_map<std::string, FileWatcher::FileState> current_states;

            // Scan current state
            if (watcher->recursive) {
                for (auto& entry : fs::recursive_directory_iterator(watcher->path, fs::directory_options::skip_permission_denied)) {
                    if (fs::is_regular_file(entry)) {
                        current_states[entry.path().string()] = get_file_state(entry.path());
                    }
                }
            } else {
                for (auto& entry : fs::directory_iterator(watcher->path, fs::directory_options::skip_permission_denied)) {
                    if (fs::is_regular_file(entry)) {
                        current_states[entry.path().string()] = get_file_state(entry.path());
                    }
                }
            }

            // Check for changes
            for (auto& [path, current_state] : current_states) {
                auto it = watcher->file_states.find(path);

                if (it == watcher->file_states.end()) {
                    // New file
                    auto event = std::make_shared<ObjectValue>();
                    event->properties["type"] = PropertyDescriptor{Value{std::string("create")}, false, false, true, Token()};
                    event->properties["path"] = PropertyDescriptor{Value{path}, false, false, true, Token()};

                    if (watcher->callback) {
                        CallbackPayload* payload = new CallbackPayload(watcher->callback, {Value{event}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                } else if (it->second.last_write != current_state.last_write ||
                    it->second.size != current_state.size) {
                    // Modified file
                    auto event = std::make_shared<ObjectValue>();
                    event->properties["type"] = PropertyDescriptor{Value{std::string("change")}, false, false, true, Token()};
                    event->properties["path"] = PropertyDescriptor{Value{path}, false, false, true, Token()};

                    if (watcher->callback) {
                        CallbackPayload* payload = new CallbackPayload(watcher->callback, {Value{event}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                }
            }

            // Check for deletions
            for (auto& [path, old_state] : watcher->file_states) {
                if (current_states.find(path) == current_states.end()) {
                    // Deleted file
                    auto event = std::make_shared<ObjectValue>();
                    event->properties["type"] = PropertyDescriptor{Value{std::string("delete")}, false, false, true, Token()};
                    event->properties["path"] = PropertyDescriptor{Value{path}, false, false, true, Token()};

                    if (watcher->callback) {
                        CallbackPayload* payload = new CallbackPayload(watcher->callback, {Value{event}});
                        enqueue_callback_global(static_cast<void*>(payload));
                    }
                }
            }

            watcher->file_states = std::move(current_states);

        } catch (...) {
            // Ignore scan errors during watching
        }

        // Sleep for a bit (polling interval)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
        
        auto obj = std::make_shared<ObjectValue>();
        
        try {
            if (!std::filesystem::exists(path)) {
                obj->properties["exists"] = PropertyDescriptor{Value{false}, false, false, true, Token()};
                return Value{obj};
            }
            
            auto status = std::filesystem::status(path);
            bool isFile = std::filesystem::is_regular_file(status);
            bool isDir = std::filesystem::is_directory(status);
            bool isSymlink = std::filesystem::is_symlink(path);
            
            uintmax_t size = 0;
            if (isFile) {
                size = std::filesystem::file_size(path);
            }
            
            // Get timestamps
            std::string mtime;  // Modified time
            std::string ctime;  // Creation time (birthtime on supporting systems)
            std::string atime;  // Access time
            
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

                // Get creation and access times (platform-specific)
#ifndef _WIN32
            // Unix: use stat() to get all timestamps
            struct stat st;
            bool stat_success = (stat(path.c_str(), &st) == 0); 
            if (stat_success) {
                // Birth time (creation time) - available on some systems
                std::tm tm{};
                char buf[64];

                    // Access time
#ifdef __APPLE__
                // macOS has st_atimespec
                std::time_t at = st.st_atimespec.tv_sec;
#else
                std::time_t at = st.st_atime;
#endif
                gmtime_r(&at, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                atime = buf;

                    // Birth time (creation) - platform specific
#ifdef __APPLE__
                // macOS has st_birthtimespec
                std::time_t bt = st.st_birthtimespec.tv_sec;
                gmtime_r(&bt, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                ctime = buf;
#elif defined(__linux__)
                // Linux: statx() can provide birth time on newer kernels/filesystems
                // For now, fall back to ctime (inode change time, not creation)
                std::time_t ct = st.st_ctime;
                gmtime_r(&ct, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                ctime = buf;  // Note: this is inode change time, not true creation
#else
                ctime = "";  // Not available
#endif
            }
#else
            // Windows: use GetFileTime for proper creation time
            HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, 
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                FILETIME ftCreate, ftAccess, ftWrite;
                if (GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
                    auto convert_filetime = [](const FILETIME& ft) -> std::string {
                        ULARGE_INTEGER uli;
                        uli.LowPart = ft.dwLowDateTime;
                        uli.HighPart = ft.dwHighDateTime;
                        
                        // Convert to Unix epoch
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
            
            // Get raw permissions (the actual bits)
            auto perms = status.permissions();
            uint32_t perms_raw = static_cast<uint32_t>(perms);
            
            // Compute human-readable summary (convenience)
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
            
            // Standard fields (normalized cross-platform)
            obj->properties["exists"] = PropertyDescriptor{Value{true}, false, false, true, Token()};
            obj->properties["type"] = PropertyDescriptor{
                Value{isFile ? std::string("file") : isDir ? std::string("directory") : isSymlink ? std::string("symlink") : std::string("other")},
                false, false, true, Token()
            };
            obj->properties["size"] = PropertyDescriptor{Value{static_cast<double>(size)}, false, false, true, Token()};
            
            // Timestamps (standard names)
            obj->properties["mtime"] = PropertyDescriptor{Value{mtime}, false, false, true, Token()};        // Modified
            obj->properties["ctime"] = PropertyDescriptor{Value{ctime}, false, false, true, Token()};        // Created
            obj->properties["atime"] = PropertyDescriptor{Value{atime}, false, false, true, Token()};        // Accessed
            
            // Permissions (both forms)
            obj->properties["permissions"] = PropertyDescriptor{Value{perms_summary}, false, false, true, Token()};
            obj->properties["mode"] = PropertyDescriptor{Value{static_cast<double>(perms_raw)}, false, false, true, Token()};
            
            // Deprecated fields (for compatibility)
            obj->properties["isFile"] = PropertyDescriptor{Value{isFile}, false, false, true, Token()};
            obj->properties["isDir"] = PropertyDescriptor{Value{isDir}, false, false, true, Token()};

                // Platform-specific raw data
#ifndef _WIN32
            // Unix: include inode, device, nlink, uid, gid
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
            // Windows: include file attributes
            DWORD attrs = GetFileAttributesA(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                auto raw_obj = std::make_shared<ObjectValue>();
                raw_obj->properties["attributes"] = PropertyDescriptor{Value{static_cast<double>(attrs)}, false, false, true, Token()};
                raw_obj->properties["hidden"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_HIDDEN) != 0}, false, false, true, Token()};
                raw_obj->properties["system"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_SYSTEM) != 0}, false, false, true, Token()};
                raw_obj->properties["archive"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_ARCHIVE) != 0}, false, false, true, Token()};
                
                obj->properties["raw"] = PropertyDescriptor{Value{raw_obj}, false, false, true, Token()};
            }
#endif
            
            return Value{obj};
        } catch (const std::filesystem::filesystem_error &e) {
            throw SwaziError("FilesystemError", std::string("fs.stat failed: ") + e.what(), token.loc);
        } }, env);
        obj->properties["stat"] = PropertyDescriptor{fn, false, false, true, Token()};
    }

    // lstat(path) -> object doesnt follow symlink
    {
        auto fn = make_native_fn("fs.lstat", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("RuntimeError", "fs.stat requires a path argument", token.loc);
        }
        std::string path = value_to_string_simple(args[0]);
        
        auto obj = std::make_shared<ObjectValue>();
        
        try {
            if (!std::filesystem::exists(path)) {
                obj->properties["exists"] = PropertyDescriptor{Value{false}, false, false, true, Token()};
                return Value{obj};
            }
            
            auto status = std::filesystem::symlink_status(path); 
            bool isFile = std::filesystem::is_regular_file(status);
            bool isDir = std::filesystem::is_directory(status);
            bool isSymlink = std::filesystem::is_symlink(path);
            
            uintmax_t size = 0;
            if (isFile) {
                size = std::filesystem::file_size(path);
            }
            
            // Get timestamps
            std::string mtime;  // Modified time
            std::string ctime;  // Creation time (birthtime on supporting systems)
            std::string atime;  // Access time
            
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

                // Get creation and access times (platform-specific)
#ifndef _WIN32
            // Unix: use stat() to get all timestamps
            struct stat st;
            bool stat_success = (stat(path.c_str(), &st) == 0); 
            if (stat_success) {
                // Birth time (creation time) - available on some systems
                std::tm tm{};
                char buf[64];

                    // Access time
#ifdef __APPLE__
                // macOS has st_atimespec
                std::time_t at = st.st_atimespec.tv_sec;
#else
                std::time_t at = st.st_atime;
#endif
                gmtime_r(&at, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                atime = buf;

                    // Birth time (creation) - platform specific
#ifdef __APPLE__
                // macOS has st_birthtimespec
                std::time_t bt = st.st_birthtimespec.tv_sec;
                gmtime_r(&bt, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                ctime = buf;
#elif defined(__linux__)
                // Linux: statx() can provide birth time on newer kernels/filesystems
                // For now, fall back to ctime (inode change time, not creation)
                std::time_t ct = st.st_ctime;
                gmtime_r(&ct, &tm);
                std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                ctime = buf;  // Note: this is inode change time, not true creation
#else
                ctime = "";  // Not available
#endif
            }
#else
            // Windows: use GetFileTime for proper creation time
            HANDLE hFile = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, 
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (hFile != INVALID_HANDLE_VALUE) {
                FILETIME ftCreate, ftAccess, ftWrite;
                if (GetFileTime(hFile, &ftCreate, &ftAccess, &ftWrite)) {
                    auto convert_filetime = [](const FILETIME& ft) -> std::string {
                        ULARGE_INTEGER uli;
                        uli.LowPart = ft.dwLowDateTime;
                        uli.HighPart = ft.dwHighDateTime;
                        
                        // Convert to Unix epoch
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
            
            // Get raw permissions (the actual bits)
            auto perms = status.permissions();
            uint32_t perms_raw = static_cast<uint32_t>(perms);
            
            // Compute human-readable summary (convenience)
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
            
            // Standard fields (normalized cross-platform)
            obj->properties["exists"] = PropertyDescriptor{Value{true}, false, false, true, Token()};
            obj->properties["type"] = PropertyDescriptor{
                Value{isFile ? std::string("file") : isDir ? std::string("directory") : isSymlink ? std::string("symlink") : std::string("other")},
                false, false, true, Token()
            };
            obj->properties["size"] = PropertyDescriptor{Value{static_cast<double>(size)}, false, false, true, Token()};
            
            // Timestamps (standard names)
            obj->properties["mtime"] = PropertyDescriptor{Value{mtime}, false, false, true, Token()};        // Modified
            obj->properties["ctime"] = PropertyDescriptor{Value{ctime}, false, false, true, Token()};        // Created
            obj->properties["atime"] = PropertyDescriptor{Value{atime}, false, false, true, Token()};        // Accessed
            
            // Permissions (both forms)
            obj->properties["permissions"] = PropertyDescriptor{Value{perms_summary}, false, false, true, Token()};
            obj->properties["mode"] = PropertyDescriptor{Value{static_cast<double>(perms_raw)}, false, false, true, Token()};
            
            // Deprecated fields (for compatibility)
            obj->properties["isFile"] = PropertyDescriptor{Value{isFile}, false, false, true, Token()};
            obj->properties["isDir"] = PropertyDescriptor{Value{isDir}, false, false, true, Token()};

                // Platform-specific raw data
#ifndef _WIN32
            // Unix: include inode, device, nlink, uid, gid
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
            // Windows: include file attributes
            DWORD attrs = GetFileAttributesA(path.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES) {
                auto raw_obj = std::make_shared<ObjectValue>();
                raw_obj->properties["attributes"] = PropertyDescriptor{Value{static_cast<double>(attrs)}, false, false, true, Token()};
                raw_obj->properties["hidden"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_HIDDEN) != 0}, false, false, true, Token()};
                raw_obj->properties["system"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_SYSTEM) != 0}, false, false, true, Token()};
                raw_obj->properties["archive"] = PropertyDescriptor{Value{(attrs & FILE_ATTRIBUTE_ARCHIVE) != 0}, false, false, true, Token()};
                
                obj->properties["raw"] = PropertyDescriptor{Value{raw_obj}, false, false, true, Token()};
            }
#endif
            
            return Value{obj};
        } catch (const std::filesystem::filesystem_error &e) {
            throw SwaziError("FilesystemError", std::string("fs.stat failed: ") + e.what(), token.loc);
        } }, env);
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
                    auto rec_it = opts->properties.find("recursive");
                    if (rec_it != opts->properties.end() && std::holds_alternative<bool>(rec_it->second.value)) {
                        recursive = std::get<bool>(rec_it->second.value);
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
            
            // Create watcher
            auto watcher = std::make_shared<FileWatcher>(path, recursive, callback);
            
            // Start watch thread
            watcher->watch_thread = std::thread(watch_thread_func, watcher);
            watcher->watch_thread.detach();
            
            // Store watcher
            {
                std::lock_guard<std::mutex> lock(watchers_mutex);
                active_watchers.push_back(watcher);
            }
            
            // Return watcher control object
            auto control = std::make_shared<ObjectValue>();
            
            // close() method
            auto close_fn = make_native_fn("watcher.close", [watcher](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                watcher->should_stop = true;
                return Value{std::monostate{}};
            }, nullptr);
            control->properties["close"] = PropertyDescriptor{close_fn, false, false, false, Token()};
            
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

        scheduler_run_on_loop([promise, path]() {
            try {
                auto obj = std::make_shared<ObjectValue>();
                
                if (!std::filesystem::exists(path)) {
                    obj->properties["exists"] = PropertyDescriptor{Value{false}, false, false, true, Token()};
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{obj};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                    return;
                }

                auto status = std::filesystem::status(path);
                bool isFile = std::filesystem::is_regular_file(status);
                bool isDir = std::filesystem::is_directory(status);
                bool isSymlink = std::filesystem::is_symlink(path);
                
                uintmax_t size = 0;
                if (isFile) {
                    size = std::filesystem::file_size(path);
                }

                // Get timestamps
                std::string mtime;  // Modified time
                std::string ctime;  // Creation time
                std::string atime;  // Access time

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

                        // Get creation and access times (platform-specific)
#ifndef _WIN32
                // Unix: use stat() to get all timestamps
                struct stat st;
                bool stat_success = (stat(path.c_str(), &st) == 0);
                if (stat_success) {
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
                    // Linux: use ctime (inode change time)
                    std::time_t ct = st.st_ctime;
                    gmtime_r(&ct, &tm);
                    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                    ctime = buf;
#else
                    ctime = "";
#endif
                }
#else
                // Windows: use GetFileTime for proper creation time
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

                // Get permissions (both raw and human-readable)
                auto perms = status.permissions();
                uint32_t perms_raw = static_cast<uint32_t>(perms);
                
                // Full permission summary (rwxrwxrwx)
                std::string permSummary;
                permSummary += ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? "x" : "-";
                permSummary += ((perms & std::filesystem::perms::group_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::group_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none) ? "x" : "-";
                permSummary += ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::others_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none) ? "x" : "-";

                // Standard fields (normalized cross-platform)
                obj->properties["exists"] = PropertyDescriptor{Value{true}, false, false, true, Token()};
                obj->properties["type"] = PropertyDescriptor{
                    Value{isFile ? std::string("file") : isDir ? std::string("directory") : isSymlink ? std::string("symlink") : std::string("other")},
                    false, false, true, Token()
                };
                obj->properties["size"] = PropertyDescriptor{Value{static_cast<double>(size)}, false, false, true, Token()};
                
                // Timestamps (standard names)
                obj->properties["mtime"] = PropertyDescriptor{Value{mtime}, false, false, true, Token()};
                obj->properties["ctime"] = PropertyDescriptor{Value{ctime}, false, false, true, Token()};
                obj->properties["atime"] = PropertyDescriptor{Value{atime}, false, false, true, Token()};
                
                // Permissions (both convenience and raw)
                obj->properties["permissions"] = PropertyDescriptor{Value{permSummary}, false, false, true, Token()};
                obj->properties["mode"] = PropertyDescriptor{Value{static_cast<double>(perms_raw)}, false, false, true, Token()};
                
                // Deprecated fields (for compatibility)
                obj->properties["isFile"] = PropertyDescriptor{Value{isFile}, false, false, true, Token()};
                obj->properties["isDir"] = PropertyDescriptor{Value{isDir}, false, false, true, Token()};

                        // Platform-specific raw data
#ifndef _WIN32
                // Unix: include inode, device, nlink, uid, gid, etc.
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
                // Windows: include file attributes
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

                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{obj};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            } catch (const std::filesystem::filesystem_error& e) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Stat error: ") + e.what()};
                for (auto& cb : promise->catch_callbacks) {
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
            throw SwaziError("RuntimeError", "fs.promises.stat requires a path argument", token.loc);
        }
        std::string path = value_to_string_simple(args[0]);

        auto promise = std::make_shared<PromiseValue>();
        promise->state = PromiseValue::State::PENDING;

        scheduler_run_on_loop([promise, path]() {
            try {
                auto obj = std::make_shared<ObjectValue>();
                
                if (!std::filesystem::exists(path)) {
                    obj->properties["exists"] = PropertyDescriptor{Value{false}, false, false, true, Token()};
                    promise->state = PromiseValue::State::FULFILLED;
                    promise->result = Value{obj};
                    for (auto& cb : promise->then_callbacks) {
                        try { cb(promise->result); } catch(...) {}
                    }
                    return;
                }

                auto status = std::filesystem::symlink_status(path); 
                bool isFile = std::filesystem::is_regular_file(status);
                bool isDir = std::filesystem::is_directory(status);
                bool isSymlink = std::filesystem::is_symlink(path);
                
                uintmax_t size = 0;
                if (isFile) {
                    size = std::filesystem::file_size(path);
                }

                // Get timestamps
                std::string mtime;  // Modified time
                std::string ctime;  // Creation time
                std::string atime;  // Access time

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

                        // Get creation and access times (platform-specific)
#ifndef _WIN32
                // Unix: use stat() to get all timestamps
                struct stat st;
                bool stat_success = (stat(path.c_str(), &st) == 0); 
                if (stat_success) {
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
                    // Linux: use ctime (inode change time)
                    std::time_t ct = st.st_ctime;
                    gmtime_r(&ct, &tm);
                    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
                    ctime = buf;
#else
                    ctime = "";
#endif
                }
#else
                // Windows: use GetFileTime for proper creation time
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

                // Get permissions (both raw and human-readable)
                auto perms = status.permissions();
                uint32_t perms_raw = static_cast<uint32_t>(perms);
                
                // Full permission summary (rwxrwxrwx)
                std::string permSummary;
                permSummary += ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none) ? "x" : "-";
                permSummary += ((perms & std::filesystem::perms::group_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::group_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none) ? "x" : "-";
                permSummary += ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none) ? "r" : "-";
                permSummary += ((perms & std::filesystem::perms::others_write) != std::filesystem::perms::none) ? "w" : "-";
                permSummary += ((perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none) ? "x" : "-";

                // Standard fields (normalized cross-platform)
                obj->properties["exists"] = PropertyDescriptor{Value{true}, false, false, true, Token()};
                obj->properties["type"] = PropertyDescriptor{
                    Value{isFile ? std::string("file") : isDir ? std::string("directory") : isSymlink ? std::string("symlink") : std::string("other")},
                    false, false, true, Token()
                };
                obj->properties["size"] = PropertyDescriptor{Value{static_cast<double>(size)}, false, false, true, Token()};
                
                // Timestamps (standard names)
                obj->properties["mtime"] = PropertyDescriptor{Value{mtime}, false, false, true, Token()};
                obj->properties["ctime"] = PropertyDescriptor{Value{ctime}, false, false, true, Token()};
                obj->properties["atime"] = PropertyDescriptor{Value{atime}, false, false, true, Token()};
                
                // Permissions (both convenience and raw)
                obj->properties["permissions"] = PropertyDescriptor{Value{permSummary}, false, false, true, Token()};
                obj->properties["mode"] = PropertyDescriptor{Value{static_cast<double>(perms_raw)}, false, false, true, Token()};
                
                // Deprecated fields (for compatibility)
                obj->properties["isFile"] = PropertyDescriptor{Value{isFile}, false, false, true, Token()};
                obj->properties["isDir"] = PropertyDescriptor{Value{isDir}, false, false, true, Token()};

                        // Platform-specific raw data
#ifndef _WIN32
                // Unix: include inode, device, nlink, uid, gid, etc.
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
                // Windows: include file attributes
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

                promise->state = PromiseValue::State::FULFILLED;
                promise->result = Value{obj};
                for (auto& cb : promise->then_callbacks) {
                    try { cb(promise->result); } catch(...) {}
                }
            } catch (const std::filesystem::filesystem_error& e) {
                promise->state = PromiseValue::State::REJECTED;
                promise->result = Value{std::string("Stat error: ") + e.what()};
                for (auto& cb : promise->catch_callbacks) {
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

    return obj;
}