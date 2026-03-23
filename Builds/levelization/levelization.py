#!/usr/bin/env python3
"""
Development-oriented levelization generator.

This script produces the same result artifact set as levelization.sh, but is
much faster by doing parsing/counting in-process instead of spawning many
external tools in tight loops.

The shell script remains the canonical CI path. Use this script for local
iteration speed, then run levelization.sh before committing if you need strict
parity with existing workflow.
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


def parse_paths(path: Path) -> dict[tuple[str, str], int]:
    out: dict[tuple[str, str], int] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        match = PATHS_LINE_PATTERN.match(line)
        if not match:
            raise ValueError(f"Cannot parse paths line: {line!r}")
        count = int(match.group(1))
        src = match.group(2)
        dst = match.group(3)
        out[(src, dst)] = count
    return out


def parse_relation_dir(path: Path) -> dict[str, Counter[str]]:
    out: dict[str, Counter[str]] = {}
    if not path.exists():
        return out
    for file in sorted(p for p in path.iterdir() if p.is_file()):
        lines = [line for line in file.read_text(encoding="utf-8").splitlines() if line]
        out[file.name] = Counter(lines)
    return out


def compare_results(generated: Path, canonical: Path) -> list[str]:
    mismatches: list[str] = []

    # rawincludes: compare as sets/multisets of lines to ignore traversal order.
    gen_raw = Counter(generated.joinpath("rawincludes.txt").read_text(encoding="utf-8").splitlines())
    can_raw = Counter(canonical.joinpath("rawincludes.txt").read_text(encoding="utf-8").splitlines())
    if gen_raw != can_raw:
        mismatches.append("rawincludes.txt differs (line multiset mismatch)")

    # paths: compare parsed edge->count map, ignoring ordering/whitespace.
    gen_paths = parse_paths(generated / "paths.txt")
    can_paths = parse_paths(canonical / "paths.txt")
    if gen_paths != can_paths:
        mismatches.append("paths.txt differs (edge count mismatch)")

    # includes / includedby: compare per-file line multisets.
    for rel in ("includes", "includedby"):
        gen_rel = parse_relation_dir(generated / rel)
        can_rel = parse_relation_dir(canonical / rel)
        if gen_rel != can_rel:
            mismatches.append(f"{rel}/ differs (file set or content mismatch)")

    # ordering and loops are canonical artifacts; require exact bytes.
    for name in ("ordering.txt", "loops.txt"):
        gen_text = generated.joinpath(name).read_text(encoding="utf-8")
        can_text = canonical.joinpath(name).read_text(encoding="utf-8")
        if gen_text != can_text:
            mismatches.append(f"{name} differs")

    return mismatches


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
            dictionary_sort_key(f"{item[0][0]} {item[0][1]}"),
            item[0][0],
            item[0][1],
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


def explain_edge(src_level: str, dst_level: str, repo_root: Path, workers: int) -> None:
    """Show every source file and #include line that creates a dependency edge."""
    files = iter_source_files(repo_root)

    matches: list[tuple[str, str]] = []  # (source_file, include_line)

    def check_file(path: Path) -> list[tuple[str, str]]:
        rel = path.relative_to(repo_root).as_posix()
        sl = source_level(rel)
        if sl != src_level:
            return []
        hits: list[tuple[str, str]] = []
        with path.open("r", encoding="utf-8", errors="ignore") as handle:
            for line in handle:
                if "boost" in line:
                    continue
                if not INCLUDE_PATTERN.match(line):
                    continue
                line = line.rstrip("\n")
                dl = include_level(line)
                if dl == dst_level:
                    hits.append((rel, line.strip()))
        return hits

    if workers <= 1:
        for file in files:
            matches.extend(check_file(file))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            for result in pool.map(check_file, files):
                matches.extend(result)

    if not matches:
        print(f"No includes found from {src_level} -> {dst_level}")
        return

    print(f"{src_level} > {dst_level}  ({len(matches)} include(s)):\n")
    for source_file, include_line in matches:
        print(f"  {source_file}")
        print(f"    {include_line}")
    print()


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
    parser.add_argument(
        "--compare-to",
        type=Path,
        default=None,
        help=(
            "Compare generated results against this directory and exit non-zero "
            "on mismatch (semantic comparison for rawincludes/paths/includes)."
        ),
    )
    parser.add_argument(
        "--explain",
        nargs=2,
        metavar=("SRC", "DST"),
        default=None,
        help=(
            "Show which source files cause a dependency edge. "
            "Example: --explain test.csf xrpld.app"
        ),
    )
    args = parser.parse_args()

    if args.explain is not None:
        explain_edge(args.explain[0], args.explain[1], args.repo_root.resolve(), max(1, args.workers))
        return 0

    generated_dir = args.results_dir.resolve()
    generate(
        results_dir=generated_dir,
        repo_root=args.repo_root.resolve(),
        workers=max(1, args.workers),
    )

    if args.compare_to is not None:
        canonical_dir = args.compare_to.resolve()
        mismatches = compare_results(generated_dir, canonical_dir)
        if mismatches:
            print("levelization.py: mismatch against canonical results:")
            for mismatch in mismatches:
                print(f"  - {mismatch}")
            return 1
        print("levelization.py: matches canonical results")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
