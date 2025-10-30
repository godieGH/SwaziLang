cmake_minimum_required(VERSION 3.25)
project(SwaziLang VERSION 2.3.0 LANGUAGES C CXX)

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

# --- BEGIN embed builtins: regenerate when any lib/*.sl or lib/*.swz changes ---
find_package(Python COMPONENTS Interpreter REQUIRED)

set(BUILTIN_LIB_DIR "${CMAKE_SOURCE_DIR}/lib")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(GENERATED_BUILTIN_CPP "${GENERATED_DIR}/builtin_sl.cpp")

file(MAKE_DIRECTORY "${GENERATED_DIR}")

# Glob lib files and make the configure step re-run if files are added/removed.
# CMake's CONFIGURE_DEPENDS makes CMake re-evaluate the glob if the directory contents change.
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

# Target that ensures generated file exists. Not necessary to be ALL but it's safe.
add_custom_target(generate_builtins DEPENDS "${GENERATED_BUILTIN_CPP}")

# Ensure the generated directory is visible for includes if generator emits headers there
target_include_directories(swazi PRIVATE "${GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/src/evaluator")

# Add the generated cpp into the swazi target sources and ensure generation happens before building
target_sources(swazi PRIVATE "${GENERATED_BUILTIN_CPP}")
add_dependencies(swazi generate_builtins)
# --- END embed builtins ---