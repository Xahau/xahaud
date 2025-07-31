#!/usr/bin/env python3

import argparse
import hashlib
import logging
import os
import re
import subprocess
import sys
from datetime import datetime
from pathlib import Path


def get_qjsc_hash(qjsc_path):
    """Generate a hash of the qjsc binary to include in the cache key."""
    try:
        with open(qjsc_path, "rb") as f:
            return hashlib.sha256(f.read()).hexdigest()[
                :16
            ]  # Use first 16 chars of hash for brevity
    except Exception as e:
        logging.warning(f"Could not hash qjsc binary: {e}")
        return "unknown"


def find_qjsc_binary():
    """Find qjsc binary in expected locations."""
    # Check environment variable first
    if "QJSC_BINARY" in os.environ:
        qjsc_path = os.environ["QJSC_BINARY"]
        if os.path.exists(qjsc_path) and os.access(qjsc_path, os.X_OK):
            return qjsc_path
        else:
            logging.error(f"QJSC_BINARY points to invalid path: {qjsc_path}")
            sys.exit(1)

    # Check current directory
    qjsc_path = "./qjsc"
    if os.path.exists(qjsc_path) and os.access(qjsc_path, os.X_OK):
        return qjsc_path

    # Not found
    logging.error(
        "qjsc not found in current directory.\n"
        "This requires the custom quickjslite build from:\n"
        "https://github.com/RichardAH/quickjslite\n"
        "Build it and place in src/test/app/ or set QJSC_BINARY environment variable"
    )
    sys.exit(1)


def convert_js_to_carray(js_file, js_content, qjsc_path):
    """
    Convert a JavaScript file to a C array using qjsc.
    Extracts just the hex bytes from the qjsc output.
    """
    try:
        # Run qjsc to compile the JavaScript file to C code
        result = subprocess.run(
            [qjsc_path, "-c", "-o", "/dev/stdout", js_file],
            capture_output=True,
            check=True,
        )

        # Check if we have any output
        if not result.stdout:
            logging.error(f"Error: qjsc produced no output for {js_file}")
            sys.exit(1)

        # Convert to text and extract just the hex values
        output_text = result.stdout.decode("utf-8", errors="replace")

        # Extract hexadecimal values from the array definition
        # Looking for patterns like 0x43, 0x0c, etc.
        hex_values = re.findall(r"0x[0-9A-Fa-f]{2}", output_text)

        # Format them as 0xXXU for the C array
        c_array = ", ".join([f"{hex_val}U" for hex_val in hex_values])

        return c_array

    except subprocess.CalledProcessError as e:
        logging.error(f"Error executing qjsc: {e}, content: {js_content}")
        if e.stderr:
            logging.error(f"stderr: {e.stderr.decode('utf-8', errors='replace')}")
        sys.exit(1)


def extract_hook_description(js_content, start_line=None):
    """Extract description from // @hook comment at start of JS content."""
    lines = js_content.split("\n")
    for line in lines:
        line = line.strip()
        if line.startswith("// @hook "):
            return line[9:]  # Remove '// @hook ' prefix
        elif line and not line.startswith("//"):
            break  # Stop at first non-comment line
    
    # If no @hook comment found, return line number info
    if start_line is not None:
        return f"MISSING @hook (line {start_line})"
    return None


def generate_content_hash(content):
    """Generate 8-character hash for content."""
    return hashlib.sha256(content.encode("utf-8")).hexdigest()[:8]


def main():
    parser = argparse.ArgumentParser(
        description="Generate JS hook headers with individual .inc files"
    )
    parser.add_argument(
        "--source",
        default="SetJSHook_test.cpp",
        help="Source file containing JS hooks",
    )
    parser.add_argument(
        "--master",
        default="SetJSHook_wasm.h",
        help="Master header file to generate",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Directory for generated .inc files",
    )
    parser.add_argument(
        "--cache-dir",
        help="Directory for compilation cache (optional)",
    )
    parser.add_argument(
        "--xahaud-root",
        help="Xahaud root directory (if not specified, will try to auto-detect)",
    )
    parser.add_argument(
        "--log-level",
        default="error",
        choices=["debug", "info", "warning", "error", "critical"],
        help="Set logging level (default: error)",
    )

    args = parser.parse_args()

    # Configure logging
    numeric_level = getattr(logging, args.log_level.upper(), None)
    logging.basicConfig(
        level=numeric_level, format="[jshooks] %(levelname)s: %(message)s"
    )

    # Set working directory if xahaud_root is provided
    if args.xahaud_root:
        working_dir = os.path.join(args.xahaud_root, "src/test/app")
        if os.path.exists(working_dir):
            os.chdir(working_dir)
            logging.info(f"Changed working directory to: {working_dir}")
        else:
            logging.error(f"Working directory does not exist: {working_dir}")
            sys.exit(1)

    # Find qjsc binary
    qjsc_path = find_qjsc_binary()
    logging.info(f"Using qjsc: {qjsc_path}")

    # Create output directories
    Path(args.output_dir).mkdir(parents=True, exist_ok=True)
    if args.cache_dir:
        Path(args.cache_dir).mkdir(parents=True, exist_ok=True)

    # Get hash of qjsc binary for cache key
    qjsc_hash = get_qjsc_hash(qjsc_path)

    # Make paths relative for logging
    relative_source = f"src/test/app/{os.path.basename(args.source)}"
    relative_master = f"src/test/app/{os.path.basename(args.master)}"
    logging.info(f"Processing {relative_source} to generate {relative_master}...")

    # Check if input file exists
    if not os.path.exists(args.source):
        logging.error(f"Error: Input file '{args.source}' not found.")
        sys.exit(1)

    # Read input file content
    with open(args.source, "r", encoding="utf-8") as f:
        content = f.read()

    content_with_form_feeds = content.replace("\n", "\f")

    # Get hooks using regex that matches the original bash script's grep command
    pattern = r'R"\[test\.hook\](.*?)\[test\.hook\]"'
    hook_matches = list(re.finditer(pattern, content_with_form_feeds, re.DOTALL))

    if not hook_matches:
        logging.warning("Warning: No test hooks found in the input file.")
        # Create empty master header
        with open(args.master, "w") as f:
            f.write(
                f"""// Generated by build_test_jshooks.py - DO COMMIT THIS FILE
// Last updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
// 
// This file provides a registry of all JS test hooks.
// Individual compiled hooks are in build/jshooks/generated/

#ifndef SETHOOK_JSWASM_INCLUDED
#define SETHOOK_JSWASM_INCLUDED

#include <map>
#include <stdint.h>
#include <string>
#include <vector>

namespace ripple {{
namespace test {{

// Map of all JS test hooks - populated at compile time
std::map<std::string, std::vector<uint8_t>> jswasm = {{
    // No hooks found
}};

}} // namespace test
}} // namespace ripple

#endif
"""
            )
        sys.exit(0)

    # Process the matches and calculate line numbers
    processed_matches = []
    hook_line_ranges = []

    for match in hook_matches:
        # Calculate start and end line numbers
        start_line = content_with_form_feeds[: match.start()].count("\f") + 1
        end_line = content_with_form_feeds[: match.end()].count("\f") + 1
        hook_line_ranges.append((start_line, end_line))

        # Extract the content and process it
        raw_content = match.group(1)
        # Remove the opening tag
        processed = re.sub(r"^\(", "", raw_content)
        # Remove the closing tag and any trailing whitespace/form feeds
        processed = re.sub(r"\)[\f \t]*$", "/*end*/", processed)
        processed_matches.append(processed)

    # Generate individual .inc files and collect include statements
    include_statements = []

    counter = 0
    for hook_content in processed_matches:
        logging.debug(f"Processing hook {counter}...")

        # Clean content and convert back to newlines
        clean_content = (
            hook_content[:-7] if hook_content.endswith("/*end*/") else hook_content
        )
        clean_content = clean_content.replace("\f", "\n")

        # Check if this is a WebAssembly module
        wat_count = len(re.findall(r"\(module", hook_content))
        if wat_count > 0:
            logging.error(f"Error: WebAssembly text format detected in hook {counter}")
            sys.exit(1)

        # Generate hash for this hook
        content_hash = generate_content_hash(clean_content)
        inc_filename = f"test.hook.{content_hash}.inc"
        inc_path = os.path.join(args.output_dir, inc_filename)

        # Extract description
        start_line, end_line = hook_line_ranges[counter]
        description = extract_hook_description(clean_content, start_line)
        comment = f"  // {description}" if description else ""

        # Add to include statements
        include_statements.append(f'    #include "generated/{inc_filename}"{comment}')

        # Check cache
        cache_file = None
        if args.cache_dir:
            cache_content_hash = hashlib.sha256(
                f"{qjsc_hash}:{clean_content}".encode("utf-8")
            ).hexdigest()
            cache_file = os.path.join(
                args.cache_dir, f"hook-{cache_content_hash}.c_array"
            )

        # Get or generate C array
        if args.cache_dir and cache_file and os.path.exists(cache_file):
            logging.debug(f"Using cached version for hook {counter}")
            with open(cache_file, "r") as cache:
                c_array = cache.read()
        else:
            logging.debug(f"Compiling hook {counter}...")
            # Generate JS file
            js_content = clean_content + "\n"
            js_file = os.path.join(args.output_dir, f"temp-{counter}-gen.js")
            with open(js_file, "w", encoding="utf-8") as f:
                f.write(js_content)

            try:
                c_array = convert_js_to_carray(js_file, js_content, qjsc_path)

                # Cache the result
                if args.cache_dir and cache_file:
                    with open(cache_file, "w") as cache:
                        cache.write(c_array)

                # Clean up temp file
                os.remove(js_file)
            except Exception as e:
                logging.error(f"Compilation error for hook {counter}: {e}")
                sys.exit(1)

        # Generate .inc file with actual line range
        start_line, end_line = hook_line_ranges[counter]
        # Make source path relative to repo root
        relative_source = f"src/test/app/{os.path.basename(args.source)}"
        with open(inc_path, "w") as f:
            f.write(
                f"""// Auto-generated - DO NOT EDIT
// Source: {relative_source}:{start_line} ({start_line}:{end_line})
// Hash: {content_hash}

{{R"[test.hook]({clean_content})[test.hook]", {{
{c_array}
}}}},
"""
            )

        counter += 1

    # Generate master header
    with open(args.master, "w") as f:
        f.write(
            f"""// Generated by build_test_jshooks.py - DO COMMIT THIS FILE
// Last updated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}
// 
// This file provides a registry of all JS test hooks.
// Individual compiled hooks are in build/jshooks/generated/

#ifndef SETHOOK_JSWASM_INCLUDED
#define SETHOOK_JSWASM_INCLUDED

#include <map>
#include <stdint.h>
#include <string>
#include <vector>

namespace ripple {{
namespace test {{

// Map of all JS test hooks - populated at compile time
std::map<std::string, std::vector<uint8_t>> jswasm = {{
{chr(10).join(include_statements)}
}};

}} // namespace test
}} // namespace ripple

#endif
"""
        )

    # Format the output file using clang-format
    logging.info("Formatting master header...")
    try:
        subprocess.run(["clang-format", "-i", args.master], check=True)
        logging.info(f"Successfully generated {relative_master} with {counter} hooks")
    except subprocess.CalledProcessError:
        logging.warning(
            "Warning: clang-format failed, output might not be properly formatted"
        )
    except FileNotFoundError:
        logging.warning("Warning: clang-format not found, output will not be formatted")


if __name__ == "__main__":
    main()
