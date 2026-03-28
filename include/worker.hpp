#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "evaluator.hpp"
#include "uv.h"

class Scheduler;

// ── Worker source mode ────────────────────────────────────────────────────────
// Discriminates how worker_thread_fn should boot the worker.
enum class WorkerMode {
    File,     // Worker("path.sl")          — load from filesystem
    Eval,     // Worker("code", {eval:true}) — source string, no file
    Function  // Worker(fn)                 — AST body, no source at all
};

// Worker limit = number of logical CPU cores.
// One core → one worker maps cleanly; the OS scheduler handles preemption.
// No artificial inflation (e.g. 8-cap) and no proximity math — keep it simple.
static const int SWAZI_MAX_WORKERS = (int)uv_available_parallelism();

// Safe cross-thread value copy — only transferable data types, throws otherwise.
Value deep_clone_value(const Value& v, const Token& tok = {});

// Join all active workers — called at program exit from Evaluator::evaluate().
void join_all_workers();

// Returns true while at least one worker thread is still running.
// Wired into the main run_event_loop predicate.
bool worker_threads_exist();

struct WorkerCtx {
    uv_thread_t thread;
    std::string label;  // "W0", "W1", … — also exposed as w.id

    std::string source_path;
    std::string source_code;

    WorkerMode mode = WorkerMode::File;

    // Set only in Function mode.  The body AST node is shared read-only across
    // threads (AST is immutable after parse).  The closure is deliberately NOT
    // used — the worker calls this body inside its own fresh global env.
    FunctionPtr worker_fn;

    // argv passed via unda Worker(spec, { argv: [...] }).
    // Injected into the worker's process.argv equivalent at startup.
    // The worker does NOT inherit the main thread's argv — isolation by design.
    //
    // stdout / stderr / stdin notes:
    //   Workers share the process's OS-level fds (0, 1, 2) because they live in
    //   the same OS process.  Concurrent print() calls can interleave — that is
    //   intentional and acceptable for a scripting runtime.  If the caller wants
    //   separated output they should use parentThread.send() / w.on("message").
    std::vector<std::string> worker_argv;

    // ── main-thread handles (set at spawn, valid for process lifetime) ──────
    Scheduler* main_scheduler = nullptr;
    Evaluator* main_evaluator = nullptr;

    // ── worker-thread handles (published by worker thread after boot) ────────
    std::atomic<Scheduler*> worker_scheduler{nullptr};
    std::atomic<Evaluator*> worker_evaluator{nullptr};

    // ── script-facing objects ────────────────────────────────────────────────
    // main side:   data w = unda Worker("...")
    // worker side: parentThread  (injected into worker global env)
    ObjectPtr main_worker_obj;
    ObjectPtr worker_port_obj;

    // ── lifecycle ────────────────────────────────────────────────────────────
    std::atomic<bool> running{false};
    std::atomic<bool> terminated{false};

    // exit code sent to w.on("exit", fn): 0 = clean, 1 = unhandled exception
    std::atomic<int> exit_code{0};

    // keep-alive uv_async_t — lives on the worker's own loop.
    // Armed the first time parentThread.on("message", …) is registered so
    // run_until_idle does not exit while the worker is still listening.
    // Closed via close_keep_alive() on the worker thread when it is no longer
    // needed; safe to skip entirely if never armed (no listener registered).
    uv_async_t keep_alive_handle;
    std::atomic<bool> keep_alive_initialized{false};
    std::atomic<bool> is_listening{false};

    // Messages sent before worker_scheduler is published — drained at boot.
    std::mutex pending_mutex;
    std::vector<Value> pending_messages;

    WorkerCtx* parent_ctx = nullptr;
};

using WorkerCtxPtr = std::shared_ptr<WorkerCtx>;

// Called once from init_globals to register the Worker constructor.
void init_worker(EnvPtr env, Evaluator* evaluator);
