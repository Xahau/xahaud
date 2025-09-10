#!/usr/bin/env python3
"""
Compile a single file using commands from compile_commands.json
"""

import json
import os
import sys
import subprocess
import argparse
from pathlib import Path


def find_compile_command(compile_commands, file_path):
    """Find the compile command for a given file path."""
    # Normalize the input path
    abs_path = os.path.abspath(file_path)
    
    for entry in compile_commands:
        # Check if this entry matches our file
        entry_file = os.path.abspath(entry['file'])
        if entry_file == abs_path:
            return entry
    
    # Try relative path matching as fallback
    for entry in compile_commands:
        if entry['file'].endswith(file_path) or file_path.endswith(entry['file']):
            return entry
    
    return None


def main():
    parser = argparse.ArgumentParser(
        description='Compile a single file using compile_commands.json'
    )
    parser.add_argument(
        'file',
        help='Path to the source file to compile'
    )
    parser.add_argument(
        '--verbose', '-v',
        action='store_true',
        help='Show the compile command being executed'
    )
    parser.add_argument(
        '--dump-output', '-d',
        action='store_true',
        help='Dump the full output from the compiler'
    )
    parser.add_argument(
        '--compile-db',
        default='build/compile_commands.json',
        help='Path to compile_commands.json (default: build/compile_commands.json)'
    )
    
    args = parser.parse_args()
    
    # Check if compile_commands.json exists
    if not os.path.exists(args.compile_db):
        print(f"Error: {args.compile_db} not found", file=sys.stderr)
        print("Make sure you've run cmake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON", file=sys.stderr)
        sys.exit(1)
    
    # Load compile commands
    try:
        with open(args.compile_db, 'r') as f:
            compile_commands = json.load(f)
    except json.JSONDecodeError as e:
        print(f"Error parsing {args.compile_db}: {e}", file=sys.stderr)
        sys.exit(1)
    
    # Find the compile command for the requested file
    entry = find_compile_command(compile_commands, args.file)
    
    if not entry:
        print(f"Error: No compile command found for {args.file}", file=sys.stderr)
        print(f"Available files in {args.compile_db}:", file=sys.stderr)
        # Show first 10 files as examples
        for i, cmd in enumerate(compile_commands[:10]):
            print(f"  {cmd['file']}", file=sys.stderr)
        if len(compile_commands) > 10:
            print(f"  ... and {len(compile_commands) - 10} more", file=sys.stderr)
        sys.exit(1)
    
    # Extract the command and directory
    command = entry['command']
    directory = entry.get('directory', '.')
    
    if args.verbose:
        print(f"Directory: {directory}", file=sys.stderr)
        print(f"Command: {command}", file=sys.stderr)
        print("-" * 80, file=sys.stderr)
    
    # Execute the compile command
    try:
        result = subprocess.run(
            command,
            shell=True,
            cwd=directory,
            capture_output=not args.dump_output,
            text=True
        )
        
        if args.dump_output:
            # Output was already printed to stdout/stderr
            pass
        else:
            # Only show output if there were errors or warnings
            if result.stderr:
                print(result.stderr, file=sys.stderr)
            if result.stdout:
                print(result.stdout)
        
        # Exit with the same code as the compiler
        sys.exit(result.returncode)
        
    except subprocess.SubprocessError as e:
        print(f"Error executing compile command: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nCompilation interrupted", file=sys.stderr)
        sys.exit(130)


if __name__ == '__main__':
    main()