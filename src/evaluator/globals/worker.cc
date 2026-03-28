#include "worker.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

#include "Scheduler.hpp"
#include "SourceManager.hpp"
#include "SwaziError.hpp"
#include "lexer.hpp"
#include "parser.hpp"

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Registry
// ─────────────────────────────────────────────────────────────────────────────
static std::mutex g_workers_mutex;
static std::vector<WorkerCtxPtr> g_active_workers;

// Set to the current thread's WorkerCtx while inside worker_thread_fn.
// Null on the main thread.  Used by worker_threads_exist() so that a worker's
// own run_until_idle predicate does not count itself as "pending work" — which
// would cause the worker to deadlock waiting for itself to finish.
static thread_local WorkerCtx* g_current_worker_ctx = nullptr;

bool worker_threads_exist() {
    std::lock_guard<std::mutex> lk(g_workers_mutex);
    for (auto& w : g_active_workers) {
        if (w.get() == g_current_worker_ctx) continue;
        if (!w->running.load()) continue;
        // Only count workers this thread actually spawned
        if (w->parent_ctx == g_current_worker_ctx) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// deep_clone_value
// ─────────────────────────────────────────────────────────────────────────────
Value deep_clone_value(const Value& v, const Token& tok) {
    if (std::holds_alternative<std::monostate>(v)) return v;
    if (std::holds_alternative<double>(v)) return v;
    if (std::holds_alternative<std::string>(v)) return v;
    if (std::holds_alternative<bool>(v)) return v;

    if (auto* b = std::get_if<BufferPtr>(&v)) {
        auto nb = std::make_shared<BufferValue>();
        nb->data = (*b)->data;
        nb->encoding = (*b)->encoding;
        return nb;
    }
    if (auto* r = std::get_if<RangePtr>(&v)) {
        auto nr = std::make_shared<RangeValue>(
            (*r)->start, (*r)->end, (*r)->step, (*r)->inclusive);
        nr->cur = (*r)->cur;
        return nr;
    }
    if (auto* dt = std::get_if<DateTimePtr>(&v))
        return std::make_shared<DateTimeValue>(**dt);
    if (auto* rx = std::get_if<RegexPtr>(&v))
        return std::make_shared<RegexValue>((*rx)->pattern, (*rx)->flags);
    if (auto* a = std::get_if<ArrayPtr>(&v)) {
        auto na = std::make_shared<ArrayValue>();
        na->elements.reserve((*a)->elements.size());
        for (auto& el : (*a)->elements)
            na->elements.push_back(deep_clone_value(el, tok));
        return na;
    }
    if (auto* o = std::get_if<ObjectPtr>(&v)) {
        auto no = std::make_shared<ObjectValue>();
        for (auto& [k, pd] : (*o)->properties) {
            PropertyDescriptor npd = pd;
            npd.value = deep_clone_value(pd.value, tok);
            no->properties[k] = npd;
        }
        return no;
    }
    throw SwaziError("TypeError",
        "Worker: value type '" + _type_name(v) + "' is not transferable across threads.",
        tok.loc);
}

// ─────────────────────────────────────────────────────────────────────────────
// keep-alive helpers
// Must only be called from the worker thread (it owns the loop).
// ─────────────────────────────────────────────────────────────────────────────
static void no_op_async_cb(uv_async_t*) {}

static void close_keep_alive(WorkerCtxPtr ctx) {
    if (!ctx->keep_alive_initialized.exchange(false)) return;
    uv_close(reinterpret_cast<uv_handle_t*>(&ctx->keep_alive_handle), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// Message delivery helpers
// ─────────────────────────────────────────────────────────────────────────────

// main → worker  (retries on next main tick if worker hasn't booted yet)
static void deliver_to_worker(WorkerCtxPtr ctx, Value msg) {
    Scheduler* ws = ctx->worker_scheduler.load();
    if (!ws) {
        // Worker hasn't booted yet — buffer locally.
        // Drained in worker_thread_fn after scheduler is published.
        std::lock_guard<std::mutex> lk(ctx->pending_mutex);
        ctx->pending_messages.push_back(std::move(msg));
        return;
    }
    ws->enqueue_macrotask([ctx, msg = std::move(msg)]() mutable {
        Evaluator* we = ctx->worker_evaluator.load();
        if (!we) return;
        auto it = ctx->worker_port_obj->properties.find("onmessage");
        if (it == ctx->worker_port_obj->properties.end()) return;
        auto* fp = std::get_if<FunctionPtr>(&it->second.value);
        if (!fp || !*fp) return;
        try {
            we->invoke_function(*fp, {msg}, (*fp)->closure, Token{});
        } catch (const std::exception& e) {
            std::cerr << "[Worker:" << ctx->label
                      << "] onmessage error: " << e.what() << "\n";
        }
    });
    ws->notify();
}

// worker → main
static void deliver_to_main(WorkerCtxPtr ctx, Value msg) {
    if (!ctx->main_scheduler) return;
    ctx->main_scheduler->enqueue_macrotask([ctx, msg = std::move(msg)]() {
        if (!ctx->main_evaluator) return;
        auto it = ctx->main_worker_obj->properties.find("onmessage");
        if (it == ctx->main_worker_obj->properties.end()) return;
        auto* fp = std::get_if<FunctionPtr>(&it->second.value);
        if (!fp || !*fp) return;
        try {
            ctx->main_evaluator->invoke_function(
                *fp, {msg}, (*fp)->closure, Token{});
        } catch (const std::exception& e) {
            std::cerr << "[Worker:" << ctx->label
                      << "] w.onmessage error: " << e.what() << "\n";
        }
    });
    ctx->main_scheduler->notify();
}

// worker → main  explicit error channel
static void deliver_error_to_main(WorkerCtxPtr ctx, Value err) {
    if (!ctx->main_scheduler) return;
    ctx->main_scheduler->enqueue_macrotask([ctx, err = std::move(err)]() {
        if (!ctx->main_evaluator) return;
        auto it = ctx->main_worker_obj->properties.find("onerror");
        if (it == ctx->main_worker_obj->properties.end()) return;
        auto* fp = std::get_if<FunctionPtr>(&it->second.value);
        if (!fp || !*fp) return;
        try {
            ctx->main_evaluator->invoke_function(
                *fp, {err}, (*fp)->closure, Token{});
        } catch (const std::exception& e) {
            std::cerr << "[Worker:" << ctx->label
                      << "] w.on(\"error\") handler threw: " << e.what() << "\n";
        }
    });
    ctx->main_scheduler->notify();
}

// ─────────────────────────────────────────────────────────────────────────────
// fire_exit_on_main
// Enqueues the "exit" event on the main scheduler and wakes it.
// Called at the very end of worker_thread_fn — the ONLY place that should
// flip running → false and notify main.  This is also the fix for the hang:
// without this notify, main's uv_run never returns when the last worker exits.
// ─────────────────────────────────────────────────────────────────────────────
static void fire_exit_on_main(WorkerCtxPtr ctx) {
    ctx->worker_evaluator.store(nullptr);
    ctx->worker_scheduler.store(nullptr);
    ctx->running.store(false);
    ctx->terminated.store(true);

    if (!ctx->main_scheduler) return;

    int code = ctx->exit_code.load();
    ctx->main_scheduler->enqueue_macrotask([ctx, code]() {
        if (!ctx->main_evaluator) return;
        auto it = ctx->main_worker_obj->properties.find("onexit");
        if (it == ctx->main_worker_obj->properties.end()) return;
        auto* fp = std::get_if<FunctionPtr>(&it->second.value);
        if (!fp || !*fp) return;
        try {
            ctx->main_evaluator->invoke_function(
                *fp, {(double)code}, (*fp)->closure, Token{});
        } catch (const std::exception& e) {
            std::cerr << "[Worker:" << ctx->label
                      << "] w.on(\"exit\") handler error: " << e.what() << "\n";
        }
    });
    // Wake the main loop.  This is the critical notify that was missing before,
    // which caused the process to hang after workers finished.
    ctx->main_scheduler->notify();
}

// ─────────────────────────────────────────────────────────────────────────────
// Worker thread entry
// ─────────────────────────────────────────────────────────────────────────────
static void worker_thread_fn(void* arg) {
    WorkerCtxPtr* raw = static_cast<WorkerCtxPtr*>(arg);
    WorkerCtxPtr ctx = *raw;
    delete raw;
    g_current_worker_ctx = ctx.get();

    try {
        Evaluator evaluator;

        if (ctx->mode != WorkerMode::Function)
            evaluator.set_entry_point(ctx->source_path);

        if (!ctx->worker_argv.empty())
            evaluator.set_cli_args(ctx->worker_argv);

        ctx->worker_scheduler.store(evaluator.scheduler());
        ctx->worker_evaluator.store(&evaluator);

        // Drain messages buffered before scheduler was published.
        {
            std::vector<Value> pending;
            {
                std::lock_guard<std::mutex> lk(ctx->pending_mutex);
                pending = std::move(ctx->pending_messages);
            }
            for (auto& m : pending)
                deliver_to_worker(ctx, std::move(m));
        }

        // Inject parentThread into the worker's global env — all three modes need it.
        Environment::Variable sv;
        sv.value = ctx->worker_port_obj;
        sv.is_constant = true;
        evaluator.get_global_env()->set("parentThread", sv);

        struct KeepAliveGuard {
            WorkerCtxPtr& ctx;
            ~KeepAliveGuard() {
                if (!ctx->keep_alive_initialized.load()) return;
                close_keep_alive(ctx);
                if (Scheduler* ws = ctx->worker_scheduler.load())
                    uv_run(ws->get_uv_loop(), UV_RUN_NOWAIT);
            }
        } keep_alive_guard{ctx};

        if (ctx->mode == WorkerMode::Function) {
            // ── Function mode ────────────────────────────────────────────────
            // Take the body AST from the captured FunctionPtr but run it
            // inside the worker's own fresh global env — no closure bleed.
            // parentThread is passed as the first (and only) argument.
            FunctionPtr fn = ctx->worker_fn;

            // Build a thin wrapper that shares the same body but whose
            // closure is the worker's global env, not the main thread's env.
            auto isolated = std::make_shared<FunctionValue>(
                fn->name.empty() ? "<worker>" : fn->name,
                fn->parameters,
                fn->body,
                evaluator.get_global_env(),  // ← worker's env, not fn->closure
                fn->token);
            isolated->is_async = fn->is_async;
            isolated->is_generator = fn->is_generator;

            // parentThread is already in global env; also pass it as arg
            // so the function signature (parentThread) => { ... } works.
            Value pt_val = ctx->worker_port_obj;
            evaluator.invoke_function(isolated, {pt_val},
                evaluator.get_global_env(), fn->token);

            // Drive the event loop to completion (async work, timers, etc.)
            evaluator.run_loop();

        } else {
            // ── File / Eval mode ─────────────────────────────────────────────
            auto src_mgr = std::make_shared<SourceManager>(
                ctx->source_path, ctx->source_code);
            Lexer lexer(ctx->source_code, ctx->source_path, src_mgr.get());
            auto tokens = lexer.tokenize();
            Parser parser(tokens);
            auto ast = parser.parse();
            evaluator.evaluate(ast.get());  // runs loop internally
        }

    } catch (const std::exception& e) {
        std::cerr << "[Worker:" << ctx->label << "] fatal: " << e.what() << "\n";
        ctx->exit_code.store(1);
    } catch (...) {
        std::cerr << "[Worker:" << ctx->label << "] unknown fatal error\n";
        ctx->exit_code.store(1);
    }

    g_current_worker_ctx = nullptr;
    fire_exit_on_main(ctx);
}
// ─────────────────────────────────────────────────────────────────────────────
// join_all_workers — called after main event loop exits
// ─────────────────────────────────────────────────────────────────────────────
void join_all_workers() {
    std::vector<WorkerCtxPtr> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_workers_mutex);
        snapshot = g_active_workers;
    }
    for (auto& ctx : snapshot) {
        if (ctx.get() == g_current_worker_ctx) continue;        // skip self
        if (ctx->parent_ctx != g_current_worker_ctx) continue;  // skip siblings/others
        Scheduler* ws = ctx->worker_scheduler.load();
        if (ctx->running.load() && ws) ws->stop();
        uv_thread_join(&ctx->thread);
    }
    // Only the main thread owns the global registry
    if (g_current_worker_ctx == nullptr) {
        std::lock_guard<std::mutex> lk(g_workers_mutex);
        g_active_workers.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// init_worker — registers the Worker constructor in the global environment
//
// Usage in SwaziLang:
//
//   data w = Worker("path/to/worker.sl")
//   data w = Worker("path/to/worker.sl", { argv: ["--port", "3000"] })
//
// Worker events (register via w.on):
//   "message"  — worker called parentThread.send(value)
//   "error"    — worker threw an error value
//   "exit"     — worker thread finished; callback receives exit code (0 or 1)
//
// parentThread (available inside the worker script as a global):
//   parentThread.send(value)           — send a value to the main thread
//   parentThread.error(value)          — send error as value to the main thread
//   parentThread.on("message", fn)     — receive values from the main thread
//   parentThread.terminate()           — exit this worker cleanly
//
// ─────────────────────────────────────────────────────────────────────────────
void init_worker(EnvPtr env, Evaluator* evaluator) {
    if (!env || !evaluator) return;

    auto add_fn = [&](const std::string& name,
                      std::function<Value(const std::vector<Value>&, EnvPtr, const Token&)> impl) {
        auto fn = std::make_shared<FunctionValue>(name, impl, env, Token{});

        // ── Worker.max  (static — never changes per process) ─────────────────────────
        fn->properties["max"] = PropertyDescriptor{
            Value{(double)SWAZI_MAX_WORKERS}, false, true, true, Token{}};

        // ── Worker.active()  live count of running workers ───────────────────────────
        fn->properties["active"] = PropertyDescriptor{
            std::make_shared<FunctionValue>("native:Worker.active", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            std::lock_guard<std::mutex> lk(g_workers_mutex);
            int count = 0;
            for (auto& w : g_active_workers)
                if (w->running.load()) count++;
            return (double)count; }, nullptr, Token{}), false, true, true, Token{}};

        // ── Worker.list()  snapshot of active workers ────────────────────────────────
        fn->properties["list"] = PropertyDescriptor{
            std::make_shared<FunctionValue>("native:Worker.list", [](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
            std::lock_guard<std::mutex> lk(g_workers_mutex);
            auto arr = std::make_shared<ArrayValue>();
            for (auto& w : g_active_workers) {
                if (!w->running.load()) continue;
                auto obj = std::make_shared<ObjectValue>();
                obj->properties["id"] =
                    PropertyDescriptor{Value{w->label}, false, true, true, Token{}};
                arr->elements.push_back(Value{obj});
            }
            return arr; }, nullptr, Token{}), false, true, true, Token{}};

        env->set(name, {fn, true});
    };

    add_fn("Worker",
        [evaluator](const std::vector<Value>& args, EnvPtr, const Token& tok) -> Value {
            // ── Mode detection ───────────────────────────────────────────────────────────
            if (args.empty())
                throw SwaziError("TypeError",
                    "Worker(): requires at least one argument (path, code string, or function).", tok.loc);

            WorkerMode mode = WorkerMode::File;
            FunctionPtr worker_fn;

            if (std::holds_alternative<FunctionPtr>(args[0])) {
                // Worker(fn, opts?)
                worker_fn = std::get<FunctionPtr>(args[0]);
                if (!worker_fn)
                    throw SwaziError("TypeError", "Worker(): function argument must not be null.", tok.loc);
                mode = WorkerMode::Function;

            } else if (std::holds_alternative<std::string>(args[0])) {
                // Worker("...", opts?) — file or eval, determined by options.eval
                // options are parsed below; default is File unless eval:true is found
                mode = WorkerMode::File;  // may be overridden after options parse

            } else {
                throw SwaziError("TypeError",
                    "Worker(): first argument must be a file path, a code string, or a function.", tok.loc);
            }

            // ── Worker cap ──────────────────────────────────────────────────────
            {
                std::lock_guard<std::mutex> lk(g_workers_mutex);
                int alive = 0;
                for (auto& w : g_active_workers)
                    if (w->running.load()) alive++;
                if (alive >= SWAZI_MAX_WORKERS)
                    throw SwaziError("RuntimeError",
                        "Worker: cannot spawn more than " +
                            std::to_string(SWAZI_MAX_WORKERS) +
                            " concurrent workers (= logical CPU cores).",
                        tok.loc);
            }

            // ── Parse options object ─────────────────────────────────────────────────────
            std::vector<std::string> worker_argv;
            bool eval_mode = false;

            if (args.size() >= 2) {
                // In Function mode the second arg is still options — that's fine.
                // We just ignore eval: there is nothing to eval.
                auto* opts = std::get_if<ObjectPtr>(&args[1]);
                if (!opts)
                    throw SwaziError("TypeError",
                        "Worker(spec, options): options must be an object.", tok.loc);

                // options.eval — only meaningful in string mode
                auto eval_it = (*opts)->properties.find("eval");
                if (eval_it != (*opts)->properties.end()) {
                    if (auto* b = std::get_if<bool>(&eval_it->second.value))
                        eval_mode = *b;
                }

                // options.argv
                auto argv_it = (*opts)->properties.find("argv");
                if (argv_it != (*opts)->properties.end()) {
                    auto* arr = std::get_if<ArrayPtr>(&argv_it->second.value);
                    if (!arr)
                        throw SwaziError("TypeError",
                            "Worker options.argv must be an array of strings.", tok.loc);
                    for (auto& el : (*arr)->elements) {
                        auto* s = std::get_if<std::string>(&el);
                        if (!s)
                            throw SwaziError("TypeError",
                                "Worker options.argv: every element must be a string.", tok.loc);
                        worker_argv.push_back(*s);
                    }
                }
            }

            // Apply eval flag now that options are parsed
            if (mode == WorkerMode::File && eval_mode)
                mode = WorkerMode::Eval;

            // ── Resolve / load source (skipped entirely in Function mode) ────────────────
            std::string source_path;
            std::string source_code;

            if (mode == WorkerMode::File) {
                std::string spec = std::get<std::string>(args[0]);
                fs::path p(spec);
                if (!p.is_absolute()) p = fs::current_path() / p;
                if (!fs::exists(p)) {
                    for (auto& ext : {".sl", ".swz"}) {
                        fs::path c = fs::path(spec).concat(ext);
                        if (!c.is_absolute()) c = fs::current_path() / c;
                        if (fs::exists(c)) {
                            p = c;
                            break;
                        }
                    }
                }
                if (!fs::exists(p))
                    throw SwaziError("RuntimeError",
                        "Worker: cannot find file '" + spec + "'.", tok.loc);
                std::ifstream f(p);
                if (!f.is_open())
                    throw SwaziError("RuntimeError",
                        "Worker: cannot open '" + p.string() + "'.", tok.loc);
                std::ostringstream ss;
                ss << f.rdbuf();
                source_path = p.string();
                source_code = ss.str();
                if (source_code.empty() || source_code.back() != '\n') source_code += '\n';

            } else if (mode == WorkerMode::Eval) {
                source_path = "<eval>";
                source_code = std::get<std::string>(args[0]);
                if (source_code.empty() || source_code.back() != '\n') source_code += '\n';
            }

            // ── Build context ────────────────────────────────────────────────────
            static std::atomic<int> id_counter{0};
            auto ctx = std::make_shared<WorkerCtx>();
            ctx->label = "W" + std::to_string(id_counter.fetch_add(1));
            ctx->mode = mode;
            ctx->source_path = source_path;
            ctx->source_code = std::move(source_code);
            ctx->worker_fn = worker_fn;
            ctx->worker_argv = std::move(worker_argv);
            ctx->main_scheduler = evaluator->scheduler();
            ctx->main_evaluator = evaluator;
            ctx->main_worker_obj = std::make_shared<ObjectValue>();
            ctx->worker_port_obj = std::make_shared<ObjectValue>();

            ObjectPtr worker_obj = ctx->main_worker_obj;
            ObjectPtr port_obj = ctx->worker_port_obj;

            // ────────────────────────────────────────────────────────────────────
            // parentThread.send(value)   worker → main
            // ────────────────────────────────────────────────────────────────────
            port_obj->properties["send"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:parentThread.send",
                    [ctx](const std::vector<Value>& a, EnvPtr, const Token& t) -> Value {
                        if (a.empty())
                            throw SwaziError("TypeError",
                                "parentThread.send() requires one argument.", t.loc);
                        deliver_to_main(ctx, deep_clone_value(a[0], t));
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ────────────────────────────────────────────────────────────────────
            // parentThread.error(value)   worker → main explicit error channel
            // ────────────────────────────────────────────────────────────────────
            port_obj->properties["error"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:parentThread.error",
                    [ctx](const std::vector<Value>& a, EnvPtr, const Token& t) -> Value {
                        if (a.empty())
                            throw SwaziError("TypeError",
                                "parentThread.error() requires one argument.", t.loc);
                        deliver_error_to_main(ctx, deep_clone_value(a[0], t));
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ────────────────────────────────────────────────────────────────────
            // parentThread.on(event, fn)   worker registers a listener
            //
            // Supported events: "message"
            //
            // First call arms the keep-alive uv_async_t on the worker's own loop so
            // run_until_idle does not exit while the worker is still listening for
            // messages from the main thread.
            // ────────────────────────────────────────────────────────────────────
            port_obj->properties["on"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:parentThread.on",
                    [ctx](const std::vector<Value>& a, EnvPtr, const Token& t) -> Value {
                        if (a.size() < 2)
                            throw SwaziError("TypeError",
                                "parentThread.on(event, fn) requires two arguments.", t.loc);
                        if (!std::holds_alternative<std::string>(a[0]))
                            throw SwaziError("TypeError",
                                "parentThread.on(): first argument must be a string.", t.loc);
                        if (!std::holds_alternative<FunctionPtr>(a[1]))
                            throw SwaziError("TypeError",
                                "parentThread.on(): second argument must be a function.", t.loc);

                        const std::string& event = std::get<std::string>(a[0]);

                        if (event == "message") {
                            ctx->worker_port_obj->properties["onmessage"] =
                                PropertyDescriptor{a[1], false, false, false, t};
                        } else {
                            throw SwaziError("RuntimeError",
                                "parentThread.on(): unknown event '" + event +
                                    "'. Supported: \"message\".",
                                t.loc);
                        }

                        // Arm keep-alive on first listener registration.
                        // If this is never called the worker loop exits naturally
                        // when the script finishes — no hang possible.
                        if (!ctx->is_listening.exchange(true)) {
                            Scheduler* ws = ctx->worker_scheduler.load();
                            if (ws && ws->get_uv_loop()) {
                                uv_async_init(ws->get_uv_loop(),
                                    &ctx->keep_alive_handle, no_op_async_cb);
                                ctx->keep_alive_initialized.store(true);
                            }
                        }
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ────────────────────────────────────────────────────────────────────
            // parentThread.terminate()   worker exits itself
            //
            // Called from inside the worker thread — owns the loop, so closing the
            // keep-alive handle here is safe.  The loop exits naturally on next
            // uv_run iteration; fire_exit_on_main() at the bottom of
            // worker_thread_fn then wakes the main loop and fires w.on("exit").
            // ────────────────────────────────────────────────────────────────────
            port_obj->properties["terminate"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:parentThread.terminate",
                    [ctx](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                        ctx->terminated.store(true);
                        close_keep_alive(ctx);  // safe — we ARE the worker thread
                        Scheduler* ws = ctx->worker_scheduler.load();
                        if (ws) ws->stop();
                        // Main loop woken by fire_exit_on_main() at thread end.
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ────────────────────────────────────────────────────────────────────
            // w.send(value)   main → worker
            // ────────────────────────────────────────────────────────────────────
            worker_obj->properties["send"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:Worker.send",
                    [ctx](const std::vector<Value>& a, EnvPtr, const Token& t) -> Value {
                        if (a.empty())
                            throw SwaziError("TypeError",
                                "w.send() requires one argument.", t.loc);
                        if (ctx->terminated.load())
                            throw SwaziError("RuntimeError",
                                "w.send(): worker '" + ctx->label + "' has already terminated.", t.loc);
                        deliver_to_worker(ctx, deep_clone_value(a[0], t));
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ────────────────────────────────────────────────────────────────────
            // w.on(event, fn)   main registers a listener on the worker handle
            //
            // Supported events:
            //   "message" — worker called parentThread.send(value)
            //   "error"   — worker threw an unhandled error (string payload)
            //   "exit"    — worker exited; callback receives exit code (0 or 1)
            // ────────────────────────────────────────────────────────────────────
            worker_obj->properties["on"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:Worker.on",
                    [ctx](const std::vector<Value>& a, EnvPtr, const Token& t) -> Value {
                        if (a.size() < 2)
                            throw SwaziError("TypeError",
                                "w.on(event, fn) requires two arguments.", t.loc);
                        if (!std::holds_alternative<std::string>(a[0]))
                            throw SwaziError("TypeError",
                                "w.on(): first argument must be a string.", t.loc);
                        if (!std::holds_alternative<FunctionPtr>(a[1]))
                            throw SwaziError("TypeError",
                                "w.on(): second argument must be a function.", t.loc);

                        const std::string& event = std::get<std::string>(a[0]);

                        if (event == "message") {
                            ctx->main_worker_obj->properties["onmessage"] =
                                PropertyDescriptor{a[1], false, false, false, t};
                        } else if (event == "error") {
                            ctx->main_worker_obj->properties["onerror"] =
                                PropertyDescriptor{a[1], false, false, false, t};
                        } else if (event == "exit") {
                            // Stored as "onexit"; looked up by fire_exit_on_main().
                            ctx->main_worker_obj->properties["onexit"] =
                                PropertyDescriptor{a[1], false, false, false, t};
                        } else {
                            throw SwaziError("RuntimeError",
                                "w.on(): unknown event '" + event +
                                    "'. Supported: \"message\", \"error\", \"exit\".",
                                t.loc);
                        }
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ────────────────────────────────────────────────────────────────────
            // w.terminate()   main kills the worker
            //
            // Stops the worker's scheduler.  The worker thread will fall through to
            // fire_exit_on_main() which wakes the main loop and fires "exit".
            // No need to enqueue close_keep_alive from here — worker_thread_fn
            // already calls it in every exit path, including after ws->stop().
            // ────────────────────────────────────────────────────────────────────
            worker_obj->properties["terminate"] = PropertyDescriptor{
                std::make_shared<FunctionValue>(
                    "native:Worker.terminate",
                    [ctx](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
                        if (ctx->terminated.load()) return std::monostate{};
                        ctx->terminated.store(true);
                        Scheduler* ws = ctx->worker_scheduler.load();
                        if (ws) ws->stop();
                        // Main loop woken by fire_exit_on_main() at thread end.
                        return std::monostate{};
                    },
                    nullptr, Token{}),
                false, false, true, Token{}};

            // ── w.id  (read-only string "W0", "W1", …) ──────────────────────────
            worker_obj->properties["id"] = PropertyDescriptor{
                Value{ctx->label}, false, true, true, Token{}};

            // ── Spawn ────────────────────────────────────────────────────────────
            ctx->parent_ctx = g_current_worker_ctx;
            ctx->running.store(true);
            {
                std::lock_guard<std::mutex> lk(g_workers_mutex);
                // Evict terminated workers to keep the registry lean.
                g_active_workers.erase(
                    std::remove_if(g_active_workers.begin(), g_active_workers.end(),
                        [](const WorkerCtxPtr& w) { return w->terminated.load(); }),
                    g_active_workers.end());
                g_active_workers.push_back(ctx);
            }
            uv_thread_create(&ctx->thread, worker_thread_fn, new WorkerCtxPtr(ctx));

            return Value{worker_obj};
        });
}
