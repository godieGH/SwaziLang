cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 2.14.0 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.cc" "src/*.c")

add_library(linenoise thirdparty/linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC thirdparty/linenoise/)

# Create executable target
add_executable(swazi ${SOURCES})
# Include headers for this target
target_include_directories(swazi PRIVATE include thirdparty/libuv-1.51.0/include)

# make the project version available as SWAZI_VERSION (a C string) to the compiler
# use the PROJECT_VERSION variable set by `project(... VERSION ...)`
target_compile_definitions(swazi PRIVATE SWAZI_VERSION=\"${PROJECT_VERSION}\")

find_package(CURL QUIET)

if (CURL_FOUND)
    message(STATUS "libcurl found: enabling builtin http.get and linking libcurl")
    target_compile_definitions(swazi PRIVATE HAVE_LIBCURL=1)
    target_link_libraries(swazi PRIVATE CURL::libcurl)
else()
    message(WARNING "libcurl not found: http builtin will use a stub that throws at runtime. To enable HTTP, install libcurl development package and re-run CMake.")
endif()

target_link_libraries(swazi PRIVATE linenoise)

# --- Build and include local libuv from the thirdparty directory ---
add_subdirectory(thirdparty/libuv-1.51.0)
target_link_libraries(swazi PRIVATE uv)
target_compile_definitions(swazi PRIVATE HAVE_LIBUV=1)
# --- End local libuv integration ---

# --- BEGIN embed builtins: regenerate when any lib/*.sl or lib/*.swz changes ---
find_package(Python COMPONENTS Interpreter REQUIRED)

set(BUILTIN_LIB_DIR "${CMAKE_SOURCE_DIR}/lib")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(GENERATED_BUILTIN_CPP "${GENERATED_DIR}/builtin_sl.cpp")

file(MAKE_DIRECTORY "${GENERATED_DIR}")

file(GLOB_RECURSE EMBED_LIB_FILES
     CONFIGURE_DEPENDS
     "${BUILTIN_LIB_DIR}/*.sl"
     "${BUILTIN_LIB_DIR}/*.swz"
)

add_custom_command(
  OUTPUT "${GENERATED_BUILTIN_CPP}"
  COMMAND ${Python_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/embed_builtins.py" "${BUILTIN_LIB_DIR}" "${GENERATED_BUILTIN_CPP}"
  DEPENDS "${CMAKE_SOURCE_DIR}/scripts/embed_builtins.py" ${EMBED_LIB_FILES}
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Embedding .sl/.swz files from ${BUILTIN_LIB_DIR} into ${GENERATED_BUILTIN_CPP}"
  VERBATIM
)

add_custom_target(generate_builtins DEPENDS "${GENERATED_BUILTIN_CPP}")

target_include_directories(swazi PRIVATE "${GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/src/evaluator")
target_sources(swazi PRIVATE "${GENERATED_BUILTIN_CPP}")
add_dependencies(swazi generate_builtins)
# --- END embed builtins ---

# --- INSTALLATION RULES ---
install(TARGETS swazi DESTINATION bin)

# ===== Google Test Setup =====
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.12.1.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

# Collect all test files
file(GLOB_RECURSE TEST_SOURCES "tests/*.cc")

# Create a library from sources (excluding main if you have one)
file(GLOB_RECURSE LIB_SOURCES "src/*.cpp" "src/*.cc" "src/*.c")
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*main\\.cpp$")

add_library(swazi_lib ${LIB_SOURCES})
target_include_directories(swazi_lib PUBLIC 
    include 
    thirdparty/libuv-1.51.0/include 
    "${GENERATED_DIR}" 
    "${CMAKE_SOURCE_DIR}/src/evaluator"
)
target_compile_definitions(swazi_lib PUBLIC 
    SWAZI_VERSION=\"${PROJECT_VERSION}\" 
    HAVE_LIBUV=1
)
target_link_libraries(swazi_lib PUBLIC linenoise uv)

if (CURL_FOUND)
    target_compile_definitions(swazi_lib PUBLIC HAVE_LIBCURL=1)
    target_link_libraries(swazi_lib PUBLIC CURL::libcurl)
endif()

target_sources(swazi_lib PRIVATE "${GENERATED_BUILTIN_CPP}")
add_dependencies(swazi_lib generate_builtins)

# Update swazi executable to use the library
target_link_libraries(swazi PRIVATE swazi_lib)

# Create test executable
add_executable(swazi_tests ${TEST_SOURCES})
target_link_libraries(swazi_tests PRIVATE swazi_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(swazi_tests)