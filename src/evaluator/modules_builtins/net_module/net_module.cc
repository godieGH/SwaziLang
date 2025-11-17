
#include "./net.hpp"
#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

// Helper: create a native FunctionValue from a lambda
template <typename F>
static FunctionPtr make_native_fn(const std::string& name, F impl, EnvPtr env) {
    auto native_impl = [impl](const std::vector<Value>& args, EnvPtr callEnv, const Token& token) -> Value {
        return impl(args, callEnv, token);
    };
    auto fn = std::make_shared<FunctionValue>(name, native_impl, env, Token());
    return fn;
}




std::shared_ptr<ObjectValue> make_net_exports(EnvPtr env, Evaluator* evaluator) {
  auto obj = std::make_shared<ObjectValue>();
  
  // net.tcp -> provide tool to work with raw tcp sockets...both server + client
  {
    // code to handle tcp APIs most will be written in tcp.cc and connected here
    // net.tcp is an object that provide other server + client APIs to work with raw tcp connection
    obj->properties["tcp"] = PropertyDescriptor{Value(), false, false, true, Token()};
  }
 
  // net.udp -> provide tool to work with udp binding etc...
  {
    // code to handle udp APIs most will be written in udp.cc and connected here
    // the net.udp is an object that provide other properties
    obj->properties["udp"] = PropertyDescriptor{Value(), false, false, true, Token()};
  }
  
  // net.ws -> websocket - both client + server..
  {
    // code will implemented in ws.cc file... and integrate here
    // it is also an object that provides other ws APIs to work with both client + server websockets
    obj->properties["ws"] = PropertyDescriptor{Value(), false, false, true, Token()};
  }
  
  
  // net.resolve(host) -> dns resolver... we will be using libuv for fast dns resolver
  {
    auto fn_resolve = make_native_fn("net.resolve", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) {
      // handled here
      return Value();
    }, env);
    obj->properties["resolve"] = PropertyDescriptor{fn_resolve, false, false, true, Token()};
  }
  
  // net.isIPv4() -> bool
  {
    auto fn_isIPv4 = make_native_fn("net.isIPv4", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) {
      // handled here
      return Value(false);
    }, env);
    obj->properties["isIPv4"] = PropertyDescriptor{fn_isIPv4, false, false, true, Token()};
  }
  
  // net.isIPv6() -> bool
  {
    auto fn_isIPv6 = make_native_fn("net.isIPv6", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) {
      // handled here
      return Value(false);
    }, env);
    obj->properties["isIPv6"] = PropertyDescriptor{fn_isIPv6, false, false, true, Token()};
  }
  
  // net.localIPs() - Array of all localIPs eg. ["192.168.1.40", "10.0.2.15", "127.0.0.1"]
  {
    auto fn_localIPs = make_native_fn("net.localIPs", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) {
      auto arr = std::make_shared<ArrayValue>();
      // handled here
      return arr;
    }, env);
    obj->properties["localIPs"] = PropertyDescriptor{fn_localIPs, false, false, true, Token()};
  }
  
  // net.isPortFree(port) - bool
  {
    auto fn_isPortFree = make_native_fn("net.isPortFree", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) {
      // handled here
      return Value(false);
    }, env);
    obj->properties["isPortFree"] = PropertyDescriptor{fn_isPortFree, false, false, true, Token()};
  }
  
  // net.ping(host, options?) - options -> eg. { timeout: 2000, method: "tcp" }
  // this should be asyncronous with a promise... that can then chained or awaited
  {
    auto fn_ping = make_native_fn("net.ping", [](const std::vector<Value>& args, EnvPtr /*callEnv*/, const Token& token) {
      // handled here
      return Value(); // will return an object eg. {ok: true,host: "8.8.8.8",method: "icmp|tcp|etc.",rtt: 12.3,error: null|permissionerror|etc.}
    }, env);
    obj->properties["ping"] = PropertyDescriptor{fn_ping, false, false, true, Token()};
  }
  
  return obj;
}