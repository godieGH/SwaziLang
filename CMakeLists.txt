cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 1.11.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Create executable target
add_executable(swazi ${SOURCES})

# Include headers for this target
target_include_directories(swazi PRIVATE include)

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
