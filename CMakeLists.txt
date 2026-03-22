cmake_minimum_required(VERSION 3.28)
project(SwaziLang VERSION 3.3.5 LANGUAGES C CXX)

# Standard install paths (bin, lib, include, etc.)
include(GNUInstallDirs)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# RPATH (Linux / macOS only — Windows uses PATH / DLL-next-to-exe instead)
# ---------------------------------------------------------------------------
# Project-owned shared libs (swazi_format, llhttp, libuv) live in a private
# subdirectory lib/swazi/ so they never conflict with system packages that
# might own libuv.so or libllhttp.so in the global lib/ directory.
# The installed binary's RPATH points here so it always finds them.
set(SWAZI_PRIVATE_LIBDIR "${CMAKE_INSTALL_LIBDIR}/swazi")

if(NOT WIN32)
    set(CMAKE_SKIP_BUILD_RPATH          FALSE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH  FALSE)
    set(CMAKE_INSTALL_RPATH             "${CMAKE_INSTALL_PREFIX}/${SWAZI_PRIVATE_LIBDIR}")
    set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

# --- linenoise (static, compiled into swazi_lib, no runtime artifact) ---
add_library(linenoise STATIC thirdparty/linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC thirdparty/linenoise/)

# --- re2 (static via FetchContent, compiled in, no runtime artifact) ---
include(FetchContent)
FetchContent_Declare(
    re2
    GIT_REPOSITORY https://github.com/google/re2.git
    GIT_TAG        2023-11-01
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(re2)

# --- libsodium (static, compiled in, no runtime artifact) ---
# Unix:    uses its autotools configure + make (no CMake build available)
# Windows: uses CMake build via MSVC-compatible path
include(ExternalProject)

set(LIBSODIUM_PREFIX      "${CMAKE_BINARY_DIR}/libsodium")
set(LIBSODIUM_INSTALL_DIR "${LIBSODIUM_PREFIX}/install")

file(MAKE_DIRECTORY "${LIBSODIUM_INSTALL_DIR}/include")
file(MAKE_DIRECTORY "${LIBSODIUM_INSTALL_DIR}/lib")

if(WIN32)
    # libsodium ships a separate CMake-compatible build under builds/msvc.
    # We use the bundled CMakeLists that the community maintains for Windows.
    ExternalProject_Add(
        libsodium_build
        URL      https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz
        URL_HASH SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
        PREFIX   ${LIBSODIUM_PREFIX}
        CMAKE_ARGS
            -DCMAKE_INSTALL_PREFIX=${LIBSODIUM_INSTALL_DIR}
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DSODIUM_DISABLE_TESTS=ON
            -DBUILD_SHARED_LIBS=OFF
        BUILD_COMMAND   ${CMAKE_COMMAND} --build . --config ${CMAKE_BUILD_TYPE}
        INSTALL_COMMAND ${CMAKE_COMMAND} --build . --config ${CMAKE_BUILD_TYPE} --target install
        BUILD_IN_SOURCE 0
    )
    # On Windows the static lib is libsodium.lib
    set(LIBSODIUM_LIB "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.lib")
else()
    # Unix: autotools build — configure + make
    ExternalProject_Add(
        libsodium_build
        URL      https://github.com/jedisct1/libsodium/releases/download/1.0.20-RELEASE/libsodium-1.0.20.tar.gz
        URL_HASH SHA256=ebb65ef6ca439333c2bb41a0c1990587288da07f6c7fd07cb3a18cc18d30ce19
        PREFIX   ${LIBSODIUM_PREFIX}
        CONFIGURE_COMMAND <SOURCE_DIR>/configure
            --prefix=${LIBSODIUM_INSTALL_DIR}
            --disable-shared
            --enable-static
            --enable-minimal
        BUILD_COMMAND   make -j4
        INSTALL_COMMAND make install
        BUILD_IN_SOURCE 0
    )
    set(LIBSODIUM_LIB "${LIBSODIUM_INSTALL_DIR}/lib/libsodium.a")
endif()

add_library(sodium STATIC IMPORTED GLOBAL)
set_property(TARGET sodium PROPERTY IMPORTED_LOCATION         "${LIBSODIUM_LIB}")
set_property(TARGET sodium PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${LIBSODIUM_INSTALL_DIR}/include")
add_dependencies(sodium libsodium_build)

# --- nlohmann/json (header-only via FetchContent, compiled in, no runtime artifact) ---
FetchContent_Declare(
    nlohmann_json
    URL      https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(nlohmann_json)

# --- libuv (shared, project-owned, built from thirdparty/) ---
# EXCLUDE_FROM_ALL: suppress libuv's own install() rules — we install it
# ourselves into the private lib/swazi/ dir below.
add_subdirectory(thirdparty/libuv-1.51.0 EXCLUDE_FROM_ALL)

# --- llhttp (shared, project-owned, built from thirdparty/) ---
# Same rationale as libuv.
set(BUILD_SHARED_LIBS ON)
set(BUILD_STATIC_LIBS OFF)
add_subdirectory(thirdparty/llhttp EXCLUDE_FROM_ALL)
set(BUILD_SHARED_LIBS OFF)

# --- System libs (optional, already installed on the machine) ---
# curl, zlib, openssl are found via find_package — the system package manager
# already owns these. shlibdeps/autoreq auto-declares them as deps at
# packaging time, so we never have to hardcode version strings.
find_package(CURL    QUIET)
find_package(ZLIB    QUIET)
find_package(OpenSSL QUIET)

# ---------------------------------------------------------------------------
# Project libraries
# ---------------------------------------------------------------------------

# --- Network module (static, compiled into swazi_lib) ---
file(GLOB_RECURSE NETSOURCE
    "src/evaluator/modules_builtins/net_module/*.cc"
    "src/evaluator/modules_builtins/net_module/*.cpp"
    "src/evaluator/modules_builtins/net_module/*.c"
)
add_library(swazi_net STATIC ${NETSOURCE})
target_include_directories(swazi_net
    PUBLIC  ${CMAKE_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/src/evaluator/modules_builtins/net_module
)
target_link_libraries(swazi_net PUBLIC re2::re2 llhttp::llhttp)

# --- Format library (shared, project-owned, installed to lib/swazi/) ---
file(GLOB_RECURSE FORMATSOURCE "src/format/*.cc" "src/format/*.cpp")
add_library(swazi_format SHARED ${FORMATSOURCE})
target_include_directories(swazi_format PUBLIC ${CMAKE_SOURCE_DIR}/include)

# --- Embed builtins ---
find_package(Python COMPONENTS Interpreter REQUIRED)

set(BUILTIN_LIB_DIR       "${CMAKE_SOURCE_DIR}/lib")
set(GENERATED_DIR         "${CMAKE_BINARY_DIR}/generated")
set(GENERATED_BUILTIN_CPP "${GENERATED_DIR}/builtin_sl.cpp")

file(MAKE_DIRECTORY "${GENERATED_DIR}")

file(GLOB_RECURSE EMBED_LIB_FILES
    CONFIGURE_DEPENDS
    "${BUILTIN_LIB_DIR}/*.sl"
    "${BUILTIN_LIB_DIR}/*.swz"
)

add_custom_command(
    OUTPUT  "${GENERATED_BUILTIN_CPP}"
    COMMAND ${Python_EXECUTABLE}
            "${CMAKE_SOURCE_DIR}/scripts/embed_builtins.py"
            "${BUILTIN_LIB_DIR}"
            "${GENERATED_BUILTIN_CPP}"
    DEPENDS "${CMAKE_SOURCE_DIR}/scripts/embed_builtins.py" ${EMBED_LIB_FILES}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Embedding .sl/.swz files into ${GENERATED_BUILTIN_CPP}"
    VERBATIM
)
add_custom_target(generate_builtins DEPENDS "${GENERATED_BUILTIN_CPP}")

# --- Core library (static, aggregates everything) ---
file(GLOB_RECURSE LIB_SOURCES "src/*.cpp" "src/*.cc" "src/*.c")
list(FILTER LIB_SOURCES EXCLUDE REGEX ".*main\\.cpp$")
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/evaluator/modules_builtins/net_module/.*")
list(FILTER LIB_SOURCES EXCLUDE REGEX "src/format/.*")

add_library(swazi_lib STATIC ${LIB_SOURCES} "${GENERATED_BUILTIN_CPP}")
add_dependencies(swazi_lib generate_builtins)

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
    re2::re2
    llhttp::llhttp
)

if(CURL_FOUND)
    message(STATUS "libcurl found: enabling builtin http.get")
    target_compile_definitions(swazi_lib PUBLIC HAVE_LIBCURL=1)
    target_link_libraries(swazi_lib PUBLIC CURL::libcurl)
else()
    message(WARNING "libcurl not found: http builtin will use a stub")
endif()

if(ZLIB_FOUND)
    message(STATUS "zlib found: enabling archiver compression features")
    target_compile_definitions(swazi_lib PUBLIC HAVE_ZLIB=1)
    target_link_libraries(swazi_lib PUBLIC ZLIB::ZLIB)
else()
    message(WARNING "zlib not found: archiver will use stubs for compression")
endif()

if(OpenSSL_FOUND)
    message(STATUS "OpenSSL found: enabling TLS/crypto support")
    target_compile_definitions(swazi_lib PUBLIC HAVE_OPENSSL=1)
    target_link_libraries(swazi_lib PUBLIC OpenSSL::SSL OpenSSL::Crypto)
else()
    message(WARNING "OpenSSL not found: TLS features disabled")
endif()

# --- Main executable ---
add_executable(swazi src/main.cpp)
target_link_libraries(swazi PRIVATE swazi_lib)

# -rdynamic is a GCC/Clang linker flag that exports all symbols from the
# binary so dynamically loaded ABI modules can resolve them at runtime.
# MSVC does not need this — it uses __declspec(dllexport) instead, and the
# flag doesn't exist on Windows at all.
if(NOT WIN32)
    target_link_options(swazi PRIVATE -rdynamic)
endif()

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
# Dependency summary at install time:
#
#   linenoise     → static, compiled into binary        nothing to install
#   re2           → static, compiled in                 nothing to install
#   libsodium     → static, compiled in                 nothing to install
#   nlohmann_json → header-only, compiled in            nothing to install
#   curl          → system lib (optional)               already on machine
#   zlib          → system lib (optional)               already on machine
#   openssl       → system lib (optional)               already on machine
#   swazi_format  → project-owned SHARED → lib/swazi/   private, we own this
#   llhttp        → project-owned SHARED → lib/swazi/   built from thirdparty/
#   libuv         → project-owned SHARED → lib/swazi/   built from thirdparty/
#
# Project-owned shared libs go to lib/swazi/ (private subdir) so they can
# never conflict with any system package owning the same soname in lib/.
# On Windows they go next to the .exe in bin/ which is how Windows finds DLLs.

install(TARGETS swazi
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(TARGETS swazi_format
    LIBRARY DESTINATION ${SWAZI_PRIVATE_LIBDIR}     # .so  on Linux/macOS
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}     # .dll on Windows (next to .exe)
)

install(TARGETS llhttp_shared
    LIBRARY       DESTINATION ${SWAZI_PRIVATE_LIBDIR}
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/llhttp
)

install(TARGETS uv
    LIBRARY DESTINATION ${SWAZI_PRIVATE_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# swazi_abi.h — ABI authors can #include <swazi/swazi_abi.h> after install
install(FILES ${CMAKE_SOURCE_DIR}/include/swazi_abi.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/swazi
)

# ---------------------------------------------------------------------------
# Packaging  (make package / cmake --build . --target package)
# ---------------------------------------------------------------------------
# Usage:
#   cmake .. [-DCMAKE_INSTALL_PREFIX=<prefix>]   # only needed if not /usr/local
#   cmake --build . --target package             # or: make package
#
#   Termux / Debian / Ubuntu:  apt install ./packages/swazi-x.y.z-Android.deb
#   Fedora / RHEL:             dnf install ./packages/swazi-x.y.z-Linux.rpm
#   macOS:                     open        ./packages/swazi-x.y.z-Darwin.pkg
#   Windows:                   run         ./packages/swazi-x.y.z-win64.exe/.msi
#   Other:                     tar -xzf    ./packages/swazi-x.y.z-<OS>.tar.gz
#
#   Uninstall: apt remove swazi / dnf remove swazi / pkg remove swazi

set(CPACK_PACKAGE_NAME              "swazi")
set(CPACK_PACKAGE_VERSION           "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT           "your@email.com")
set(CPACK_PACKAGE_DESCRIPTION_SHORT "The Swazi programming language interpreter")
set(CPACK_PACKAGE_DESCRIPTION
    "Swazi is a dynamic, embeddable scripting language.\n"
    "Includes the swazi interpreter, its bundled runtime shared libraries\n"
    "(installed to a private lib/swazi/ directory to avoid system conflicts),\n"
    "and the swazi_abi.h header for ABI module authors.")

# Tie the package install prefix to whatever was passed to cmake.
# Defaults to /usr/local on Linux/macOS. On Termux pass:
#   cmake .. -DCMAKE_INSTALL_PREFIX=$PREFIX
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_OUTPUT_FILE_PREFIX       "${CMAKE_BINARY_DIR}/packages")

# Platform detection.
# Termux must come first — it reports CMAKE_SYSTEM_NAME="Android" not "Linux"
# and would otherwise fall through to the TGZ fallback.
if(DEFINED ENV{TERMUX_VERSION} OR DEFINED ENV{TERMUX_PREFIX})
    # --- Termux (Android, uses apt/dpkg) ---
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_SECTION  "interpreters")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    # shlibdeps inspects the binary and auto-generates Depends: for system
    # libs (curl, zlib, openssl). Our private libs in lib/swazi/ are bundled
    # inside the package so they don't appear as unresolved system deps.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(EXISTS "/etc/debian_version")
        # --- Debian / Ubuntu ---
        set(CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_SECTION  "interpreters")
        set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    elseif(EXISTS "/etc/redhat-release" OR EXISTS "/etc/fedora-release")
        # --- Fedora / RHEL / CentOS / openSUSE ---
        set(CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_GROUP   "Development/Languages")
        set(CPACK_RPM_PACKAGE_LICENSE "MIT")
        # autoreq is the RPM equivalent of shlibdeps
        set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
    else()
        # --- Unknown Linux — self-contained tarball ---
        set(CPACK_GENERATOR "TGZ")
    endif()

elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # --- macOS ---
    set(CPACK_GENERATOR "productbuild")
    set(CPACK_PRODUCTBUILD_IDENTIFIER "com.swazilang.swazi")

elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # --- Windows: prefer WIX (.msi), fall back to NSIS (.exe) ---
    find_program(WIX_CANDLE candle)
    if(WIX_CANDLE)
        set(CPACK_GENERATOR "WIX")
        # Generate a real UUID with: uuidgen
        # This GUID identifies the product for upgrades — never change it
        # after first release or Windows will treat upgrades as new installs.
        set(CPACK_WIX_UPGRADE_GUID "PUT-A-REAL-UUID-HERE")
    else()
        set(CPACK_GENERATOR "NSIS")
        set(CPACK_NSIS_MODIFY_PATH ON)
    endif()

else()
    # --- Any other OS — self-contained tarball ---
    set(CPACK_GENERATOR "TGZ")
endif()

message(STATUS "CPack generator: ${CPACK_GENERATOR}")
include(CPack)

# ---------------------------------------------------------------------------
# Testing
# ---------------------------------------------------------------------------
enable_testing()

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/releases/download/v1.17.0/googletest-1.17.0.tar.gz
    EXCLUDE_FROM_ALL
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE TEST_SOURCES "tests/*.cc")
add_executable(swazi_tests ${TEST_SOURCES})
target_link_libraries(swazi_tests PRIVATE swazi_lib GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(swazi_tests)
