#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

static uv_tty_t* g_stdin_handle = nullptr;
static uv_tty_t* g_stdout_handle = nullptr;  // NEW: for prompt output
static std::mutex g_stdin_mutex;
static std::vector<FunctionPtr> g_data_listeners;
static std::vector<FunctionPtr> g_eof_listeners;
static std::vector<FunctionPtr> g_sigint_listeners;
static std::atomic<bool> g_stdin_initialized(false);
static std::atomic<bool> g_stdin_closed(false);
static std::atomic<bool> g_raw_mode(false);
static std::atomic<bool> g_paused(false);
static std::atomic<bool> g_discard_on_pause(false);
static std::atomic<bool> g_discard_on_resume(false);

static uv_async_t* g_pause_keepalive = nullptr;
static std::string g_line_buffer;

// NEW: Prompt state
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

    std::vector<FunctionPtr> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_data_listeners;
    }
    for (auto& cb : listeners) {
        if (!cb) continue;
        CallbackPayload* p = new CallbackPayload(cb, {Value{buffer}});
        enqueue_callback_global(static_cast<void*>(p));
    }
}

static void enqueue_eof_callbacks() {
    std::vector<FunctionPtr> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_eof_listeners;
    }

    for (auto& cb : listeners) {
        if (!cb) continue;
        CallbackPayload* p = new CallbackPayload(cb, {});
        enqueue_callback_global(static_cast<void*>(p));
    }
}

static void enqueue_sigint_callbacks() {
    std::vector<FunctionPtr> listeners;
    {
        std::lock_guard<std::mutex> lk(g_stdin_mutex);
        listeners = g_sigint_listeners;
    }

    for (auto& cb : listeners) {
        if (!cb) continue;
        CallbackPayload* p = new CallbackPayload(cb, {});
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

                if (ch == 0x03) {
                    enqueue_sigint_callbacks();
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                } else if (ch == 0x04) {
                    enqueue_eof_callbacks();
                    enqueue_data_callbacks_from_bytes((const char*)&ch, 1);
                } else {
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

                    std::vector<FunctionPtr> listeners;
                    {
                        std::lock_guard<std::mutex> lk(g_stdin_mutex);
                        listeners = g_data_listeners;
                    }
                    for (auto& cb : listeners) {
                        if (!cb) continue;
                        CallbackPayload* p = new CallbackPayload(cb, {Value{buffer}});
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
            g_data_listeners.push_back(cb);
            ensure_init();
        } else if (event == "eof") {
            g_eof_listeners.push_back(cb);
            ensure_init();
        } else if (event == "sigint") {
            g_sigint_listeners.push_back(cb);
            ensure_init();
        } else {
            throw SwaziError("TypeError",
                "stdin.on unknown event. Valid: data, eof, sigint", token.loc);
        }

        return std::monostate{};
    };

    obj->properties["on"] = {
        Value{std::make_shared<FunctionValue>("stdin.on", on_impl, env, tok)},
        false, false, true, tok};

    // NEW: stdin.prompt(text) - sets and displays a prompt
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
            g_prompt_active.store(false);  // Disable prompt when paused
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

                    // Re-enable and display prompt if set
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

        if (enabled && !g_raw_mode.load()) {
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            g_line_buffer.clear();
            g_prompt_active.store(false);  // Disable prompts in raw mode
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

            if (loop) destroy_pause_keepalive(loop);

            g_stdin_initialized.store(false);
        });

        return std::monostate{};
    };
    obj->properties["close"] = {
        Value{std::make_shared<FunctionValue>("stdin.close", close_impl, env, tok)},
        false, false, true, tok};

    return obj;
}
