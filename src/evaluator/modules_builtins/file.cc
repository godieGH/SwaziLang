
#include "builtins.hpp"
#include "SwaziError.hpp"
#include "evaluator.hpp"

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#else
#include <windows.h>
#include <io.h>
#endif
// Helper: close file handle (called by FileValue destructor and close())
void FileValue::close_internal() {
if (!is_open) return;
#ifdef _WIN32
if (handle && handle != INVALID_HANDLE_VALUE) {
    CloseHandle((HANDLE)handle);
    handle = nullptr;
}
#else
if (fd >= 0) {
    ::close(fd);
    fd = -1;
}
#endif

is_open = false;
}
// Helper: convert mode string to flags
static int parse_mode_flags(const std::string& mode, bool& binary) {
binary = (mode.find('b') != std::string::npos);
#ifdef _WIN32
// Windows mode flags
if (mode.find("r+") != std::string::npos || mode.find("rb+") != std::string::npos)
    return GENERIC_READ | GENERIC_WRITE;
if (mode.find('r') != std::string::npos)
    return GENERIC_READ;
if (mode.find("w+") != std::string::npos || mode.find("wb+") != std::string::npos)
    return GENERIC_READ | GENERIC_WRITE;
if (mode.find('w') != std::string::npos)
    return GENERIC_WRITE;
if (mode.find("a+") != std::string::npos || mode.find("ab+") != std::string::npos)
    return GENERIC_READ | GENERIC_WRITE;
if (mode.find('a') != std::string::npos)
    return GENERIC_WRITE;
return GENERIC_READ;
#else
// POSIX flags
if (mode.find("r+") != std::string::npos) return O_RDWR;
if (mode.find('r') != std::string::npos) return O_RDONLY;
if (mode.find("w+") != std::string::npos) return O_RDWR | O_CREAT | O_TRUNC;
if (mode.find('w') != std::string::npos) return O_WRONLY | O_CREAT | O_TRUNC;
if (mode.find("a+") != std::string::npos) return O_RDWR | O_CREAT | O_APPEND;
if (mode.find('a') != std::string::npos) return O_WRONLY | O_CREAT | O_APPEND;
return O_RDONLY;
#endif
}
std::shared_ptr<ObjectValue> make_file_exports(EnvPtr env) {
auto obj = std::make_shared<ObjectValue>();
// file.open(path, mode="r") -> FileValue
{
    auto fn = [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "file.open requires path argument", token.loc);
        }
        
        std::string path;
        if (std::holds_alternative<std::string>(args[0])) {
            path = std::get<std::string>(args[0]);
        } else {
            throw SwaziError("TypeError", "file.open path must be string", token.loc);
        }
        
        std::string mode = "r";
        if (args.size() >= 2 && std::holds_alternative<std::string>(args[1])) {
            mode = std::get<std::string>(args[1]);
        }
        
        // Validate mode
        if (mode != "r" && mode != "w" && mode != "a" && 
            mode != "r+" && mode != "w+" && mode != "a+" &&
            mode != "rb" && mode != "wb" && mode != "ab" &&
            mode != "rb+" && mode != "wb+" && mode != "ab+") {
            throw SwaziError("ValueError", 
                "Invalid mode '" + mode + "'. Valid modes: r, w, a, r+, w+, a+ (append b for binary)",
                token.loc);
        }
        
        auto file = std::make_shared<FileValue>();
        file->path = path;
        file->mode = mode;
        
        bool binary = false;
        int flags = parse_mode_flags(mode, binary);
        file->is_binary = binary;
        
        #ifdef _WIN32
        DWORD creation = (mode.find('w') != std::string::npos) ? CREATE_ALWAYS :
                        (mode.find('a') != std::string::npos) ? OPEN_ALWAYS : OPEN_EXISTING;
        
        file->handle = CreateFileA(
            path.c_str(),
            flags,
            FILE_SHARE_READ,
            nullptr,
            creation,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        
        if (file->handle == INVALID_HANDLE_VALUE) {
            throw SwaziError("IOError", 
                "Failed to open file: " + path + " (error " + std::to_string(GetLastError()) + ")",
                token.loc);
        }
        
        // Seek to end if append mode
        if (mode.find('a') != std::string::npos) {
            SetFilePointer((HANDLE)file->handle, 0, nullptr, FILE_END);
        }
        #else
        file->fd = ::open(path.c_str(), flags, 0644);
        if (file->fd < 0) {
            throw SwaziError("IOError", 
                "Failed to open file: " + path + " (" + std::strerror(errno) + ")",
                token.loc);
        }
        #endif
        
        file->is_open = true;
        return Value{file};
    };
    
    auto fn_value = std::make_shared<FunctionValue>("file.open", fn, env, Token());
    obj->properties["open"] = PropertyDescriptor{fn_value, false, false, true, Token()};
}

return obj;
}