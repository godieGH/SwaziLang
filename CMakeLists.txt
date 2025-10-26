cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 1.15.8 LANGUAGES C CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

add_library(linenoise thirdparty/linenoise/linenoise.c)
target_include_directories(linenoise PUBLIC thirdparty/linenoise/)

# Create executable target
add_executable(swazi ${SOURCES})
# Include headers for this target
target_include_directories(swazi PRIVATE include)

# make the project version available as SWAZI_VERSION (a C string) to the compiler
# use the PROJECT_VERSION variable set by `project(... VERSION ...)`
target_compile_definitions(swazi PRIVATE SWAZI_VERSION=\"${PROJECT_VERSION}\")



find_package(CURL QUIET)

if (CURL_FOUND)
    message(STATUS "libcurl found: enabling builtin http.get and linking libcurl")
    # Ensure the compile unit sees HAVE_LIBCURL so the builtins unit compiles the real implementation.
    target_compile_definitions(swazi PRIVATE HAVE_LIBCURL=1)
    # Link the libcurl library to the swazi target
    target_link_libraries(swazi PRIVATE CURL::libcurl)
else()
    message(WARNING "libcurl not found: http builtin will use a stub that throws at runtime. To enable HTTP, install libcurl development package and re-run CMake.")
endif()




# Optional: link external libraries
# target_link_libraries(swazi PRIVATE <lib>)

target_link_libraries(swazi PRIVATE linenoise)
