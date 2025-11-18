/**
 * This module will be the header for the three tcp.cc, udp.cc, ws.cc so they could be called in net_module.cc
 * 
 */

#pragma once

#include "AsyncBridge.hpp"
#include "SwaziError.hpp"
#include "builtins.hpp"
#include "evaluator.hpp"
#include "uv.h"

#include <memory>
#include <string>
#include <vector>

// Forward declarations for TCP, UDP, and WebSocket exports
std::shared_ptr<ObjectValue> make_tcp_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_udp_exports(EnvPtr env, Evaluator* evaluator);
std::shared_ptr<ObjectValue> make_ws_exports(EnvPtr env, Evaluator* evaluator);

// Helper functions for network operations
namespace NetHelpers {
    // Convert Value to string (similar to builtins)
    std::string value_to_string(const Value& v);
    
    // Convert Value to number
    double value_to_number(const Value& v);
    
    // Check if value is a Buffer
    bool is_buffer(const Value& v);
    
    // Extract buffer data from Value
    std::vector<uint8_t> get_buffer_data(const Value& v);
}