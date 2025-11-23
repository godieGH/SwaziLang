#pragma once
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// ============================================================================
// FORWARD DECLARATIONS
// ============================================================================

struct ReadableStreamState;
struct WritableStreamState;

using ReadableStreamStatePtr = std::shared_ptr<ReadableStreamState>;
using WritableStreamStatePtr = std::shared_ptr<WritableStreamState>;

// ============================================================================
// STREAM STATE STRUCTURES
// ============================================================================

struct ReadableStreamState : public std::enable_shared_from_this<ReadableStreamState> {
    long long id;
    uv_file fd = -1;
    std::string path;

    size_t current_position = 0;
    size_t stream_start = 0;
    size_t stream_end = 0;
    size_t file_size = 0;

    size_t high_water_mark = 65536;
    std::string encoding = "binary";
    bool auto_close = true;
    double speed = 1.0;

    bool paused = false;
    bool ended = false;
    bool reading = false;
    bool destroyed = false;
    bool flowing = false;

    EnvPtr env;
    Evaluator* evaluator = nullptr;

    std::vector<FunctionPtr> data_listeners;
    std::vector<FunctionPtr> end_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> close_listeners;

    std::vector<std::shared_ptr<ReadableStreamState>> self_references;

    void keep_alive() {
        self_references.push_back(shared_from_this());
    }

    void release_keepalive() {
        self_references.clear();
    }

    void close_file() {
        if (fd >= 0) {
            uv_fs_t close_req;
            uv_fs_close(scheduler_get_loop(), &close_req, fd, NULL);
            uv_fs_req_cleanup(&close_req);
            fd = -1;
        }
    }

    void pause();

    void resume();
};

struct WriteChunk {
    std::vector<char> data;
    FunctionPtr callback;
};

struct WritableStreamState : public std::enable_shared_from_this<WritableStreamState> {
    long long id;
    uv_file fd = -1;
    std::string path;

    size_t high_water_mark = 65536;
    std::string encoding = "utf8";
    bool auto_destroy = true;

    std::deque<WriteChunk> write_queue;
    size_t buffered_size = 0;
    bool writing = false;
    bool draining = false;

    bool ended = false;
    bool finished = false;
    bool destroyed = false;
    bool corked = false;
    size_t cork_count = 0;

    size_t bytes_written = 0;

    EnvPtr env;
    Evaluator* evaluator = nullptr;

    std::vector<FunctionPtr> drain_listeners;
    std::vector<FunctionPtr> finish_listeners;
    std::vector<FunctionPtr> error_listeners;
    std::vector<FunctionPtr> close_listeners;

    std::vector<std::shared_ptr<WritableStreamState>> self_references;

    void keep_alive() {
        self_references.push_back(shared_from_this());
    }

    void release_keepalive() {
        self_references.clear();
    }

    void close_file() {
        if (fd >= 0) {
            uv_fs_t close_req;
            uv_fs_close(scheduler_get_loop(), &close_req, fd, NULL);
            uv_fs_req_cleanup(&close_req);
            fd = -1;
        }
    }
};

// ============================================================================
// GLOBAL STATE
// ============================================================================

extern std::atomic<size_t> g_active_stream_operations;

extern std::mutex g_readable_streams_mutex;
extern std::unordered_map<long long, ReadableStreamStatePtr> g_readable_streams;

extern std::mutex g_writable_streams_mutex;
extern std::unordered_map<long long, WritableStreamStatePtr> g_writable_streams;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

std::string value_to_string_simple(const Value& v);

void schedule_listener_call(FunctionPtr cb, const std::vector<Value>& args);

Value encode_buffer_for_emission(const BufferPtr& buf, const std::string& encoding);

void schedule_next_read(ReadableStreamStatePtr state);

void schedule_next_write(WritableStreamStatePtr state);

ObjectPtr create_readable_stream_object(ReadableStreamStatePtr state);

ObjectPtr create_writable_stream_object(WritableStreamStatePtr state);

// ============================================================================
// FACTORY FUNCTIONS
// ============================================================================

Value native_createReadStream(const std::vector<Value>& args, EnvPtr env, Evaluator* evaluator, const Token& token);
Value native_createWriteStream(const std::vector<Value>& args, EnvPtr env, Evaluator* evaluator, const Token& token);

// ============================================================================
// PIPE CONTEXT
// ============================================================================

struct PipeContext {
    ReadableStreamStatePtr readable;
    WritableStreamStatePtr writable;
    bool end_on_finish = true;
    bool piping = false;
    bool cleanup_done = false;
    
    FunctionPtr data_handler;
    FunctionPtr end_handler;
    FunctionPtr error_handler;
    FunctionPtr close_handler;
    FunctionPtr drain_handler;
};

using PipeContextPtr = std::shared_ptr<PipeContext>;

// ============================================================================
// PIPE FUNCTIONS
// ============================================================================

void cleanup_pipe(PipeContextPtr ctx);

Value implement_pipe(ReadableStreamStatePtr readable_state, 
                     WritableStreamStatePtr writable_state,
                     bool end_on_finish,
                     const Token& token);