cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 1.4.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source files
file(GLOB_RECURSE SOURCES "src/*.cpp")

# Create executable target
add_executable(swazi ${SOURCES})

# Include headers for this target
target_include_directories(swazi PRIVATE include)

# Optional: link external libraries
# target_link_libraries(swazi PRIVATE <lib>)
