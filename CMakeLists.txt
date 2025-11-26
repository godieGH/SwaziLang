cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 2.21.0 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp" "src/*.cc" "src/*.c")

add_library(linenoise thirdparty/linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC thirdparty/linenoise/)

file(
  GLOB_RECURSE
  NETSOURCE
  "src/evaluator/modules_builtins/net_module/*.cc"
  "src/evaluator/modules_builtins/net_module/*.cpp"
  "src/evaluator/modules_builtins/net_module/*.c"
  )
add_library(swazi_net ${NETSOURCE})
target_include_directories(swazi_net
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
    PRIVATE 
        ${CMAKE_SOURCE_DIR}/src/evaluator/modules_builtins/net_module
)
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
target_link_libraries(swazi PRIVATE swazi_net)

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
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ===== libsodium Setup =====
include(ExternalProject)

set(LIBSODIUM_PREFIX "${CMAKE_BINARY_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${LIBSODIUM_PREFIX}/install")

# Create the directories that will exist after build (prevents CMake error)
file(MAKE_DIRECTORY "${LIBSODIUM_INSTALL_DIR}/include")
file(MAKE_DIRECTORY "${LIBSODIUM_INSTALL_DIR}/lib")

ExternalProject_Add(
    libsodium_build
    URL https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz
    URL_HASH SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
    PREFIX ${LIBSODIUM_PREFIX}
    CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${LIBSODIUM_INSTALL_DIR} --disable-shared --enable-static --enable-minimal
    BUILD_COMMAND make -j4
    INSTALL_COMMAND make install
    BUILD_IN_SOURCE 0
)

# Create imported target for libsodium
add_library(sodium STATIC IMPORTED GLOBAL)
set_property(TARGET sodium PROPERTY IMPORTED_LOCATION "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a")
set_property(TARGET sodium PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${LIBSODIUM_INSTALL_DIR}/include")
add_dependencies(sodium libsodium_build)

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
    HAVE_LIBSODIUM=1
)
target_link_libraries(swazi_lib PUBLIC linenoise uv sodium)

if (CURL_FOUND)
    target_compile_definitions(swazi_lib PUBLIC HAVE_LIBCURL=1)
    target_link_libraries(swazi_lib PUBLIC CURL::libcurl)
endif()

target_sources(swazi_lib PRIVATE "${GENERATED_BUILTIN_CPP}")
add_dependencies(swazi_lib generate_builtins)

# Link libsodium and set compile definitions for swazi executable
target_compile_definitions(swazi PRIVATE HAVE_LIBSODIUM=1)
target_link_libraries(swazi PRIVATE sodium)

# Update swazi executable to use the library
target_link_libraries(swazi PRIVATE swazi_lib)

# Create test executable
add_executable(swazi_tests ${TEST_SOURCES})
target_link_libraries(swazi_tests PRIVATE swazi_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(swazi_tests)