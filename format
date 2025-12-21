#!/bin/bash

# --- Robust Code Formatting Script using find and clang-format ---

echo "Starting robust code formatting with clang-format..."

# Find all relevant files and pass them safely to clang-format using xargs
find . -type f \( \
    -path "./src/*" \( -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) -o \
    -path "./include/*" \( -name "*.hpp" -o -name "*.h" \) -o \
    -path "./tests/*" \( -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) \
\) -print0 | xargs -0 ${CLANG_FORMAT_EXE:-clang-format} -i

echo "Formatting complete."