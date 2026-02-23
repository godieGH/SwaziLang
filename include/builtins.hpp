#pragma once
#include "evaluator.hpp"
#include "swazi.hpp"
#include "uv.h"

// Factories producing the ObjectPtr exports for each builtin module.
// The EnvPtr is supplied so native functions can capture a module environment if needed.
std::shared_ptr<ObjectValue> make_regex_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_fs_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_http_exports(EnvPtr env, Evaluator* evaluator);

std::shared_ptr<ObjectValue> make_json_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_path_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_os_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_process_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_timers_exports(EnvPtr env);

// Fork implementation (defined in subprocess_fork.cc)
Value native_fork(const std::vector<Value>& args, EnvPtr env, const Token& token);
std::shared_ptr<ObjectValue> make_subprocess_exports(EnvPtr env, Evaluator* evaluator);
Value process_send_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token);
Value process_on_message_ipc(const std::vector<Value>& args, EnvPtr env, const Token& token);

std::shared_ptr<ObjectValue> make_base64_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_buffer_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_file_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_streams_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_net_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_crypto_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_collections_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_archiver_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_datetime_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_events_exports(EnvPtr env);

Value native_createStdout(EnvPtr env, Evaluator* evaluator);
Value native_createStderr(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_stdin_exports(EnvPtr env);
std::shared_ptr<ObjectValue> make_ipc_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_uv_exports(EnvPtr env);

// Forward declaration: native createServer implementation (defined in HttpAPI.cpp)
Value native_createServer(const std::vector<Value>& args, EnvPtr env, const Token& token, Evaluator* evaluator);
void native_http_exetended(const ObjectPtr& http_module, Evaluator* evaluator, EnvPtr env);

// Network stream helpers (defined in streams.cc, used by HttpAPI.cpp)
ObjectPtr create_network_readable_stream_object(uv_tcp_t* socket);
ObjectPtr create_network_writable_stream_object(uv_tcp_t* socket);

// for process_detach_impl abd process_ignore_signals_impl wrappers
Value process_detach_impl(const std::vector<Value>& args, EnvPtr env, const Token& token);
Value process_ignore_signals_impl(const std::vector<Value>& args, EnvPtr env, const Token& token);

bool tcp_has_active_work();
bool net_has_active_work();
bool udp_has_active_work();
bool unix_has_active_work();
bool streams_have_active_work();
bool fs_has_active_work();
bool ipc_has_active_work();
bool http_has_active_work();
bool http_fetch_has_active_work();
bool uv_module_has_active_handles();

struct SignalInfo {
    std::string name;
    int number;
    bool catchable;
    std::string description;
};
std::vector<SignalInfo> get_all_signals();