
#!/bin/bash

# --- Robust Code Formatting Script using find and clang-format ---

echo "Starting robust code formatting with clang-format..."

# Find all relevant files and pass them safely to clang-format using xargs
# -type f: Only look for files (not directories)
# -o: The OR operator, allowing us to combine search patterns

find . -type f \( \
    -path "./src/*" \( -name "*.cpp" -o -name "*.cc" -o -name "*.c" \) -o \
    -path "./include/*" \( -name "*.hpp" -o -name "*.h" \) \
\) -print0 | xargs -0 ${CLANG_FORMAT_EXE:-clang-format} -i

# Explanation of the find command:
# 1. find .: Start search in the current directory.
# 2. -type f: Only match regular files.
# 3. \( ... \): Groups the following search criteria.
# 4. -path "./src/*" \( ... \): Finds files under src/ matching the C/C++ patterns.
# 5. -o: OR operator.
# 6. -path "./include/*" \( ... \): Finds files under include/ matching the header patterns.
# 7. -print0: Prints results separated by null characters (safely handles spaces/special chars in filenames).
# 8. | xargs -0 ... -i: Passes the null-separated list to clang-format -i.
# 9. ${CLANG_FORMAT_EXE:-clang-format}: Allows running 'clang-format' directly, but provides a fallback if a variable was set.

echo "Formatting complete."
