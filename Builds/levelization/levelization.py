#!/usr/bin/env python3

"""
Usage: levelization.py
This script takes no parameters, and can be called from any directory in the file system.
"""

import os
import re
import sys
from collections import defaultdict
from pathlib import Path

# Compile regex patterns once at module level
INCLUDE_PATTERN = re.compile(r"^\s*#include.*/.*\.h")
INCLUDE_PATH_PATTERN = re.compile(r'[<"]([^>"]+)[>"]')


def dictionary_sort_key(s):
    """
    Create a sort key that mimics 'sort -d' (dictionary order).
    Dictionary order only considers blanks and alphanumeric characters.
    """
    return "".join(c for c in s if c.isalnum() or c.isspace())


def get_level(file_path):
    """
    Extract the level from a file path (second and third directory components).
    Equivalent to bash: cut -d/ -f 2,3

    Examples:
        src/ripple/app/main.cpp -> ripple.app
        src/test/app/Import_test.cpp -> test.app
    """
    parts = file_path.split("/")

    if len(parts) >= 3:
        level = f"{parts[1]}/{parts[2]}"
    elif len(parts) >= 2:
        level = f"{parts[1]}/toplevel"
    else:
        level = file_path

    # If the "level" indicates a file, cut off the filename
    if "." in level.split("/")[-1]:
        # Use the "toplevel" label as a workaround for `sort`
        # inconsistencies between different utility versions
        level = level.rsplit("/", 1)[0] + "/toplevel"

    return level.replace("/", ".")


def extract_include_level(include_line):
    """
    Extract the include path from an #include directive.
    Gets the first two directory components from the include path.
    Equivalent to bash: cut -d/ -f 1,2

    Examples:
        #include <ripple/basics/base_uint.h> -> ripple.basics
        #include "ripple/app/main/Application.h" -> ripple.app
    """
    match = INCLUDE_PATH_PATTERN.search(include_line)
    if not match:
        return None

    include_path = match.group(1)
    parts = include_path.split("/")

    if len(parts) >= 2:
        include_level = f"{parts[0]}/{parts[1]}"
    else:
        include_level = include_path

    # If the "includelevel" indicates a file, cut off the filename
    if "." in include_level.split("/")[-1]:
        include_level = include_level.rsplit("/", 1)[0] + "/toplevel"

    return include_level.replace("/", ".")


def find_repository_directories(start_path, depth_limit=10):
    """
    Find the repository root by looking for src or include folders.
    Walks up the directory tree from the start path.
    """
    current = start_path.resolve()

    for _ in range(depth_limit):
        src_path = current / "src"
        include_path = current / "include"
        has_src = src_path.exists()
        has_include = include_path.exists()

        if has_src or has_include:
            dirs = []
            if has_src:
                dirs.append(src_path)
            if has_include:
                dirs.append(include_path)
            return current, dirs

        parent = current.parent
        if parent == current:
            break
        current = parent

    raise RuntimeError(
        "Could not find repository root. "
        "Expected to find a directory containing 'src' and/or 'include' folders."
    )


def main():
    script_dir = Path(__file__).parent.resolve()
    os.chdir(script_dir)

    # Clean up and create results directory.
    results_dir = script_dir / "results"
    if results_dir.exists():
        import shutil

        shutil.rmtree(results_dir)
    results_dir.mkdir()

    # Find the repository root.
    try:
        repo_root, scan_dirs = find_repository_directories(script_dir)
        print(f"Found repository root: {repo_root}")
        for scan_dir in scan_dirs:
            print(f"  Scanning: {scan_dir.relative_to(repo_root)}")
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    # Find all #include directives.
    print("\nScanning for raw includes...")
    raw_includes = []
    rawincludes_file = results_dir / "rawincludes.txt"

    with open(rawincludes_file, "w", buffering=8192) as raw_f:
        for dir_path in scan_dirs:
            for file_path in dir_path.rglob("*"):
                if not file_path.is_file():
                    continue
                try:
                    rel_path_str = str(file_path.relative_to(repo_root))
                    with open(
                        file_path, "r", encoding="utf-8", errors="ignore", buffering=8192
                    ) as f:
                        for line in f:
                            if "#include" not in line or "boost" in line:
                                continue
                            if INCLUDE_PATTERN.match(line):
                                line_stripped = line.strip()
                                entry = f"{rel_path_str}:{line_stripped}\n"
                                print(entry, end="")
                                raw_f.write(entry)
                                raw_includes.append((rel_path_str, line_stripped))
                except Exception as e:
                    print(f"Error reading {file_path}: {e}", file=sys.stderr)

    # Build levelization paths and count directly.
    print("Build levelization paths")
    path_counts = defaultdict(int)

    for file_path, include_line in raw_includes:
        include_level = extract_include_level(include_line)
        if not include_level:
            continue
        level = get_level(file_path)
        if level != include_level:
            path_counts[(level, include_level)] += 1

    # Sort and deduplicate paths.
    print("Sort and deduplicate paths")
    sorted_items = sorted(
        path_counts.items(),
        key=lambda x: (dictionary_sort_key(x[0][0]), dictionary_sort_key(x[0][1])),
    )

    paths_file = results_dir / "paths.txt"
    with open(paths_file, "w") as f:
        for (level, include_level), count in sorted_items:
            line = f"{count:7} {level} {include_level}\n"
            print(line.rstrip())
            f.write(line)

    # Split into flat-file database.
    print("Split into flat-file database")
    includes_dir = results_dir / "includes"
    includedby_dir = results_dir / "includedby"
    includes_dir.mkdir()
    includedby_dir.mkdir()

    includes_data = defaultdict(list)
    includedby_data = defaultdict(list)

    for (level, include_level), count in sorted_items:
        includes_data[level].append((include_level, count))
        includedby_data[include_level].append((level, count))

    for level in sorted(includes_data.keys(), key=dictionary_sort_key):
        with open(includes_dir / level, "w") as f:
            for include_level, count in includes_data[level]:
                line = f"{include_level} {count}\n"
                print(line.rstrip())
                f.write(line)

    for include_level in sorted(includedby_data.keys(), key=dictionary_sort_key):
        with open(includedby_dir / include_level, "w") as f:
            for level, count in includedby_data[include_level]:
                line = f"{level} {count}\n"
                print(line.rstrip())
                f.write(line)

    # Search for loops.
    print("Search for loops")
    loops_file = results_dir / "loops.txt"
    ordering_file = results_dir / "ordering.txt"

    # Pre-load all include files into memory for fast lookup.
    includes_cache = {}
    includes_lookup = {}

    for include_file in sorted(includes_dir.iterdir(), key=lambda p: p.name):
        if not include_file.is_file():
            continue
        includes_cache[include_file.name] = []
        includes_lookup[include_file.name] = {}
        with open(include_file, "r") as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) >= 2:
                    name, count = parts[0], int(parts[1])
                    includes_cache[include_file.name].append((name, count))
                    includes_lookup[include_file.name][name] = count

    loops_found = set()

    with open(loops_file, "w", buffering=8192) as loops_f, open(
        ordering_file, "w", buffering=8192
    ) as ordering_f:
        for source in sorted(includes_cache.keys()):
            for include, include_freq in includes_cache[source]:
                if include not in includes_lookup:
                    continue

                source_freq = includes_lookup[include].get(source)

                if source_freq is not None:
                    loop_key = tuple(sorted([source, include]))
                    if loop_key in loops_found:
                        continue
                    loops_found.add(loop_key)

                    loops_f.write(f"Loop: {source} {include}\n")

                    diff = include_freq - source_freq
                    if diff > 3:
                        loops_f.write(f"  {source} > {include}\n\n")
                    elif diff < -3:
                        loops_f.write(f"  {include} > {source}\n\n")
                    elif source_freq == include_freq:
                        loops_f.write(f"  {include} == {source}\n\n")
                    else:
                        loops_f.write(f"  {include} ~= {source}\n\n")
                else:
                    ordering_f.write(f"{source} > {include}\n")

    # Print results.
    print("\nOrdering:")
    with open(ordering_file, "r") as f:
        print(f.read(), end="")

    print("\nLoops:")
    with open(loops_file, "r") as f:
        print(f.read(), end="")


if __name__ == "__main__":
    main()
