#!/bin/bash

# cfmt.sh - Robust C/C++ Code Formatter
# Usage: cfmt.sh [options]
# Options:
#   --dry-run    Show what would be formatted without making changes
#   --verbose    Show detailed output
#   --help       Show this help message

set -e

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Parse command line arguments
DRY_RUN=false
VERBOSE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --dry-run)
            DRY_RUN=true
            shift
            ;;
        --verbose)
            VERBOSE=true
            shift
            ;;
        --help)
            echo "Usage: cfmt.sh [options]"
            echo "Options:"
            echo "  --dry-run    Show what would be formatted without making changes"
            echo "  --verbose    Show detailed output"
            echo "  --help       Show this help message"
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option $1${NC}"
            exit 1
            ;;
    esac
done

# Function to print colored messages
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_verbose() {
    if [ "$VERBOSE" = true ]; then
        echo -e "${BLUE}[VERBOSE]${NC} $1"
    fi
}

# Find project root by looking for CMakeLists.txt or .cfmtconfig
find_project_root() {
    local current_dir="$(pwd)"
    local search_dir="$current_dir"
    
    print_verbose "Starting search from: $current_dir"
    
    # Search upwards for project markers
    while [ "$search_dir" != "/" ]; do
        print_verbose "Checking directory: $search_dir"
        
        # Check for .cfmtconfig first (highest priority)
        if [ -f "$search_dir/.cfmtconfig" ]; then
            print_verbose "Found .cfmtconfig in: $search_dir"
            echo "$search_dir"
            return 0
        fi
        
        # Check for CMakeLists.txt
        if [ -f "$search_dir/CMakeLists.txt" ]; then
            print_verbose "Found CMakeLists.txt in: $search_dir"
            echo "$search_dir"
            return 0
        fi
        
        search_dir="$(dirname "$search_dir")"
    done
    
    return 1
}

# Parse .cfmtconfig file
parse_config() {
    local config_file="$1"
    local key="$2"
    
    if [ -f "$config_file" ]; then
        # Read the config file and extract the value for the given key
        grep "^${key}=" "$config_file" | cut -d'=' -f2- | sed 's/^[ \t]*//;s/[ \t]*$//'
    fi
}

# Build find command with exclusions
build_find_command() {
    local project_root="$1"
    local exclude_dirs="$2"
    local exclude_files="$3"
    
    local find_cmd="find \"$project_root\" -type f"
    
    # Add directory exclusions
    if [ -n "$exclude_dirs" ]; then
        IFS=',' read -ra DIRS <<< "$exclude_dirs"
        for dir in "${DIRS[@]}"; do
            dir=$(echo "$dir" | sed 's/^[ \t]*//;s/[ \t]*$//')
            find_cmd="$find_cmd ! -path \"*/${dir}/*\""
            print_verbose "Excluding directory: $dir"
        done
    fi
    
    # Add file pattern exclusions
    if [ -n "$exclude_files" ]; then
        IFS=',' read -ra FILES <<< "$exclude_files"
        for pattern in "${FILES[@]}"; do
            pattern=$(echo "$pattern" | sed 's/^[ \t]*//;s/[ \t]*$//')
            find_cmd="$find_cmd ! -name \"$pattern\""
            print_verbose "Excluding file pattern: $pattern"
        done
    fi
    
    # Add C/C++ file extensions
    find_cmd="$find_cmd \\( -name \"*.c\" -o -name \"*.cpp\" -o -name \"*.cc\" -o -name \"*.cxx\" -o -name \"*.h\" -o -name \"*.hpp\" -o -name \"*.hh\" -o -name \"*.hxx\" \\)"
    
    echo "$find_cmd"
}

# Main script starts here
print_info "cfmt - C/C++ Code Formatter"
echo ""

# Find project root
print_info "Searching for C/C++ project root..."
PROJECT_ROOT=$(find_project_root)

if [ $? -ne 0 ] || [ -z "$PROJECT_ROOT" ]; then
    print_error "No C/C++ project found!"
    print_error "Looked for CMakeLists.txt or .cfmtconfig file in parent directories."
    echo ""
    echo "To mark a directory as a C/C++ project, create a .cfmtconfig file in the project root."
    echo "Example .cfmtconfig:"
    echo ""
    echo "  # Directories to exclude (comma-separated)"
    echo "  exclude_dirs=build,third_party,external"
    echo "  # File patterns to exclude (comma-separated)"
    echo "  exclude_files=*_generated.h,*.pb.cc"
    echo "  # Custom clang-format executable (optional)"
    echo "  clang_format=clang-format-14"
    echo ""
    exit 1
fi

print_success "Found project root: $PROJECT_ROOT"

# Check for .cfmtconfig
CONFIG_FILE="$PROJECT_ROOT/.cfmtconfig"
EXCLUDE_DIRS=""
EXCLUDE_FILES=""
CLANG_FORMAT_CMD="${CLANG_FORMAT_EXE:-clang-format}"

if [ -f "$CONFIG_FILE" ]; then
    print_info "Loading configuration from .cfmtconfig"
    EXCLUDE_DIRS=$(parse_config "$CONFIG_FILE" "exclude_dirs")
    EXCLUDE_FILES=$(parse_config "$CONFIG_FILE" "exclude_files")
    CUSTOM_CLANG_FORMAT=$(parse_config "$CONFIG_FILE" "clang_format")
    
    if [ -n "$CUSTOM_CLANG_FORMAT" ]; then
        CLANG_FORMAT_CMD="$CUSTOM_CLANG_FORMAT"
    fi
    
    if [ -n "$EXCLUDE_DIRS" ]; then
        print_verbose "Excluded directories: $EXCLUDE_DIRS"
    fi
    if [ -n "$EXCLUDE_FILES" ]; then
        print_verbose "Excluded files: $EXCLUDE_FILES"
    fi
else
    print_info "No .cfmtconfig found, using default settings"
    # Default exclusions
    EXCLUDE_DIRS="build,cmake-build-debug,cmake-build-release,.git"
fi

# Check if clang-format is available
if ! command -v "$CLANG_FORMAT_CMD" &> /dev/null; then
    print_error "clang-format not found!"
    print_error "Please install clang-format or set CLANG_FORMAT_EXE environment variable"
    exit 1
fi

print_info "Using clang-format: $CLANG_FORMAT_CMD"

# Build and execute find command
print_info "Scanning for C/C++ files..."
FIND_CMD=$(build_find_command "$PROJECT_ROOT" "$EXCLUDE_DIRS" "$EXCLUDE_FILES")
print_verbose "Find command: $FIND_CMD"

# Get list of files
FILE_LIST=$(eval "$FIND_CMD -print0" | xargs -0 -n1 2>/dev/null)
FILE_COUNT=$(echo "$FILE_LIST" | grep -c "^" 2>/dev/null || echo "0")

if [ "$FILE_COUNT" -eq 0 ]; then
    print_warning "No C/C++ files found to format"
    exit 0
fi

print_info "Found $FILE_COUNT files to format"

if [ "$DRY_RUN" = true ]; then
    print_warning "DRY RUN mode - no files will be modified"
    echo ""
    echo "Files that would be formatted:"
    echo "$FILE_LIST"
    exit 0
fi

# Format the files
echo ""
print_info "Formatting files..."
echo "$FILE_LIST" | while IFS= read -r file; do
    if [ -n "$file" ]; then
        print_verbose "Formatting: $file"
        "$CLANG_FORMAT_CMD" -i "$file"
    fi
done

print_success "Formatting complete! Formatted $FILE_COUNT files."