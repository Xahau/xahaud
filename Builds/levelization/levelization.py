#!/usr/bin/env python3
"""
Levelization generator.

Produces the same result artifacts as levelization.sh, but much faster by
doing parsing/counting in-process instead of spawning external tools in
tight loops.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import os
import posixpath
import re
import shutil
import time
from collections import Counter, defaultdict
from pathlib import Path

INCLUDE_PATTERN = re.compile(r"^\s*#include.*/.*\.h")
INCLUDE_TARGET_PATTERN = re.compile(r'.*["<]([^">]+)[">].*')
PATHS_LINE_PATTERN = re.compile(r"^\s*(\d+)\s+(\S+)\s+(\S+)\s*$")


def dictionary_sort_key(value: str) -> str:
    """Approximate `sort -d` behavior used by the shell script."""
    return "".join(ch for ch in value if ch.isalnum() or ch.isspace())


def normalize_level(value: str) -> str:
    # Match shell behavior: if level includes a file component (contains "."),
    # replace with dirname + "/toplevel".
    if "." in value:
        parent = posixpath.dirname(value) or "."
        value = f"{parent}/toplevel"
    return value.replace("/", ".")


def source_level(rel_path: str) -> str:
    parts = rel_path.split("/")
    return normalize_level("/".join(parts[1:3]))


def include_level(include_line: str) -> str | None:
    match = INCLUDE_TARGET_PATTERN.match(include_line)
    if not match:
        return None
    include_path = match.group(1)
    parts = include_path.split("/")
    return normalize_level("/".join(parts[:2]))


def scan_file(path: Path, repo_root: Path) -> tuple[list[str], list[tuple[str, str]]]:
    rel = path.relative_to(repo_root).as_posix()
    src_level = source_level(rel)

    raw_lines: list[str] = []
    paths: list[tuple[str, str]] = []

    with path.open("r", encoding="utf-8", errors="ignore") as handle:
        for line in handle:
            if "boost" in line:
                continue
            if not INCLUDE_PATTERN.match(line):
                continue

            line = line.rstrip("\n")
            raw_lines.append(f"{rel}:{line}")

            dst_level = include_level(line)
            if dst_level is None:
                continue
            if src_level != dst_level:
                paths.append((src_level, dst_level))

    return raw_lines, paths


def iter_source_files(repo_root: Path) -> list[Path]:
    files: list[Path] = []
    for top in ("include", "src"):
        root = repo_root / top
        if root.exists():
            files.extend(path for path in root.rglob("*") if path.is_file())
    files.sort(key=lambda p: p.relative_to(repo_root).as_posix())
    return files


def write_relation_db(
    results_dir: Path,
    edge_counts: list[tuple[tuple[str, str], int]],
) -> tuple[dict[str, list[tuple[str, int]]], dict[str, list[tuple[str, int]]]]:
    includes_dir = results_dir / "includes"
    includedby_dir = results_dir / "includedby"
    includes_dir.mkdir(parents=True, exist_ok=True)
    includedby_dir.mkdir(parents=True, exist_ok=True)

    includes: dict[str, list[tuple[str, int]]] = defaultdict(list)
    includedby: dict[str, list[tuple[str, int]]] = defaultdict(list)

    with (results_dir / "paths.txt").open("w", encoding="utf-8") as out:
        for (src, dst), count in edge_counts:
            out.write(f"{count:7d} {src} {dst}\n")
            includes[src].append((dst, count))
            includedby[dst].append((src, count))

    for src, entries in includes.items():
        with (includes_dir / src).open("w", encoding="utf-8") as out:
            for dst, count in entries:
                out.write(f"{dst} {count}\n")

    for dst, entries in includedby.items():
        with (includedby_dir / dst).open("w", encoding="utf-8") as out:
            for src, count in entries:
                out.write(f"{src} {count}\n")

    return includes, includedby


def build_loops_and_ordering(
    includes: dict[str, list[tuple[str, int]]],
) -> tuple[list[str], list[str]]:
    include_map = {
        src: {dst: count for dst, count in entries}
        for src, entries in includes.items()
    }

    ordering_lines: list[str] = []
    loops_lines: list[str] = []

    seen_pairs: set[tuple[str, str]] = set()

    for source in sorted(includes.keys()):
        for include, includefreq in includes[source]:
            if include not in include_map:
                continue

            sourcefreq = include_map[include].get(source)
            if sourcefreq is None:
                ordering_lines.append(f"{source} > {include}\n")
                continue

            if (include, source) in seen_pairs:
                continue
            seen_pairs.add((source, include))

            loops_lines.append(f"Loop: {source} {include}\n")
            if includefreq - sourcefreq > 3:
                loops_lines.append(f"  {source} > {include}\n\n")
            elif sourcefreq - includefreq > 3:
                loops_lines.append(f"  {include} > {source}\n\n")
            elif sourcefreq == includefreq:
                loops_lines.append(f"  {include} == {source}\n\n")
            else:
                loops_lines.append(f"  {include} ~= {source}\n\n")

    return ordering_lines, loops_lines


def generate(results_dir: Path, repo_root: Path, workers: int) -> None:
    if results_dir.exists():
        shutil.rmtree(results_dir)
    results_dir.mkdir(parents=True)

    files = iter_source_files(repo_root)

    raw_by_file: dict[str, list[str]] = {}
    paths_by_file: dict[str, list[tuple[str, str]]] = {}

    start = time.perf_counter()
    if workers <= 1:
        for file in files:
            rel = file.relative_to(repo_root).as_posix()
            raw, paths = scan_file(file, repo_root)
            raw_by_file[rel] = raw
            paths_by_file[rel] = paths
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = {
                file.relative_to(repo_root).as_posix(): pool.submit(
                    scan_file, file, repo_root
                )
                for file in files
            }
            for rel in sorted(futures.keys()):
                raw, paths = futures[rel].result()
                raw_by_file[rel] = raw
                paths_by_file[rel] = paths

    raw_lines: list[str] = []
    raw_lines.extend(
        line
        for rel in sorted(raw_by_file.keys())
        for line in raw_by_file[rel]
    )
    with (results_dir / "rawincludes.txt").open("w", encoding="utf-8") as out:
        out.write("\n".join(raw_lines))
        if raw_lines:
            out.write("\n")

    path_pairs: list[tuple[str, str]] = []
    path_pairs.extend(
        pair
        for rel in sorted(paths_by_file.keys())
        for pair in paths_by_file[rel]
    )
    counts = Counter(path_pairs)

    edge_counts = sorted(
        counts.items(),
        key=lambda item: (
            dictionary_sort_key(item[0][0]),
            dictionary_sort_key(item[0][1]),
        ),
    )

    includes, _ = write_relation_db(results_dir, edge_counts)
    ordering, loops = build_loops_and_ordering(includes)

    with (results_dir / "ordering.txt").open("w", encoding="utf-8") as out:
        out.writelines(ordering)
    with (results_dir / "loops.txt").open("w", encoding="utf-8") as out:
        out.writelines(loops)

    elapsed = time.perf_counter() - start
    print(
        f"levelization.py: scanned {len(files)} files, "
        f"{len(raw_lines)} includes, {len(edge_counts)} unique paths in "
        f"{elapsed:.2f}s"
    )
    print((results_dir / "ordering.txt").read_text(encoding="utf-8"), end="")
    print((results_dir / "loops.txt").read_text(encoding="utf-8"), end="")


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parents[1]

    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--repo-root",
        type=Path,
        default=repo_root,
        help="Repository root (defaults based on script location).",
    )
    parser.add_argument(
        "--results-dir",
        type=Path,
        default=script_dir / "results",
        help="Output results directory.",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=min(32, (os.cpu_count() or 1)),
        help="Thread count for source scanning (default: CPU count, max 32).",
    )
    args = parser.parse_args()

    generated_dir = args.results_dir.resolve()
    generate(
        results_dir=generated_dir,
        repo_root=args.repo_root.resolve(),
        workers=max(1, args.workers),
    )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
