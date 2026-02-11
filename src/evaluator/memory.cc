#include "memory_tracking.hpp"

namespace MemoryTracking {
std::atomic<size_t> g_object_count{0};
std::atomic<size_t> g_array_count{0};
std::atomic<size_t> g_function_count{0};
std::atomic<size_t> g_promise_count{0};
std::atomic<size_t> g_generator_count{0};
std::atomic<size_t> g_buffer_count{0};
std::atomic<size_t> g_file_count{0};
std::atomic<size_t> g_class_count{0};
std::atomic<size_t> g_proxy_count{0};
std::atomic<size_t> g_regex_count{0};
std::atomic<size_t> g_datetime_count{0};
std::atomic<size_t> g_range_count{0};
std::atomic<size_t> g_map_count{0};

std::atomic<size_t> g_buffer_bytes{0};
std::atomic<size_t> g_string_bytes{0};
}  // namespace MemoryTracking