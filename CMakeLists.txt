cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 0.1 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include headers
include_directories(include)

# Source files
file(GLOB SOURCES "src/*.cpp")

# Create executable
add_executable(swazi ${SOURCES})

# Optional: link external libs here
# target_link_libraries(SwaziLang PRIVATE <lib>)