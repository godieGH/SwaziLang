// events.cc
#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AsyncBridge.hpp"
#include "Scheduler.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"

// ============================================================================
// EVENT EMITTER STATE
// ============================================================================

struct EventEmitter : public std::enable_shared_from_this<EventEmitter> {
    std::unordered_map<std::string, std::vector<FunctionPtr>> listeners;
    std::mutex mutex;
    long long id;
};

using EventEmitterPtr = std::shared_ptr<EventEmitter>;

static std::atomic<long long> g_next_emitter_id{1};

// ============================================================================
// HELPER: EMIT EVENT
// ============================================================================

static void emit_event_internal(EventEmitterPtr emitter,
    const std::string& event,
    const std::vector<Value>& args) {
    if (!emitter) return;

    std::vector<FunctionPtr> listeners_copy;
    {
        std::lock_guard<std::mutex> lock(emitter->mutex);
        auto it = emitter->listeners.find(event);
        if (it != emitter->listeners.end()) {
            listeners_copy = it->second;
        }
    }

    for (auto& listener : listeners_copy) {
        if (!listener) continue;
        CallbackPayload* p = new CallbackPayload(listener, args);
        enqueue_callback_global(static_cast<void*>(p));
    }
}

// ============================================================================
// FACTORY: events.create()
// ============================================================================

std::shared_ptr<ObjectValue> make_events_exports(EnvPtr env) {
    auto obj = std::make_shared<ObjectValue>();
    Token tok{};
    tok.loc = TokenLocation("<events>", 0, 0, 0);

    // events.create() -> EventEmitter
    auto create_impl = [env](const std::vector<Value>&, EnvPtr, const Token&) -> Value {
        auto emitter = std::make_shared<EventEmitter>();
        emitter->id = g_next_emitter_id.fetch_add(1);

        auto emitter_obj = std::make_shared<ObjectValue>();
        Token tok{};
        tok.loc = TokenLocation("<events>", 0, 0, 0);

        // emitter.on(event, listener)
        auto on_impl = [emitter](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "on requires (event, listener)", token.loc);
            }
            if (!std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "event must be string", token.loc);
            }
            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "listener must be function", token.loc);
            }

            std::string event = std::get<std::string>(args[0]);
            FunctionPtr listener = std::get<FunctionPtr>(args[1]);

            std::lock_guard<std::mutex> lock(emitter->mutex);
            emitter->listeners[event].push_back(listener);

            return std::monostate{};
        };
        emitter_obj->properties["on"] = {
            Value{std::make_shared<FunctionValue>("on", on_impl, nullptr, tok)},
            false, false, false, tok};

        // emitter.off(event, listener)
        auto off_impl = [emitter](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "off requires (event, listener)", token.loc);
            }
            if (!std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "event must be string", token.loc);
            }
            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "listener must be function", token.loc);
            }

            std::string event = std::get<std::string>(args[0]);
            FunctionPtr listener = std::get<FunctionPtr>(args[1]);

            std::lock_guard<std::mutex> lock(emitter->mutex);
            auto& vec = emitter->listeners[event];

            vec.erase(
                std::remove_if(vec.begin(), vec.end(),
                    [&listener](const FunctionPtr& l) {
                        return l.get() == listener.get();
                    }),
                vec.end());

            return std::monostate{};
        };
        emitter_obj->properties["off"] = {
            Value{std::make_shared<FunctionValue>("off", off_impl, nullptr, tok)},
            false, false, false, tok};

        // emitter.once(event, listener)
        auto once_impl = [emitter, env](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.size() < 2) {
                throw SwaziError("TypeError", "once requires (event, listener)", token.loc);
            }
            if (!std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "event must be string", token.loc);
            }
            if (!std::holds_alternative<FunctionPtr>(args[1])) {
                throw SwaziError("TypeError", "listener must be function", token.loc);
            }

            std::string event = std::get<std::string>(args[0]);
            FunctionPtr listener = std::get<FunctionPtr>(args[1]);

            // Create wrapper that removes itself after first call
            auto wrapper_impl = [emitter, event, listener](
                                    const std::vector<Value>& args, EnvPtr env, const Token& token) -> Value {
                // Remove self first
                {
                    std::lock_guard<std::mutex> lock(emitter->mutex);
                    auto& vec = emitter->listeners[event];
                    // Find and remove the wrapper (this function)
                    vec.erase(
                        std::remove_if(vec.begin(), vec.end(),
                            [](const FunctionPtr& l) {
                                // We can't compare by address since wrapper is the current function
                                // Just remove first matching listener (which will be us)
                                return true;
                            }),
                        vec.begin() + 1);  // Only remove first match
                }

                // Call original listener
                CallbackPayload* p = new CallbackPayload(listener, args);
                enqueue_callback_global(static_cast<void*>(p));

                return std::monostate{};
            };

            auto wrapper = std::make_shared<FunctionValue>("once_wrapper", wrapper_impl, env, token);

            std::lock_guard<std::mutex> lock(emitter->mutex);
            emitter->listeners[event].push_back(wrapper);

            return std::monostate{};
        };
        emitter_obj->properties["once"] = {
            Value{std::make_shared<FunctionValue>("once", once_impl, nullptr, tok)},
            false, false, false, tok};

        // emitter.emit(event, ...args)
        auto emit_impl = [emitter](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "emit requires event name", token.loc);
            }
            if (!std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "event must be string", token.loc);
            }

            std::string event = std::get<std::string>(args[0]);
            std::vector<Value> call_args(args.begin() + 1, args.end());

            emit_event_internal(emitter, event, call_args);

            return std::monostate{};
        };
        emitter_obj->properties["emit"] = {
            Value{std::make_shared<FunctionValue>("emit", emit_impl, nullptr, tok)},
            false, false, false, tok};

        // emitter.removeAllListeners([event])
        auto removeAll_impl = [emitter](const std::vector<Value>& args, EnvPtr, const Token&) -> Value {
            std::lock_guard<std::mutex> lock(emitter->mutex);

            if (args.empty()) {
                emitter->listeners.clear();
            } else if (std::holds_alternative<std::string>(args[0])) {
                std::string event = std::get<std::string>(args[0]);
                emitter->listeners.erase(event);
            }

            return std::monostate{};
        };
        emitter_obj->properties["removeAllListeners"] = {
            Value{std::make_shared<FunctionValue>("removeAllListeners", removeAll_impl, nullptr, tok)},
            false, false, false, tok};

        // emitter.listenerCount(event)
        auto count_impl = [emitter](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "listenerCount requires event name", token.loc);
            }
            if (!std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "event must be string", token.loc);
            }

            std::string event = std::get<std::string>(args[0]);

            std::lock_guard<std::mutex> lock(emitter->mutex);
            auto it = emitter->listeners.find(event);

            if (it != emitter->listeners.end()) {
                return Value{static_cast<double>(it->second.size())};
            }

            return Value{0.0};
        };
        emitter_obj->properties["listenerCount"] = {
            Value{std::make_shared<FunctionValue>("listenerCount", count_impl, nullptr, tok)},
            false, false, false, tok};

        // emitter.listeners(event)
        auto listeners_impl = [emitter](const std::vector<Value>& args, EnvPtr, const Token& token) -> Value {
            if (args.empty()) {
                throw SwaziError("TypeError", "listeners requires event name", token.loc);
            }
            if (!std::holds_alternative<std::string>(args[0])) {
                throw SwaziError("TypeError", "event must be string", token.loc);
            }

            std::string event = std::get<std::string>(args[0]);

            auto arr = std::make_shared<ArrayValue>();

            std::lock_guard<std::mutex> lock(emitter->mutex);
            auto it = emitter->listeners.find(event);

            if (it != emitter->listeners.end()) {
                for (const auto& listener : it->second) {
                    arr->elements.push_back(Value{listener});
                }
            }

            return Value{arr};
        };
        emitter_obj->properties["listeners"] = {
            Value{std::make_shared<FunctionValue>("listeners", listeners_impl, nullptr, tok)},
            false, false, false, tok};

        return Value{emitter_obj};
    };

    obj->properties["create"] = {
        Value{std::make_shared<FunctionValue>("events.create", create_impl, env, tok)},
        false, false, true, tok};

    return obj;
}