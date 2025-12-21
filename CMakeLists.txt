cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 2.28.1 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Linenoise library
add_library(linenoise thirdparty/linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC thirdparty/linenoise/)

# Network module library
file(GLOB_RECURSE NETSOURCE
  "src/evaluator/modules_builtins/net_module/*.cc"
  "src/evaluator/modules_builtins/net_module/*.cpp"
  "src/evaluator/modules_builtins/net_module/*.c"
)
add_library(swazi_net ${NETSOURCE})
target_include_directories(swazi_net
    PUBLIC ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/evaluator/modules_builtins/net_module
)

# begining of format lib
file(GLOB_RECURSE FORMATSOURCE "src/format/*.cc" "src/format/*.cpp")
add_library(swazi_format SHARED ${FORMATSOURCE})
target_include_directories(swazi_format PUBLIC ${CMAKE_SOURCE_DIR}/include)

# --- BEGIN embed builtins ---
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
# --- END embed builtins ---

# --- libsodium Setup ---
include(ExternalProject)

set(LIBSODIUM_PREFIX "${CMAKE_BINARY_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${LIBSODIUM_PREFIX}/install")

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

add_library(sodium STATIC IMPORTED GLOBAL)
set_property(TARGET sodium PROPERTY IMPORTED_LOCATION "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a")
set_property(TARGET sodium PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${LIBSODIUM_INSTALL_DIR}/include")
add_dependencies(sodium libsodium_build)

# --- libuv Setup ---
add_subdirectory(thirdparty/libuv-1.51.0)

# --- nlohmann/json Setup ---
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# --- CORE LIBRARY (compile everything once here) ---
file(GLOB_RECURSE LIB_SOURCES "src/*.cpp" "src/*.cc" "src/*.c")
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*main\\.cpp$")
# Exclude the network module sources to avoid double-compilation
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/evaluator/modules_builtins/net_module/.*")
# Exclude swazi_format too
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/format/.*")
add_library(swazi_lib ${LIB_SOURCES} "${GENERATED_BUILTIN_CPP}")

target_include_directories(swazi_lib PUBLIC 
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/thirdparty/libuv-1.51.0/include
    ${GENERATED_DIR}
    ${CMAKE_SOURCE_DIR}/src/evaluator
)

target_compile_definitions(swazi_lib PUBLIC 
    SWAZI_VERSION=\"${PROJECT_VERSION}\"
    HAVE_LIBUV=1
    HAVE_LIBSODIUM=1
)

target_link_libraries(swazi_lib PUBLIC 
    linenoise
    swazi_net
    swazi_format
    uv
    sodium
    nlohmann_json::nlohmann_json
)

add_dependencies(swazi_lib generate_builtins)

# Optional libcurl
find_package(CURL QUIET)
if (CURL_FOUND)
    message(STATUS "libcurl found: enabling builtin http.get")
    target_compile_definitions(swazi_lib PUBLIC HAVE_LIBCURL=1)
    target_link_libraries(swazi_lib PUBLIC CURL::libcurl)
else()
    message(WARNING "libcurl not found: http builtin will use a stub")
endif()

# Optional zlib
find_package(ZLIB QUIET)
if (ZLIB_FOUND)
    message(STATUS "zlib found: enabling archiver compression features")
    target_compile_definitions(swazi_lib PUBLIC HAVE_ZLIB=1)
    target_link_libraries(swazi_lib PUBLIC ZLIB::ZLIB)
else()
    message(WARNING "zlib not found: archiver will use stubs for compression")
endif()

# --- MAIN EXECUTABLE (just main.cpp, links to library) ---
add_executable(swazi src/main.cpp)
target_link_libraries(swazi PRIVATE swazi_lib)

# --- INSTALLATION ---
install(TARGETS swazi DESTINATION bin)

# --- TESTING ---
enable_testing()

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE TEST_SOURCES "tests/*.cc")

add_executable(swazi_tests ${TEST_SOURCES})
target_link_libraries(swazi_tests PRIVATE swazi_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(swazi_tests)