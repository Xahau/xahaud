#!/usr/bin/env python3
"""
Generate SetHook_wasm.h from SetHook_test.cpp

Extracts WASM test code blocks from the test file, compiles them using wasmcc or wat2wasm,
and generates a C++ header file with the compiled bytecode.

Features intelligent caching based on source content and binary versions.
"""

import argparse
import hashlib
import logging
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Dict, List, Optional, Tuple


class BinaryChecker:
    """Check for required binaries and provide installation instructions."""

    REQUIRED_BINARIES = {
        'wasmcc': 'curl https://raw.githubusercontent.com/wasienv/wasienv/master/install.sh | sh',
        'hook-cleaner': 'git clone https://github.com/RichardAH/hook-cleaner-c.git && cd hook-cleaner-c && make && sudo cp hook-cleaner /usr/local/bin/',
        'wat2wasm': 'brew install wabt',
        'clang-format': 'brew install clang-format',
    }

    # Note: Python implementation doesn't need GNU sed/grep, xxd, or bc
    # Regex and byte formatting are done natively in Python

    def __init__(self, logger: logging.Logger):
        self.logger = logger

    def check_binary(self, name: str) -> Optional[str]:
        """Check if binary exists and return its path."""
        result = subprocess.run(['which', name], capture_output=True, text=True)
        if result.returncode == 0:
            path = result.stdout.strip()
            self.logger.info(f"✓ {name}: {path}")
            return path
        return None

    def check_all(self) -> bool:
        """Check all required binaries. Returns True if all found."""
        self.logger.info("Checking required tools...")
        all_found = True

        for binary, install_msg in self.REQUIRED_BINARIES.items():
            path = self.check_binary(binary)
            if not path:
                self.logger.error(f"✗ {binary}: NOT FOUND")
                self.logger.error(f"  Install: {install_msg}")
                all_found = False

        if all_found:
            self.logger.info("All required tools found!")

        return all_found


class CompilationCache:
    """Cache compiled WASM bytecode based on source and binary versions."""

    def __init__(self, logger: logging.Logger):
        self.logger = logger
        self.cache_dir = Path.home() / '.cache' / 'build_test_hooks'
        self.cache_dir.mkdir(parents=True, exist_ok=True)
        self.binary_versions = self._get_binary_versions()
        self.logger.debug(f"Cache directory: {self.cache_dir}")

    def _get_binary_version(self, binary: str) -> str:
        """Get version hash of a binary."""
        try:
            which_result = subprocess.run(['which', binary], capture_output=True, text=True, check=True)
            binary_path = which_result.stdout.strip()

            # Hash the binary file itself
            hasher = hashlib.sha256()
            with open(binary_path, 'rb') as f:
                hasher.update(f.read())
            return hasher.hexdigest()[:16]
        except Exception as e:
            self.logger.warning(f"Could not hash {binary}: {e}")
            return "unknown"

    def _get_binary_versions(self) -> Dict[str, str]:
        """Get version hashes of all compilation binaries."""
        binaries = ['wasmcc', 'hook-cleaner', 'wat2wasm']
        versions = {}

        for binary in binaries:
            versions[binary] = self._get_binary_version(binary)
            self.logger.debug(f"{binary} version hash: {versions[binary]}")

        return versions

    def _compute_cache_key(self, source: str, is_wat: bool) -> str:
        """Compute cache key from source and binary versions."""
        hasher = hashlib.sha256()
        hasher.update(source.encode('utf-8'))
        hasher.update(b'wat' if is_wat else b'c')

        # Include relevant binary versions
        if is_wat:
            hasher.update(self.binary_versions['wat2wasm'].encode('utf-8'))
        else:
            hasher.update(self.binary_versions['wasmcc'].encode('utf-8'))
            hasher.update(self.binary_versions['hook-cleaner'].encode('utf-8'))

        return hasher.hexdigest()

    def get(self, source: str, is_wat: bool) -> Optional[bytes]:
        """Get cached bytecode if available."""
        cache_key = self._compute_cache_key(source, is_wat)
        cache_file = self.cache_dir / f"{cache_key}.wasm"

        if cache_file.exists():
            self.logger.debug(f"Cache hit: {cache_key[:16]}...")
            return cache_file.read_bytes()

        self.logger.debug(f"Cache miss: {cache_key[:16]}...")
        return None

    def put(self, source: str, is_wat: bool, bytecode: bytes) -> None:
        """Store bytecode in cache."""
        cache_key = self._compute_cache_key(source, is_wat)
        cache_file = self.cache_dir / f"{cache_key}.wasm"

        cache_file.write_bytes(bytecode)
        self.logger.debug(f"Cached: {cache_key[:16]}... ({len(bytecode)} bytes)")


class SourceValidator:
    """Validate C source code for undeclared functions."""

    def __init__(self, logger: logging.Logger):
        self.logger = logger

    def extract_declarations(self, source: str) -> Tuple[List[str], List[str]]:
        """Extract declared and used function names."""
        # Normalize source: collapse whitespace/newlines to handle multi-line declarations
        normalized = re.sub(r'\s+', ' ', source)

        declared = set()
        used = set()

        # Find all extern/define declarations (handles multi-line)
        # Matches: extern TYPE function_name ( ...
        decl_pattern = r'(?:extern|define)\s+[a-z0-9_]+\s+([a-z_-]+)\s*\('
        for match in re.finditer(decl_pattern, normalized):
            func_name = match.group(1)
            if func_name != 'sizeof':
                declared.add(func_name)

        # Find all function calls
        # Matches: function_name(
        call_pattern = r'([a-z_-]+)\('
        for match in re.finditer(call_pattern, normalized):
            func_name = match.group(1)
            if func_name != 'sizeof' and not func_name.startswith(('hook', 'cbak')):
                used.add(func_name)

        return sorted(declared), sorted(used)

    def validate(self, source: str, counter: int) -> None:
        """Validate that all used functions are declared."""
        declared, used = self.extract_declarations(source)
        undeclared = set(used) - set(declared)

        if undeclared:
            self.logger.error(f"Undeclared functions in block {counter}: {', '.join(sorted(undeclared))}")
            self.logger.debug(f"  Declared: {', '.join(declared)}")
            self.logger.debug(f"  Used: {', '.join(used)}")
            raise ValueError(f"Undeclared functions: {', '.join(sorted(undeclared))}")


class WasmCompiler:
    """Compile WASM from C or WAT source."""

    def __init__(self, logger: logging.Logger, wasm_dir: Path, cache: CompilationCache):
        self.logger = logger
        self.wasm_dir = wasm_dir
        self.cache = cache
        self.validator = SourceValidator(logger)

    def is_wat_format(self, source: str) -> bool:
        """Check if source is WAT format."""
        return '(module' in source

    def compile_c(self, source: str, counter: int) -> bytes:
        """Compile C source to WASM."""
        self.logger.debug(f"Compiling C for block {counter}")
        self.validator.validate(source, counter)

        # Save source for debugging
        source_file = self.wasm_dir / f"test-{counter}-gen.c"
        source_file.write_text(f'#include "api.h"\n{source}')

        # Compile with wasmcc (binary I/O)
        wasmcc_result = subprocess.run(
            ['wasmcc', '-x', 'c', '/dev/stdin', '-o', '/dev/stdout', '-O2', '-Wl,--allow-undefined'],
            input=source.encode('utf-8'),
            capture_output=True,
            check=True
        )

        # Clean with hook-cleaner (binary I/O)
        cleaner_result = subprocess.run(
            ['hook-cleaner', '-', '-'],
            input=wasmcc_result.stdout,
            capture_output=True,
            check=True
        )

        return cleaner_result.stdout

    def compile_wat(self, source: str) -> bytes:
        """Compile WAT source to WASM."""
        self.logger.debug("Compiling WAT")
        source = re.sub(r'/\*end\*/$', '', source)

        result = subprocess.run(
            ['wat2wasm', '-', '-o', '/dev/stdout'],
            input=source.encode('utf-8'),
            capture_output=True,
            check=True
        )

        return result.stdout

    def compile(self, source: str, counter: int) -> bytes:
        """Compile source, using cache if available."""
        is_wat = self.is_wat_format(source)

        # Check cache first
        cached = self.cache.get(source, is_wat)
        if cached is not None:
            self.logger.info(f"Block {counter}: using cached bytecode")
            return cached

        # Compile
        self.logger.info(f"Block {counter}: compiling {'WAT' if is_wat else 'C'}")

        try:
            if is_wat:
                bytecode = self.compile_wat(source)
            else:
                bytecode = self.compile_c(source, counter)

            # Cache result
            self.cache.put(source, is_wat, bytecode)
            return bytecode

        except subprocess.CalledProcessError as e:
            # Try to decode stderr if it exists
            error_msg = str(e)
            if e.stderr:
                try:
                    error_msg = e.stderr.decode('utf-8')
                except:
                    error_msg = f"Binary error output ({len(e.stderr)} bytes)"
            self.logger.error(f"Compilation failed: {error_msg}")
            raise


class OutputFormatter:
    """Format compiled bytecode as C++ arrays."""

    @staticmethod
    def bytes_to_cpp_array(data: bytes) -> str:
        """Convert binary data to C++ array format."""
        lines = []
        for i in range(0, len(data), 10):
            chunk = data[i:i+10]
            hex_values = ','.join(f'0x{b:02X}U' for b in chunk)
            lines.append(f'    {hex_values},')
        return '\n'.join(lines)


class SourceExtractor:
    """Extract WASM test blocks from source file."""

    def __init__(self, logger: logging.Logger, input_file: Path):
        self.logger = logger
        self.input_file = input_file

    def extract(self) -> List[Tuple[str, int]]:
        """Extract all WASM test blocks with their line numbers. Returns [(source, line_number), ...]"""
        self.logger.info(f"Reading {self.input_file}")
        content = self.input_file.read_text()

        pattern = r'R"\[test\.hook\]\((.*?)\)\[test\.hook\]"'
        blocks_with_lines = []

        for match in re.finditer(pattern, content, re.DOTALL):
            source = match.group(1)
            # Count newlines before this match to get line number
            line_number = content[:match.start()].count('\n') + 1
            blocks_with_lines.append((source, line_number))

        self.logger.info(f"Found {len(blocks_with_lines)} WASM test blocks")
        return blocks_with_lines


class OutputWriter:
    """Write compiled blocks to output file."""

    HEADER = """
//This file is generated by build_test_hooks.py
#ifndef SETHOOK_WASM_INCLUDED
#define SETHOOK_WASM_INCLUDED
#include <map>
#include <stdint.h>
#include <string>
#include <vector>
namespace ripple {
namespace test {
std::map<std::string, std::vector<uint8_t>> wasm = {
"""

    FOOTER = """};
}
}
#endif
"""

    def __init__(self, logger: logging.Logger, output_file: Path):
        self.logger = logger
        self.output_file = output_file

    def write_block(self, out, counter: int, source: str, bytecode: bytes) -> None:
        """Write a single compiled block."""
        out.write(f'/* ==== WASM: {counter} ==== */\n')
        out.write('{ R"[test.hook](')
        out.write(source)
        out.write(')[test.hook]",\n{\n')
        out.write(OutputFormatter.bytes_to_cpp_array(bytecode))
        out.write('\n}},\n\n')

    def write(self, compiled_blocks: Dict[int, Tuple[str, bytes]]) -> None:
        """Write all compiled blocks to output file."""
        self.logger.info(f"Writing {self.output_file}")

        with open(self.output_file, 'w') as out:
            out.write(self.HEADER)

            for counter in sorted(compiled_blocks.keys()):
                source, bytecode = compiled_blocks[counter]
                self.write_block(out, counter, source, bytecode)

            out.write(self.FOOTER)

    def format_with_clang(self) -> None:
        """Format output file with clang-format."""
        self.logger.info("Formatting with clang-format")
        subprocess.run(['clang-format', '-i', str(self.output_file)], check=True)


class TestHookBuilder:
    """Main builder orchestrating the compilation process."""

    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.logger = self._setup_logging()
        self.script_dir = Path(__file__).parent
        self.wasm_dir = self.script_dir / 'generated' / 'hook' / 'c'
        self.input_file = self.script_dir / 'SetHook_test.cpp'
        self.output_file = self.script_dir / 'SetHook_wasm.h'

        self.checker = BinaryChecker(self.logger)
        self.cache = CompilationCache(self.logger)
        self.compiler = WasmCompiler(self.logger, self.wasm_dir, self.cache)
        self.extractor = SourceExtractor(self.logger, self.input_file)
        self.writer = OutputWriter(self.logger, self.output_file)

    def _setup_logging(self) -> logging.Logger:
        """Setup logging with specified level."""
        level = getattr(logging, self.args.log_level.upper())
        logging.basicConfig(level=level, format='%(levelname)s: %(message)s')
        return logging.getLogger(__name__)

    def _get_worker_count(self) -> int:
        """Get number of parallel workers to use."""
        if self.args.jobs > 0:
            return self.args.jobs
        return os.cpu_count() or 1

    def compile_block(self, counter: int, source: str, line_number: int) -> Tuple[int, str, bytes]:
        """Compile a single block."""
        bytecode = self.compiler.compile(source, counter)
        return (counter, source, bytecode)

    def _format_block_ranges(self, block_numbers: List[int]) -> str:
        """Format block numbers as compact ranges (e.g., '1-3,5,7-9')."""
        if not block_numbers:
            return ""

        sorted_blocks = sorted(block_numbers)
        ranges = []
        start = sorted_blocks[0]
        end = sorted_blocks[0]

        for num in sorted_blocks[1:]:
            if num == end + 1:
                end = num
            else:
                if start == end:
                    ranges.append(str(start))
                else:
                    ranges.append(f"{start}-{end}")
                start = end = num

        # Add final range
        if start == end:
            ranges.append(str(start))
        else:
            ranges.append(f"{start}-{end}")

        return ','.join(ranges)

    def compile_all_blocks(self, blocks: List[Tuple[str, int]]) -> Dict[int, Tuple[str, bytes]]:
        """Compile all blocks in parallel."""
        workers = self._get_worker_count()
        self.logger.info(f"Compiling {len(blocks)} blocks using {workers} workers")

        compiled = {}
        failed_blocks = []

        with ThreadPoolExecutor(max_workers=workers) as executor:
            futures = {
                executor.submit(self.compile_block, i, block, line_num): (i, line_num)
                for i, (block, line_num) in enumerate(blocks)
            }

            for future in as_completed(futures):
                counter, line_num = futures[future]
                try:
                    result_counter, source, bytecode = future.result()
                    compiled[result_counter] = (source, bytecode)
                except Exception as e:
                    self.logger.error(f"Block {counter} (line {line_num} in {self.input_file.name}) failed: {e}")
                    failed_blocks.append(counter)

        if failed_blocks:
            block_range = self._format_block_ranges(failed_blocks)
            total = len(failed_blocks)
            plural = "s" if total > 1 else ""
            raise RuntimeError(f"Block{plural} {block_range} failed ({total} total)")

        return compiled

    def build(self) -> None:
        """Execute the full build process."""
        self.logger.info("Starting WASM test hook build")

        # Display configuration
        workers = self._get_worker_count()
        self.logger.info("Configuration:")
        self.logger.info(f"  Workers: {workers} (CPU count: {os.cpu_count()})")
        self.logger.info(f"  Log level: {self.args.log_level.upper()}")
        self.logger.info(f"  Input: {self.input_file}")
        self.logger.info(f"  Output: {self.output_file}")
        self.logger.info(f"  Cache: {self.cache.cache_dir}")
        self.logger.info(f"  WASM dir: {self.wasm_dir}")
        self.logger.info("")

        if not self.checker.check_all():
            self.logger.error("Missing required binaries")
            sys.exit(1)

        self.wasm_dir.mkdir(parents=True, exist_ok=True)

        blocks = self.extractor.extract()
        compiled = self.compile_all_blocks(blocks)
        self.writer.write(compiled)
        self.writer.format_with_clang()

        self.logger.info(f"Successfully generated {self.output_file}")


def create_parser() -> argparse.ArgumentParser:
    """Create argument parser."""
    parser = argparse.ArgumentParser(
        description='Generate SetHook_wasm.h from SetHook_test.cpp',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s                          # Build with INFO logging
  %(prog)s --log-level=debug        # Build with DEBUG logging
  %(prog)s -j 4                     # Build with 4 workers
  %(prog)s -j 1                     # Build sequentially
"""
    )

    parser.add_argument(
        '--log-level',
        default='info',
        choices=['debug', 'info', 'warning', 'error'],
        help='Set logging level (default: info)'
    )

    parser.add_argument(
        '-j', '--jobs',
        type=int,
        default=0,
        metavar='N',
        help='Parallel workers (default: CPU count)'
    )

    return parser


def main():
    parser = create_parser()
    args = parser.parse_args()

    try:
        builder = TestHookBuilder(args)
        builder.build()
    except RuntimeError as e:
        # RuntimeError has our nicely formatted message
        logging.error(f"Build failed: {e}")
        sys.exit(1)
    except Exception as e:
        logging.error(f"Build failed: {e}")
        sys.exit(1)


if __name__ == '__main__':
    main()
