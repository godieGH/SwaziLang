#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

struct ListenerEntry {
    size_t id;
    FunctionPtr callback;
};
static std::atomic<size_t> g_next_listener_id{0};

static uv_tty_t* g_stdin_handle = nullptr;
static uv_tty_t* g_stdout_handle = nullptr;
static std::vector<ListenerEntry> g_resize_listeners;
static uv_signal_t* g_sigwinch_handle = nullptr;
static std::mutex g_stdin_mutex;
static std::vector<ListenerEntry> g_data_listeners;
static std::vector<ListenerEntry> g_eof_listeners;
static std::vector<ListenerEntry> g_sigint_listeners;
static std::atomic<bool> g_stdin_initialized(false);
static std::atomic<bool> g_stdin_closed(false);
static std::atomic<bool> g_raw_mode(false);
static std::atomic<bool> g_paused(false);
static std::atomic<bool> g_discard_on_pause(false);
static std::atomic<bool> g_discard_on_resume(false);

static uv_async_t* g_pause_keepalive = nullptr;
static std::string g_line_buffer;

// Escape sequence buffering
static std::string g_escape_buffer;
static uv_timer_t* g_escape_timer = nullptr;
static const int ESCAPE_TIMEOUT_MS = 10;

// Prompt state
static std::string g_current_prompt;
static std::atomic<bool> g_prompt_active(false);

static void stdin_alloc_cb(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested);
    buf->len = static_cast<unsigned int>(suggested);
}

// Helper: write prompt to stdout
static void write_prompt(const std::string& prompt) {
    if (prompt.empty()) return;

    // Write directly to stdout (fd 1)
    // In production, you might want to use uv_write for async, but for prompts sync is fine
    std::cout << prompt << std::flush;
}

// Helper: enqueue data callbacks
static void enqueue_data_callbacks_from_bytes(const char* data, ssize_t len) {
    if (len <= 0 || g_stdin_closed.load()) return;

    auto buffer = std::make_shared<BufferValue>();
    buffer->data.assign(data, data + len);
    buffer->encoding = "binary";

    std::vector<ListenerEntry> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_data_listeners;
    }
    for (auto& entry : listeners) {
        if (!entry.callback) continue;
        CallbackPayload* p = new CallbackPayload(entry.callback, {Value{buffer}});
        enqueue_callback_global(static_cast<void*>(p));
    }
}

static void enqueue_eof_callbacks() {
    std::vector<ListenerEntry> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_eof_listeners;
    }

    for (auto& entry : listeners) {
        if (!entry.callback) continue;
        CallbackPayload* p = new CallbackPayload(entry.callback, {});
        enqueue_callback_global(static_cast<void*>(p));
    }
}

static void enqueue_sigint_callbacks() {
    std::vector<ListenerEntry> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_sigint_listeners;
    }

    for (auto& entry : listeners) {
        if (!entry.callback) continue;
        CallbackPayload* p = new CallbackPayload(entry.callback, {});
        enqueue_callback_global(static_cast<void*>(p));
    }
}

static void create_pause_keepalive(uv_loop_t* loop) {
    if (g_pause_keepalive) return;
    g_pause_keepalive = new uv_async_t;
    uv_async_init(loop, g_pause_keepalive, [](uv_async_t* h) {
        (void)h;
    });
}

static void destroy_pause_keepalive(uv_loop_t* loop) {
    if (!g_pause_keepalive) return;
    uv_close((uv_handle_t*)g_pause_keepalive, [](uv_handle_t* h) {
        delete (uv_async_t*)h;
    });
    g_pause_keepalive = nullptr;
}

// Timer callback to flush incomplete escape sequence
static void escape_timeout_cb(uv_timer_t* handle) {
    if (!g_escape_buffer.empty()) {
        enqueue_data_callbacks_from_bytes(g_escape_buffer.c_str(), g_escape_buffer.length());
        g_escape_buffer.clear();
    }
}

// Helper to check if we're in the middle of an escape sequence
static bool is_potential_escape_sequence(const std::string& buf) {
    if (buf.empty()) return false;
    if (buf[0] != '\x1b') return false;

    size_t len = buf.length();

    // Just ESC - wait for more
    if (len == 1) return true;

    // ESC [ ... - CSI sequence
    if (len >= 2 && buf[1] == '[') {
        // CSI sequences end with a letter (A-Z, a-z) or ~
        char last = buf[len - 1];
        if ((last >= 'A' && last <= 'Z') || (last >= 'a' && last <= 'z') || last == '~') {
            return false;  // Complete sequence
        }
        return true;  // Still building
    }

    // ESC O ... - SS3 sequence (function keys F1-F4)
    if (len >= 2 && buf[1] == 'O') {
        if (len == 2) return true;  // Wait for final character
        char last = buf[len - 1];
        if (last >= 'A' && last <= 'Z') {
            return false;  // Complete
        }
        return true;
    }

    // Other ESC sequences are typically 2 characters
    if (len >= 2) return false;

    return true;
}

// Main read callback
static void stdin_read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    if (g_stdin_closed.load()) {
        if (buf && buf->base) free(buf->base);
        return;
    }

    if (g_paused.load()) {
        if (buf && buf->base) free(buf->base);
        return;
    }

    if (nread > 0) {
        const char* data = buf->base;
        ssize_t len = nread;

        if (g_discard_on_resume.load()) {
            g_discard_on_resume.store(false);
            if (buf && buf->base) free(buf->base);
            return;
        }

        if (g_raw_mode.load()) {
            for (ssize_t i = 0; i < len; ++i) {
                unsigned char ch = static_cast<unsigned char>(data[i]);

                // Handle Ctrl+C - flush any pending escape sequence first
                if (ch == 0x03) {
                    if (!g_escape_buffer.empty()) {
                        enqueue_data_callbacks_from_bytes(g_escape_buffer.c_str(), g_escape_buffer.length());
                        g_escape_buffer.clear();
                        if (g_escape_timer) {
                            uv_timer_stop(g_escape_timer);
                        }
                    }
                    enqueue_sigint_callbacks();
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                    continue;
                }

                // Handle Ctrl+D - flush any pending escape sequence first
                if (ch == 0x04) {
                    if (!g_escape_buffer.empty()) {
                        enqueue_data_callbacks_from_bytes(g_escape_buffer.c_str(), g_escape_buffer.length());
                        g_escape_buffer.clear();
                        if (g_escape_timer) {
                            uv_timer_stop(g_escape_timer);
                        }
                    }
                    enqueue_eof_callbacks();
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                    continue;
                }

                // Check if this starts or continues an escape sequence
                if (ch == 0x1b || !g_escape_buffer.empty()) {
                    g_escape_buffer.push_back(static_cast<char>(ch));

                    // Check if sequence is complete
                    if (!is_potential_escape_sequence(g_escape_buffer)) {
                        // Complete sequence - send it as one unit
                        enqueue_data_callbacks_from_bytes(g_escape_buffer.c_str(), g_escape_buffer.length());
                        g_escape_buffer.clear();
                        if (g_escape_timer) {
                            uv_timer_stop(g_escape_timer);
                        }
                    } else {
                        // Incomplete sequence - start/reset timer
                        uv_loop_t* loop = scheduler_get_loop();
                        if (!g_escape_timer && loop) {
                            g_escape_timer = new uv_timer_t;
                            uv_timer_init(loop, g_escape_timer);
                        }
                        if (g_escape_timer) {
                            uv_timer_stop(g_escape_timer);
                            uv_timer_start(g_escape_timer, escape_timeout_cb, ESCAPE_TIMEOUT_MS, 0);
                        }
                    }
                } else {
                    // Regular character - send immediately
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                }
            }
        } else {
            for (ssize_t i = 0; i < len; ++i) {
                unsigned char ch = static_cast<unsigned char>(data[i]);

                if (ch == 0x03) {
                    enqueue_sigint_callbacks();
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                    continue;
                }

                if (ch == 0x04) {
                    enqueue_eof_callbacks();
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                    continue;
                }

                if (ch >= 0x01 && ch <= 0x1F && ch != '\n' && ch != '\r') {
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                    continue;
                }

                g_line_buffer.push_back(static_cast<char>(ch));

                if (ch == '\n') {
                    std::string line;
                    if (!g_line_buffer.empty()) {
                        line = g_line_buffer.substr(0, g_line_buffer.length() - 1);
                    }
                    g_line_buffer.clear();

                    auto buffer = std::make_shared<BufferValue>();
                    buffer->data.assign(line.begin(), line.end());
                    buffer->encoding = "utf8";

                    std::vector<ListenerEntry> listeners;
                    {
                        std::lock_guard<std::mutex> lk(g_stdin_mutex);
                        listeners = g_data_listeners;
                    }
                    for (auto& entry : listeners) {
                        if (!entry.callback) continue;
                        CallbackPayload* p = new CallbackPayload(entry.callback, {Value{buffer}});
                        enqueue_callback_global(static_cast<void*>(p));
                    }

                    // NEW: Re-display prompt after line is processed
                    // if (g_prompt_active.load() && !g_current_prompt.empty()) {
                    // write_prompt(g_current_prompt);
                    //}
                }
            }
        }

    } else if (nread < 0) {
        if (nread == UV_EOF) {
            enqueue_eof_callbacks();
        } else {
            enqueue_eof_callbacks();
        }
    }

    if (buf && buf->base) free(buf->base);
}

static void sigwinch_cb(uv_signal_t* handle, int signum) {
    (void)handle;
    (void)signum;

    if (g_stdin_closed.load() || !g_stdin_initialized.load()) return;  // Add initialized check

    // Get new terminal size
    int width = 80, height = 24;
    if (g_stdin_handle) {
        uv_tty_get_winsize(g_stdin_handle, &width, &height);
    }

    // Create size object
    auto size_obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<stdin>", 0, 0, 0);
    size_obj->properties["width"] = {Value{static_cast<double>(width)}, false, false, true, tok};
    size_obj->properties["height"] = {Value{static_cast<double>(height)}, false, false, true, tok};

    // Enqueue callbacks
    std::vector<ListenerEntry> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_resize_listeners;
    }

    for (auto& entry : listeners) {
        if (!entry.callback) continue;
        CallbackPayload* p = new CallbackPayload(entry.callback, {Value{size_obj}});
        enqueue_callback_global(static_cast<void*>(p));
    }
}

// Global signal handler flag
static bool g_signal_handlers_installed = false;

// Signal handler function
static void cleanup_on_signal(int signum) {
    // Reset terminal immediately
    std::cout << "\x1b[?25h" << std::flush;  // Show cursor
    std::cout << "\x1b[0m" << std::flush;    // Reset colors
    std::cout << "\n"
              << std::flush;  // New line for clean exit

    if (g_stdin_handle) {
        uv_tty_set_mode(g_stdin_handle, UV_TTY_MODE_NORMAL);
    }

    // Clear state
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        g_line_buffer.clear();
        g_escape_buffer.clear();
    }

    // Re-raise signal for default handling
    signal(signum, SIG_DFL);
    raise(signum);
}

std::shared_ptr<ObjectValue> make_stdin_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<stdin>", 0, 0, 0);

    auto ensure_init = [&tok]() -> void {
        if (g_stdin_initialized.load()) return;

        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            throw SwaziError("RuntimeError", "stdin requires event loop", tok.loc);
        }

        // Install signal handlers ONCE
        if (!g_signal_handlers_installed) {
            signal(SIGINT, cleanup_on_signal);
            signal(SIGTERM, cleanup_on_signal);
            signal(SIGABRT, cleanup_on_signal);
#ifndef _WIN32
            signal(SIGQUIT, cleanup_on_signal);
#endif

            std::atexit([]() {
                std::cout << "\x1b[?25h" << std::flush;
                std::cout << "\n"
                          << std::flush;
                if (g_stdin_handle) {
                    uv_tty_set_mode(g_stdin_handle, UV_TTY_MODE_NORMAL);
                }
            });
            g_signal_handlers_installed = true;
        }

        scheduler_run_on_loop([loop]() {
            if (g_stdin_initialized.load()) return;

            g_stdin_handle = new uv_tty_t;
            int r = uv_tty_init(loop, g_stdin_handle, 0, 1);
            if (r != 0) {
                delete g_stdin_handle;
                g_stdin_handle = nullptr;
                return;
            }

            uv_tty_set_mode(g_stdin_handle, g_raw_mode.load() ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
            uv_read_start((uv_stream_t*)g_stdin_handle, stdin_alloc_cb, stdin_read_cb);

#ifndef _WIN32
            // Set up SIGWINCH handler for terminal resize
            if (!g_sigwinch_handle) {
                g_sigwinch_handle = new uv_signal_t;
                uv_signal_init(loop, g_sigwinch_handle);
                uv_signal_start(g_sigwinch_handle, sigwinch_cb, SIGWINCH);
            }
#endif

            g_stdin_initialized.store(true);
            g_stdin_closed.store(false);
            g_paused.store(false);
        });
    };
    // stdin.on(event, callback)
    auto on_impl = [ensure_init](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "stdin.on requires (event, callback)", token.loc);
        }
        if (!std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", "event must be string", token.loc);
        }
        if (!std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "callback must be function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr cb = std::get<FunctionPtr>(args[1]);

        std::lock_guard<std::mutex> lk(g_stdin_mutex);

        if (event == "data") {
            ListenerEntry entry;
            entry.id = g_next_listener_id.fetch_add(1);
            entry.callback = cb;
            g_data_listeners.push_back(entry);
            ensure_init();
            return Value{static_cast<double>(entry.id)};
        } else if (event == "eof") {
            ListenerEntry entry;
            entry.id = g_next_listener_id.fetch_add(1);
            entry.callback = cb;
            g_eof_listeners.push_back(entry);
            ensure_init();
            return Value{static_cast<double>(entry.id)};
        } else if (event == "sigint") {
            ListenerEntry entry;
            entry.id = g_next_listener_id.fetch_add(1);
            entry.callback = cb;
            g_sigint_listeners.push_back(entry);
            ensure_init();
            return Value{static_cast<double>(entry.id)};
        } else if (event == "resize") {
            ListenerEntry entry;
            entry.id = g_next_listener_id.fetch_add(1);
            entry.callback = cb;
            g_resize_listeners.push_back(entry);
            ensure_init();
            return Value{static_cast<double>(entry.id)};
        } else {
            throw SwaziError("TypeError",
                "stdin.on unknown event. Valid: data, eof, sigint, resize", token.loc);
        }

        return Value{};
    };
    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stdin.on", on_impl, env, tok)},
        false, false, true, tok};

    // stdin.pipe(writable) - pipe stdin to a writable stream
    auto pipe_impl = [](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stdin.pipe(writable) requires a writable stream", token.loc);
        }

        if (!std::holds_alternative<ObjectPtr>(args[0])) {
            throw SwaziError("TypeError", "pipe destination must be a writable stream object", token.loc);
        }

        ObjectPtr dest_obj = std::get<ObjectPtr>(args[0]);

        // Get the write function from destination
        auto write_it = dest_obj->properties.find("write");
        if (write_it == dest_obj->properties.end()) {
            throw SwaziError("TypeError", "destination must have a write() method", token.loc);
        }

        if (!std::holds_alternative<FunctionPtr>(write_it->second.value)) {
            throw SwaziError("TypeError", "destination.write must be a function", token.loc);
        }

        FunctionPtr write_fn = std::get<FunctionPtr>(write_it->second.value);

        // Create data handler that writes to destination
        Token evt_tok{};
        evt_tok.loc = TokenLocation("<stdin-pipe>", 0, 0, 0);

        auto data_handler = [dest_obj, write_fn, env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) return std::monostate{};

            // Call destination's write() method
            if (write_fn->is_native && write_fn->native_impl) {
                try {
                    Value result = write_fn->native_impl({args[0]}, env, token);

                    // Handle backpressure: if write returns false, pause stdin
                    if (std::holds_alternative<bool>(result) && !std::get<bool>(result)) {
                        // Destination is full, pause reading
                        if (!g_paused.load()) {
                            g_paused.store(true);
                            scheduler_run_on_loop([]() {
                                uv_loop_t* loop = scheduler_get_loop();
                                if (!loop) return;
                                create_pause_keepalive(loop);
                                if (g_stdin_handle) {
                                    uv_read_stop((uv_stream_t*)g_stdin_handle);
                                }
                            });
                        }
                    }

                    return result;
                } catch (...) {
                    return Value{false};
                }
            }

            return Value{false};
        };

        auto data_fn = std::make_shared<FunctionValue>("stdin.pipe.data", data_handler, nullptr, evt_tok);

        // Register data listener
        {
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            ListenerEntry entry;
            entry.id = g_next_listener_id.fetch_add(1);
            entry.callback = data_fn;
            g_data_listeners.push_back(entry);
        }

        // Set up drain listener to resume stdin
        auto on_it = dest_obj->properties.find("on");
        if (on_it != dest_obj->properties.end() && std::holds_alternative<FunctionPtr>(on_it->second.value)) {
            FunctionPtr on_fn = std::get<FunctionPtr>(on_it->second.value);

            auto drain_handler = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                // Resume stdin when destination drains
                if (g_paused.load()) {
                    g_paused.store(false);
                    g_discard_on_resume.store(true);

                    scheduler_run_on_loop([]() {
                        if (g_stdin_handle) {
                            {
                                std::lock_guard<std::mutex> lk(g_stdin_mutex);
                                g_line_buffer.clear();
                            }
                            uv_read_start((uv_stream_t*)g_stdin_handle, stdin_alloc_cb, stdin_read_cb);
                        }

                        uv_loop_t* loop = scheduler_get_loop();
                        if (loop) destroy_pause_keepalive(loop);
                    });
                }
                return std::monostate{};
            };

            auto drain_fn = std::make_shared<FunctionValue>("stdin.pipe.drain", drain_handler, nullptr, evt_tok);

            if (on_fn->is_native && on_fn->native_impl) {
                try {
                    on_fn->native_impl({Value{std::string("drain")}, Value{drain_fn}}, nullptr, evt_tok);
                } catch (...) {}
            }
        }

        // Ensure stdin is initialized and start flowing
        if (!g_stdin_initialized.load()) {
            uv_loop_t* loop = scheduler_get_loop();
            if (!loop) {
                throw SwaziError("RuntimeError", "stdin requires event loop", token.loc);
            }

            scheduler_run_on_loop([loop]() {
                if (g_stdin_initialized.load()) return;

                g_stdin_handle = new uv_tty_t;
                int r = uv_tty_init(loop, g_stdin_handle, 0, 1);
                if (r != 0) {
                    delete g_stdin_handle;
                    g_stdin_handle = nullptr;
                    return;
                }

                uv_tty_set_mode(g_stdin_handle, g_raw_mode.load() ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
                uv_read_start((uv_stream_t*)g_stdin_handle, stdin_alloc_cb, stdin_read_cb);

                g_stdin_initialized.store(true);
                g_stdin_closed.store(false);
                g_paused.store(false);
            });
        } else if (g_paused.load()) {
            // Resume if paused
            g_paused.store(false);
            scheduler_run_on_loop([]() {
                if (g_stdin_handle) {
                    uv_read_start((uv_stream_t*)g_stdin_handle, stdin_alloc_cb, stdin_read_cb);
                }
            });
        }

        return Value{dest_obj};
    };

    obj->properties["pipe"] = {
        Value{std::make_shared<FunctionValue>("stdin.pipe", pipe_impl, env, tok)},
        false, false, true, tok};

    auto once_impl = [ensure_init](const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "stdin.once requires (event, callback)", token.loc);
        }
        if (!std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", "event must be string", token.loc);
        }
        if (!std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "callback must be function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr user_callback = std::get<FunctionPtr>(args[1]);

        std::lock_guard<std::mutex> lk(g_stdin_mutex);

        ListenerEntry entry;
        entry.id = g_next_listener_id.fetch_add(1);
        size_t listener_id = entry.id;

        // Create a self-removing wrapper that calls user callback then removes itself
        auto self_removing_impl = [event, user_callback, listener_id](
                                      const std::vector<Value>& cb_args, EnvPtr cb_env, const Token& cb_token) -> Value {
            // First enqueue the user's callback
            CallbackPayload* user_payload = new CallbackPayload(user_callback, cb_args);
            enqueue_callback_global(static_cast<void*>(user_payload));

            // Then remove ourselves from the listener list
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            auto remove_by_id = [listener_id](std::vector<ListenerEntry>& listeners) {
                listeners.erase(
                    std::remove_if(listeners.begin(), listeners.end(),
                        [listener_id](const ListenerEntry& e) {
                            return e.id == listener_id;
                        }),
                    listeners.end());
            };

            if (event == "data") {
                remove_by_id(g_data_listeners);
            } else if (event == "eof") {
                remove_by_id(g_eof_listeners);
            } else if (event == "sigint") {
                remove_by_id(g_sigint_listeners);
            } else if (event == "resize") {
                remove_by_id(g_resize_listeners);
            }

            return std::monostate{};
        };

        entry.callback = std::make_shared<FunctionValue>(
            "stdin.once_wrapper", self_removing_impl, env, token);

        if (event == "data") {
            g_data_listeners.push_back(entry);
        } else if (event == "eof") {
            g_eof_listeners.push_back(entry);
        } else if (event == "sigint") {
            g_sigint_listeners.push_back(entry);
        } else if (event == "resize") {
            g_resize_listeners.push_back(entry);
        } else {
            throw SwaziError("TypeError",
                "stdin.once unknown event. Valid: data, eof, sigint, resize", token.loc);
        }

        ensure_init();
        return Value{static_cast<double>(listener_id)};
    };
    obj->properties["once"] = {
        Value{std::make_shared<FunctionValue>("stdin.once", once_impl, env, tok)},
        false, false, true, tok};

    // stdin.removeListener(event, callback) - removes all instances of callback for that event
    auto removeListener_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "stdin.removeListener requires (event, callback)", token.loc);
        }
        if (!std::holds_alternative<std::string>(args[0])) {
            throw SwaziError("TypeError", "event must be string", token.loc);
        }
        if (!std::holds_alternative<FunctionPtr>(args[1])) {
            throw SwaziError("TypeError", "callback must be function", token.loc);
        }

        std::string event = std::get<std::string>(args[0]);
        FunctionPtr cb = std::get<FunctionPtr>(args[1]);

        std::lock_guard<std::mutex> lk(g_stdin_mutex);

        if (event == "data") {
            auto& listeners = g_data_listeners;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&cb](const ListenerEntry& entry) {
                        return entry.callback.get() == cb.get();
                    }),
                listeners.end());
        } else if (event == "eof") {
            auto& listeners = g_eof_listeners;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&cb](const ListenerEntry& entry) {
                        return entry.callback.get() == cb.get();
                    }),
                listeners.end());
        } else if (event == "sigint") {
            auto& listeners = g_sigint_listeners;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&cb](const ListenerEntry& entry) {
                        return entry.callback.get() == cb.get();
                    }),
                listeners.end());
        } else if (event == "resize") {
            auto& listeners = g_resize_listeners;
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&cb](const ListenerEntry& entry) {
                        return entry.callback.get() == cb.get();
                    }),
                listeners.end());
        } else {
            throw SwaziError("TypeError",
                "stdin.removeListener unknown event. Valid: data, eof, sigint, resize", token.loc);
        }

        return std::monostate{};
    };
    obj->properties["removeListener"] = {
        Value{std::make_shared<FunctionValue>("stdin.removeListener", removeListener_impl, env, tok)},
        false, false, true, tok};

    // stdin.removeListenerById(id) or stdin.removeListenerById([id1, id2, ...])
    auto removeListenerById_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stdin.removeListenerById requires (id) or ([ids])", token.loc);
        }

        std::vector<size_t> ids_to_remove;

        // Check if it's an array of IDs
        if (std::holds_alternative<std::shared_ptr<ArrayValue>>(args[0])) {
            auto arr = std::get<std::shared_ptr<ArrayValue>>(args[0]);
            for (const auto& elem : arr->elements) {
                if (!std::holds_alternative<double>(elem)) {
                    throw SwaziError("TypeError", "all array elements must be numbers (IDs)", token.loc);
                }
                ids_to_remove.push_back(static_cast<size_t>(std::get<double>(elem)));
            }
        } else if (std::holds_alternative<double>(args[0])) {
            // Single ID
            ids_to_remove.push_back(static_cast<size_t>(std::get<double>(args[0])));
        } else {
            throw SwaziError("TypeError", "id must be number or array of numbers", token.loc);
        }

        std::lock_guard<std::mutex> lk(g_stdin_mutex);

        // Helper lambda to remove by IDs from a listener vector
        auto remove_by_ids = [&ids_to_remove](std::vector<ListenerEntry>& listeners) {
            listeners.erase(
                std::remove_if(listeners.begin(), listeners.end(),
                    [&ids_to_remove](const ListenerEntry& entry) {
                        return std::find(ids_to_remove.begin(), ids_to_remove.end(), entry.id) != ids_to_remove.end();
                    }),
                listeners.end());
        };

        // Remove from all listener arrays
        remove_by_ids(g_data_listeners);
        remove_by_ids(g_eof_listeners);
        remove_by_ids(g_sigint_listeners);
        remove_by_ids(g_resize_listeners);

        return std::monostate{};
    };
    obj->properties["removeListenerById"] = {
        Value{std::make_shared<FunctionValue>("stdin.removeListenerById", removeListenerById_impl, env, tok)},
        false, false, true, tok};

    // stdin.removeAllListeners() - removes all listeners from all events
    // stdin.removeAllListeners(event) - removes all listeners from specific event
    // stdin.removeAllListeners([event1, event2, ...]) - removes all listeners from specified events
    auto removeAllListeners_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);

        if (args.empty()) {
            // Remove ALL listeners from ALL events
            g_data_listeners.clear();
            g_eof_listeners.clear();
            g_sigint_listeners.clear();
            g_resize_listeners.clear();
            return std::monostate{};
        }

        std::vector<std::string> events_to_clear;

        // Check if it's an array of events
        if (std::holds_alternative<std::shared_ptr<ArrayValue>>(args[0])) {
            auto arr = std::get<std::shared_ptr<ArrayValue>>(args[0]);
            for (const auto& elem : arr->elements) {
                if (!std::holds_alternative<std::string>(elem)) {
                    throw SwaziError("TypeError", "all array elements must be event names (strings)", token.loc);
                }
                events_to_clear.push_back(std::get<std::string>(elem));
            }
        } else if (std::holds_alternative<std::string>(args[0])) {
            // Single event
            events_to_clear.push_back(std::get<std::string>(args[0]));
        } else {
            throw SwaziError("TypeError", "event must be string or array of strings", token.loc);
        }

        // Clear listeners for each specified event
        for (const auto& event : events_to_clear) {
            if (event == "data") {
                g_data_listeners.clear();
            } else if (event == "eof") {
                g_eof_listeners.clear();
            } else if (event == "sigint") {
                g_sigint_listeners.clear();
            } else if (event == "resize") {
                g_resize_listeners.clear();
            } else {
                throw SwaziError("TypeError",
                    "Unknown event '" + event + "'. Valid: data, eof, sigint, resize", token.loc);
            }
        }

        return std::monostate{};
    };
    obj->properties["removeAllListeners"] = {
        Value{std::make_shared<FunctionValue>("stdin.removeAllListeners", removeAllListeners_impl, env, tok)},
        false, false, true, tok};

    // stdin.prompt(text) - sets and displays a prompt
    auto prompt_impl = [ensure_init](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        std::string prompt_text;

        if (!args.empty()) {
            if (std::holds_alternative<std::string>(args[0])) {
                prompt_text = std::get<std::string>(args[0]);
            } else {
                throw SwaziError("TypeError", "prompt must be a string", token.loc);
            }
        }

        ensure_init();

        {
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            g_current_prompt = prompt_text;
            g_prompt_active.store(!prompt_text.empty());
        }

        // Display the prompt immediately
        if (!prompt_text.empty()) {
            write_prompt(prompt_text);
        }

        return std::monostate{};
    };
    obj->properties["prompt"] = {
        Value{std::make_shared<FunctionValue>("stdin.prompt", prompt_impl, env, tok)},
        false, false, true, tok};

    // stdin.pause()
    auto pause_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!g_stdin_initialized.load() || g_stdin_closed.load()) return std::monostate{};

        if (!g_paused.load()) {
            g_paused.store(true);
            g_prompt_active.store(false);
            scheduler_run_on_loop([]() {
                uv_loop_t* loop = scheduler_get_loop();
                if (!loop) return;
                create_pause_keepalive(loop);
                if (g_stdin_handle) {
                    uv_read_stop((uv_stream_t*)g_stdin_handle);
                }
            });
        }
        return std::monostate{};
    };
    obj->properties["pause"] = {
        Value{std::make_shared<FunctionValue>("stdin.pause", pause_impl, env, tok)},
        false, false, true, tok};

    // stdin.resume()
    auto resume_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!g_stdin_initialized.load() || g_stdin_closed.load()) return std::monostate{};

        if (g_paused.load()) {
            g_paused.store(false);
            g_discard_on_resume.store(true);

            scheduler_run_on_loop([]() {
                if (g_stdin_handle) {
                    {
                        std::lock_guard<std::mutex> lk(g_stdin_mutex);
                        g_line_buffer.clear();
                    }
                    uv_read_start((uv_stream_t*)g_stdin_handle, stdin_alloc_cb, stdin_read_cb);

                    if (!g_current_prompt.empty()) {
                        g_prompt_active.store(true);
                        write_prompt(g_current_prompt);
                    }
                }

                uv_loop_t* loop = scheduler_get_loop();
                if (loop) destroy_pause_keepalive(loop);
            });
        }
        return std::monostate{};
    };
    obj->properties["resume"] = {
        Value{std::make_shared<FunctionValue>("stdin.resume", resume_impl, env, tok)},
        false, false, true, tok};

    // stdin.setRawMode(enabled)
    auto setRaw_impl = [ensure_init](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        bool enabled = true;
        if (!args.empty() && std::holds_alternative<bool>(args[0])) {
            enabled = std::get<bool>(args[0]);
        }

        // Clear buffers when changing modes
        {
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            if (enabled && !g_raw_mode.load()) {
                // Switching TO raw mode
                g_line_buffer.clear();
                g_prompt_active.store(false);
            } else if (!enabled && g_raw_mode.load()) {
                // Switching FROM raw mode TO normal mode
                g_escape_buffer.clear();
                if (g_escape_timer) {
                    scheduler_run_on_loop([]() {
                        if (g_escape_timer) {
                            uv_timer_stop(g_escape_timer);
                        }
                    });
                }
            }
        }

        g_raw_mode.store(enabled);

        if (g_stdin_initialized.load() && g_stdin_handle) {
            scheduler_run_on_loop([enabled]() {
                if (g_stdin_handle) {
                    uv_tty_set_mode(g_stdin_handle,
                        enabled ? UV_TTY_MODE_RAW : UV_TTY_MODE_NORMAL);
                }
            });
        }

        return std::monostate{};
    };
    obj->properties["setRawMode"] = {
        Value{std::make_shared<FunctionValue>("stdin.setRawMode", setRaw_impl, env, tok)},
        false, false, true, tok};

    // stdin.setDiscardOnPause(enabled)
    auto setDiscard_impl = [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        bool enabled = true;
        if (!args.empty() && std::holds_alternative<bool>(args[0])) {
            enabled = std::get<bool>(args[0]);
        }
        g_discard_on_pause.store(enabled);
        return std::monostate{};
    };
    obj->properties["setDiscardOnPause"] = {
        Value{std::make_shared<FunctionValue>("stdin.setDiscardOnPause", setDiscard_impl, env, tok)},
        false, false, true, tok};

    // stdin.close()
    auto close_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        if (!g_stdin_initialized.load() || g_stdin_closed.load()) return std::monostate{};

        g_stdin_closed.store(true);
        g_prompt_active.store(false);

        scheduler_run_on_loop([]() {
            uv_loop_t* loop = scheduler_get_loop();

            if (g_stdin_handle) {
                uv_tty_set_mode(g_stdin_handle, UV_TTY_MODE_NORMAL);
                uv_read_stop((uv_stream_t*)g_stdin_handle);
                uv_close((uv_handle_t*)g_stdin_handle, [](uv_handle_t* h) {
                    delete (uv_tty_t*)h;
                });
                g_stdin_handle = nullptr;
            }

            {
                std::lock_guard<std::mutex> lk(g_stdin_mutex);
                g_data_listeners.clear();
                g_eof_listeners.clear();
                g_sigint_listeners.clear();
                g_resize_listeners.clear();  // Clear resize listeners too
            }

            if (loop) destroy_pause_keepalive(loop);

#ifndef _WIN32
            // Clean up resize signal handler
            if (g_sigwinch_handle) {
                uv_signal_stop(g_sigwinch_handle);
                uv_close((uv_handle_t*)g_sigwinch_handle, [](uv_handle_t* h) {
                    delete (uv_signal_t*)h;
                });
                g_sigwinch_handle = nullptr;
            }
#endif
            // Clean up escape timer
            if (g_escape_timer) {
                uv_timer_stop(g_escape_timer);
                uv_close((uv_handle_t*)g_escape_timer, [](uv_handle_t* h) {
                    delete (uv_timer_t*)h;
                });
                g_escape_timer = nullptr;
            }
            g_escape_buffer.clear();

            g_stdin_initialized.store(false);
        });

        return std::monostate{};
    };
    obj->properties["close"] = {
        Value{std::make_shared<FunctionValue>("stdin.close", close_impl, env, tok)},
        false, false, true, tok};

    auto echo_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.empty()) {
            throw SwaziError("TypeError", "stdin.echo requires a string argument", token.loc);
        }

        std::string text;
        if (std::holds_alternative<std::string>(args[0])) {
            text = std::get<std::string>(args[0]);
        } else if (auto buf = std::get_if<std::shared_ptr<BufferValue>>(&args[0])) {
            const auto& data = (*buf)->data;
            text = std::string(data.begin(), data.end());
        } else {
            throw SwaziError("TypeError", "stdin.echo requires string or buffer", token.loc);
        }

        std::cout << text << std::flush;
        return std::monostate{};
    };
    obj->properties["echo"] = {
        Value{std::make_shared<FunctionValue>("stdin.echo", echo_impl, env, tok)},
        false, false, true, tok};

    // ============================================================================
    // TERMINAL SIZE
    // ============================================================================

    // stdin.getTermSize() -> {width: number, height: number} | null
    auto getTermSize_impl = [](const std::vector<Value>&, EnvPtr, const Token& token) -> Value {
        uv_loop_t* loop = scheduler_get_loop();
        if (!loop) {
            throw SwaziError("RuntimeError", "stdin requires event loop", token.loc);
        }

        // Use existing handle if already initialized
        uv_tty_t* query_handle = g_stdin_handle;
        bool temp_handle = false;

        // If no handle exists, create a temporary one just for querying
        if (!query_handle) {
            query_handle = new uv_tty_t;
            int r = uv_tty_init(loop, query_handle, 0, 1);
            if (r != 0) {
                delete query_handle;
                return std::monostate{};
            }
            temp_handle = true;
        }

        int width = 80, height = 24;
        int result = uv_tty_get_winsize(query_handle, &width, &height);

        // Clean up temporary handle
        if (temp_handle) {
            uv_close((uv_handle_t*)query_handle, [](uv_handle_t* h) {
                delete (uv_tty_t*)h;
            });
        }

        if (result != 0) {
            return std::monostate{};
        }

        auto obj = std::make_shared<ObjectValue>();
        Token tok{};
        tok.loc = TokenLocation("<stdin>", 0, 0, 0);

        obj->properties["width"] = {Value{static_cast<double>(width)}, false, false, true, tok};
        obj->properties["height"] = {Value{static_cast<double>(height)}, false, false, true, tok};

        return Value{obj};
    };
    obj->properties["getTermSize"] = {
        Value{std::make_shared<FunctionValue>("stdin.getTermSize", getTermSize_impl, env, tok)},
        false, false, true, tok};

    // ============================================================================
    // CURSOR CONTROL
    // ============================================================================

    auto cursorTo_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "stdin.cursorTo requires (x, y)", token.loc);
        }

        if (!std::holds_alternative<double>(args[0]) || !std::holds_alternative<double>(args[1])) {
            throw SwaziError("TypeError", "x and y must be numbers", token.loc);
        }

        int x = static_cast<int>(std::get<double>(args[0]));
        int y = static_cast<int>(std::get<double>(args[1]));

        std::string seq = "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
        std::cout << seq << std::flush;

        return std::monostate{};
    };

    obj->properties["cursorTo"] = {
        Value{std::make_shared<FunctionValue>("stdin.cursorTo", cursorTo_impl, env, tok)},
        false, false, true, tok};

    auto cursorMove_impl = [](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
        if (args.size() < 2) {
            throw SwaziError("TypeError", "stdin.cursorMove requires (dx, dy)", token.loc);
        }

        if (!std::holds_alternative<double>(args[0]) || !std::holds_alternative<double>(args[1])) {
            throw SwaziError("TypeError", "dx and dy must be numbers", token.loc);
        }

        int dx = static_cast<int>(std::get<double>(args[0]));
        int dy = static_cast<int>(std::get<double>(args[1]));

        std::string seq;

        if (dy < 0) {
            seq += "\x1b[" + std::to_string(-dy) + "A";
        } else if (dy > 0) {
            seq += "\x1b[" + std::to_string(dy) + "B";
        }

        if (dx > 0) {
            seq += "\x1b[" + std::to_string(dx) + "C";
        } else if (dx < 0) {
            seq += "\x1b[" + std::to_string(-dx) + "D";
        }

        if (!seq.empty()) {
            std::cout << seq << std::flush;
        }

        return std::monostate{};
    };

    obj->properties["cursorMove"] = {
        Value{std::make_shared<FunctionValue>("stdin.cursorMove", cursorMove_impl, env, tok)},
        false, false, true, tok};

    auto saveCursor_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        std::cout << "\x1b[s" << std::flush;
        return std::monostate{};
    };

    obj->properties["saveCursor"] = {
        Value{std::make_shared<FunctionValue>("stdin.saveCursor", saveCursor_impl, env, tok)},
        false, false, true, tok};

    auto restoreCursor_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        std::cout << "\x1b[u" << std::flush;
        return std::monostate{};
    };

    obj->properties["restoreCursor"] = {
        Value{std::make_shared<FunctionValue>("stdin.restoreCursor", restoreCursor_impl, env, tok)},
        false, false, true, tok};

    auto hideCursor_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        std::cout << "\x1b[?25l" << std::flush;
        return std::monostate{};
    };

    obj->properties["hideCursor"] = {
        Value{std::make_shared<FunctionValue>("stdin.hideCursor", hideCursor_impl, env, tok)},
        false, false, true, tok};

    auto showCursor_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        std::cout << "\x1b[?25h" << std::flush;
        return std::monostate{};
    };

    obj->properties["showCursor"] = {
        Value{std::make_shared<FunctionValue>("stdin.showCursor", showCursor_impl, env, tok)},
        false, false, true, tok};

    // ============================================================================
    // SCREEN CONTROL
    // ============================================================================

    auto clearLine_impl = [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        int mode = 2;

        if (!args.empty() && std::holds_alternative<double>(args[0])) {
            mode = static_cast<int>(std::get<double>(args[0]));
        }

        std::string seq = "\x1b[" + std::to_string(mode) + "K";
        std::cout << seq << std::flush;

        return std::monostate{};
    };

    obj->properties["clearLine"] = {
        Value{std::make_shared<FunctionValue>("stdin.clearLine", clearLine_impl, env, tok)},
        false, false, true, tok};

    auto clearScreen_impl = [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        int mode = 2;

        if (!args.empty() && std::holds_alternative<double>(args[0])) {
            mode = static_cast<int>(std::get<double>(args[0]));
        }

        std::string seq = "\x1b[" + std::to_string(mode) + "J";
        std::cout << seq << std::flush;

        return std::monostate{};
    };

    obj->properties["clearScreen"] = {
        Value{std::make_shared<FunctionValue>("stdin.clearScreen", clearScreen_impl, env, tok)},
        false, false, true, tok};

    auto clearScreenDown_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        std::cout << "\x1b[J" << std::flush;
        return std::monostate{};
    };

    obj->properties["clearScreenDown"] = {
        Value{std::make_shared<FunctionValue>("stdin.clearScreenDown", clearScreenDown_impl, env, tok)},
        false, false, true, tok};

    // ============================================================================
    // SCROLLING
    // ============================================================================

    auto scrollUp_impl = [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        int lines = 1;

        if (!args.empty() && std::holds_alternative<double>(args[0])) {
            lines = static_cast<int>(std::get<double>(args[0]));
        }

        if (lines > 0) {
            std::string seq = "\x1b[" + std::to_string(lines) + "S";
            std::cout << seq << std::flush;
        }

        return std::monostate{};
    };

    obj->properties["scrollUp"] = {
        Value{std::make_shared<FunctionValue>("stdin.scrollUp", scrollUp_impl, env, tok)},
        false, false, true, tok};

    auto scrollDown_impl = [](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
        int lines = 1;

        if (!args.empty() && std::holds_alternative<double>(args[0])) {
            lines = static_cast<int>(std::get<double>(args[0]));
        }

        if (lines > 0) {
            std::string seq = "\x1b[" + std::to_string(lines) + "T";
            std::cout << seq << std::flush;
        }

        return std::monostate{};
    };

    obj->properties["scrollDown"] = {
        Value{std::make_shared<FunctionValue>("stdin.scrollDown", scrollDown_impl, env, tok)},
        false, false, true, tok};

    // ============================================================================
    // BEEP/BELL
    // ============================================================================

    auto beep_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        std::cout << "\x07" << std::flush;
        return std::monostate{};
    };

    obj->properties["beep"] = {
        Value{std::make_shared<FunctionValue>("stdin.beep", beep_impl, env, tok)},
        false, false, true, tok};

    // ============================================================================
    // some other tools
    // ============================================================================

    auto isTTY_impl = [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        return Value{uv_guess_handle(0) == UV_TTY};
    };
    obj->properties["isTTY"] = {
        Value{std::make_shared<FunctionValue>("stdin.isTTY", isTTY_impl, env, tok)},
        false, false, true, tok};

    return obj;
}