#!/usr/bin/env python3
"""Fail if a vendor-defined <random> algorithm appears outside the allowlist.

Scans src/ and include/ for the standard algorithms whose mapping is not
portable across standard libraries. Comments are ignored. A hit is accepted
only when its path is listed in allowlist.txt. Unlisted hits are printed as
file:line and the process exits non-zero.
"""

import re
import sys
from pathlib import Path

ALGORITHMS = (
    "uniform_int_distribution",
    "uniform_real_distribution",
    "bernoulli_distribution",
    "binomial_distribution",
    "negative_binomial_distribution",
    "geometric_distribution",
    "poisson_distribution",
    "exponential_distribution",
    "gamma_distribution",
    "weibull_distribution",
    "extreme_value_distribution",
    "normal_distribution",
    "lognormal_distribution",
    "chi_squared_distribution",
    "cauchy_distribution",
    "fisher_f_distribution",
    "student_t_distribution",
    "discrete_distribution",
    "piecewise_constant_distribution",
    "piecewise_linear_distribution",
    "sample",
    "shuffle",
    "generate_canonical",
    "random_device",
)

HIT = re.compile(r"\bstd::(?:" + "|".join(ALGORITHMS) + r")\b")
SUFFIXES = {".h", ".hh", ".hpp", ".cpp", ".cc", ".cxx", ".ipp", ".inc"}


def repo_root() -> Path:
    here = Path(__file__).resolve().parent
    for candidate in (here, *here.parents):
        if (candidate / "src").is_dir() and (candidate / "include").is_dir():
            return candidate
    sys.exit("rng_tripwire: cannot find the repository root")


def load_allowlist(path: Path) -> list[tuple[str, str]]:
    entries = []
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        prefix, _, why = line.partition(" ")
        entries.append((prefix.strip(), why.strip()))
    return entries


def allowed(rel: str, entries: list[tuple[str, str]]) -> bool:
    for prefix, _why in entries:
        if rel == prefix or rel.startswith(prefix):
            return True
    return False


def strip_comments(text: str) -> list[str]:
    lines = []
    in_block = False
    for line in text.splitlines():
        out = []
        i = 0
        while i < len(line):
            if in_block:
                end = line.find("*/", i)
                if end < 0:
                    i = len(line)
                    break
                in_block = False
                i = end + 2
                continue
            if line.startswith("//", i):
                break
            if line.startswith("/*", i):
                in_block = True
                i += 2
                continue
            out.append(line[i])
            i += 1
        lines.append("".join(out))
    return lines


def main() -> int:
    root = repo_root()
    allow_path = Path(__file__).resolve().parent / "allowlist.txt"
    entries = load_allowlist(allow_path)
    hits = []
    for base in ("src", "include"):
        for path in sorted((root / base).rglob("*")):
            if not path.is_file() or path.suffix not in SUFFIXES:
                continue
            rel = path.relative_to(root).as_posix()
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeError:
                text = path.read_text(encoding="latin-1")
            for number, line in enumerate(strip_comments(text), start=1):
                if HIT.search(line):
                    hits.append((allowed(rel, entries), f"{rel}:{number}"))
    unlisted = [item for ok, item in hits if not ok]
    for ok, item in hits:
        print(("ALLOW " if ok else "HIT ") + item)
    print(f"{len(hits)} hits, {len(unlisted)} unlisted")
    for item in unlisted:
        print(item)
    return 1 if unlisted else 0


if __name__ == "__main__":
    sys.exit(main())
