#pragma once

#include <atomic>
#include <cstddef>

namespace MemoryTracking {
// Object counts
extern std::atomic<size_t> g_object_count;
extern std::atomic<size_t> g_array_count;
extern std::atomic<size_t> g_function_count;
extern std::atomic<size_t> g_promise_count;
extern std::atomic<size_t> g_generator_count;
extern std::atomic<size_t> g_buffer_count;
extern std::atomic<size_t> g_file_count;
extern std::atomic<size_t> g_class_count;
extern std::atomic<size_t> g_proxy_count;
extern std::atomic<size_t> g_regex_count;
extern std::atomic<size_t> g_datetime_count;
extern std::atomic<size_t> g_range_count;
extern std::atomic<size_t> g_map_count;

// Buffer bytes (actual data size)
extern std::atomic<size_t> g_buffer_bytes;

// String bytes (if tracking strings)
extern std::atomic<size_t> g_string_bytes;

// Helper to track buffer resize
inline void track_buffer_resize(size_t old_capacity, size_t new_capacity) {
    if (new_capacity > old_capacity) {
        g_buffer_bytes.fetch_add(new_capacity - old_capacity);
    } else if (new_capacity < old_capacity) {
        g_buffer_bytes.fetch_sub(old_capacity - new_capacity);
    }
}
}  // namespace MemoryTracking