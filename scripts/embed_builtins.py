#!/usr/bin/env python3
# scripts/embed_builtins.py
# Build-time generator: embed .sl/.swz files from lib/ into a C++ source containing
# a static map<string,string>. The generator performs an atomic update and will
# avoid touching the output file if the generated contents are identical.
#
# Usage:
#   python3 scripts/embed_builtins.py <lib_dir> <out_cpp>
#
# The generated C++ implements:
#   bool has_embedded_module(const std::string &spec);
#   std::optional<std::string> get_embedded_module_source(const std::string &spec);
#
# Notes:
# - The generator writes to a temporary file and then replaces the output only if different.
# - Keys emitted: relative path under lib/ (e.g. "std.sl" or "pkg/foo.sl"),
#   the same without extension (e.g. "std"), and a "swazi:<noext>" alias.

import sys
from pathlib import Path
import argparse
import tempfile
import os

def collect_files(lib_dir: Path):
    pairs = []
    if not lib_dir.exists():
        return pairs
    for p in sorted(lib_dir.rglob('*')):
        if p.is_file() and (p.suffix == '.sl' or p.suffix == '.swz'):
            rel = p.relative_to(lib_dir).as_posix()
            try:
                data = p.read_text(encoding='utf-8')
            except Exception:
                data = p.read_text(encoding='utf-8', errors='replace')
            pairs.append((rel, data))
    return pairs

def cpp_escape(s: str) -> str:
    # Escape backslashes and quotes, normalize newlines, break long literal into fragments
    s = s.replace('\\', '\\\\')
    s = s.replace('"', '\\"')
    s = s.replace('\r', '')
    # Replace newline with explicit escape and split into separate literal fragments for readability
    s = s.replace('\n', '\\n"\n"')
    return s

def generate_content(pairs):
    parts = []
    parts.append('// GENERATED FILE - do not edit. Regenerate with scripts/embed_builtins.py\n')
    parts.append('#include "builtin_sl.h"\n')
    parts.append('#include <unordered_map>\n#include <string>\n#include <optional>\n\n')
    parts.append('using namespace std;\n\n')
    parts.append('static unordered_map<string,string> __embedded_modules = {\n')
    for rel, data in pairs:
        esc = cpp_escape(data)
        parts.append(f'    {{ "{rel}", "{esc}" }},\n')
        if rel.endswith('.sl') or rel.endswith('.swz'):
            noext = rel.rsplit('.', 1)[0]
            parts.append(f'    {{ "{noext}", "{esc}" }},\n')
            parts.append(f'    {{ "swazi:{noext}", "{esc}" }},\n')
    parts.append('};\n\n')
    parts.append('bool has_embedded_module(const std::string &spec) {\n')
    parts.append('    return __embedded_modules.find(spec) != __embedded_modules.end();\n')
    parts.append('}\n\n')
    parts.append('std::optional<std::string> get_embedded_module_source(const std::string &spec) {\n')
    parts.append('    auto it = __embedded_modules.find(spec);\n')
    parts.append('    if (it == __embedded_modules.end()) return std::nullopt;\n')
    parts.append('    return it->second;\n')
    parts.append('}\n')
    return ''.join(parts)

def atomic_write_if_changed(path: Path, content: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    # If file exists and is identical, do nothing
    if path.exists():
        try:
            existing = path.read_text(encoding='utf-8')
            if existing == content:
                print(f"No changes for {path}, skipping rewrite.")
                return False
        except Exception:
            # Fall through to write if reading fails
            pass
    # Write to temp file in same directory then atomically move
    fd, tmpname = tempfile.mkstemp(prefix=path.name, dir=str(path.parent))
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as tmpf:
            tmpf.write(content)
        # atomic replace
        os.replace(tmpname, str(path))
        print(f"Wrote {path}")
        return True
    finally:
        if os.path.exists(tmpname):
            try:
                os.unlink(tmpname)
            except Exception:
                pass

def main():
    parser = argparse.ArgumentParser(description="Embed SwaziLang lib files into a generated C++ source.")
    parser.add_argument('lib_dir', type=str, help='directory with .sl/.swz files (e.g. lib/)')
    parser.add_argument('out_cpp', type=str, help='generated C++ output path (e.g. build/generated/builtin_sl.cpp)')
    args = parser.parse_args()

    lib_dir = Path(args.lib_dir)
    out_cpp = Path(args.out_cpp)

    pairs = collect_files(lib_dir)
    content = generate_content(pairs)
    changed = atomic_write_if_changed(out_cpp, content)
    if not changed:
        # exit 0 still: nothing changed
        sys.exit(0)
    sys.exit(0)

if __name__ == '__main__':
    main()