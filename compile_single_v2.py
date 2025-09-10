#!/usr/bin/env python3
"""
Compile a single file using commands from compile_commands.json
Enhanced version with error context display
"""

import json
import os
import sys
import subprocess
import argparse
import re
import logging
from pathlib import Path


def setup_logging(level):
    """Setup logging configuration."""
    numeric_level = getattr(logging, level.upper(), None)
    if not isinstance(numeric_level, int):
        raise ValueError(f'Invalid log level: {level}')
    
    logging.basicConfig(
        level=numeric_level,
        format='[%(levelname)s] %(message)s',
        stream=sys.stderr
    )


def find_compile_command(compile_commands, file_path):
    """Find the compile command for a given file path."""
    # Normalize the input path
    abs_path = os.path.abspath(file_path)
    logging.debug(f"Looking for compile command for: {abs_path}")
    
    for entry in compile_commands:
        # Check if this entry matches our file
        entry_file = os.path.abspath(entry['file'])
        if entry_file == abs_path:
            logging.debug(f"Found exact match: {entry_file}")
            return entry
    
    # Try relative path matching as fallback
    for entry in compile_commands:
        if entry['file'].endswith(file_path) or file_path.endswith(entry['file']):
            logging.debug(f"Found relative match: {entry['file']}")
            return entry
    
    logging.debug("No compile command found")
    return None


def extract_errors_with_context(output, file_path, context_lines=3):
    """Extract error messages with context from compiler output."""
    lines = output.split('\n')
    errors = []
    
    logging.debug(f"Parsing {len(lines)} lines of compiler output")
    logging.debug(f"Looking for errors in file: {file_path}")
    
    # Pattern to match error lines from clang/gcc
    # Matches: filename:line:col: error: message
    # Also handle color codes
    error_pattern = re.compile(r'([^:]+):(\d+):(\d+):\s*(?:\x1b\[[0-9;]*m)?\s*(error|warning):\s*(?:\x1b\[[0-9;]*m)?\s*(.*?)(?:\x1b\[[0-9;]*m)?$')
    
    for i, line in enumerate(lines):
        # Strip ANSI color codes for pattern matching
        clean_line = re.sub(r'\x1b\[[0-9;]*m', '', line)
        match = error_pattern.search(clean_line)
        
        if match:
            filename = match.group(1)
            line_num = int(match.group(2))
            col_num = int(match.group(3))
            error_type = match.group(4)
            message = match.group(5)
            
            logging.debug(f"Found {error_type} at {filename}:{line_num}:{col_num}")
            
            # Check if this error is from the file we're compiling
            # Be more flexible with path matching
            if (file_path in filename or 
                filename.endswith(os.path.basename(file_path)) or
                os.path.basename(filename) == os.path.basename(file_path)):
                
                logging.debug(f"  -> Including {error_type}: {message[:50]}...")
                
                error_info = {
                    'line': line_num,
                    'col': col_num,
                    'type': error_type,
                    'message': message,
                    'full_line': line,  # Keep original line with colors
                    'context_before': [],
                    'context_after': []
                }
                
                # Get context lines from compiler output
                for j in range(max(0, i - context_lines), i):
                    error_info['context_before'].append(lines[j])
                
                for j in range(i + 1, min(len(lines), i + context_lines + 1)):
                    error_info['context_after'].append(lines[j])
                
                errors.append(error_info)
            else:
                logging.debug(f"  -> Skipping (different file: {filename})")
    
    logging.info(f"Found {len(errors)} errors/warnings")
    return errors


def read_source_context(file_path, line_num, context_lines=3):
    """Read context from the source file around a specific line."""
    try:
        with open(file_path, 'r') as f:
            lines = f.readlines()
        
        start = max(0, line_num - context_lines - 1)
        end = min(len(lines), line_num + context_lines)
        
        context = []
        for i in range(start, end):
            line_marker = '>>> ' if i == line_num - 1 else '    '
            context.append(f"{i+1:4d}:{line_marker}{lines[i].rstrip()}")
        
        return '\n'.join(context)
    except Exception as e:
        logging.warning(f"Could not read source context: {e}")
        return None


def format_error_with_context(error, file_path, show_source_context=False):
    """Format an error with its context."""
    output = []
    output.append(f"\n{'='*80}")
    output.append(f"Error at line {error['line']}, column {error['col']}:")
    output.append(f"  {error['message']}")
    
    if show_source_context:
        source_context = read_source_context(file_path, error['line'], 3)
        if source_context:
            output.append("\nSource context:")
            output.append(source_context)
    
    if error['context_before'] or error['context_after']:
        output.append("\nCompiler output context:")
        for line in error['context_before']:
            output.append(f"  {line}")
        output.append(f">>> {error['full_line']}")
        for line in error['context_after']:
            output.append(f"  {line}")
    
    return '\n'.join(output)


def main():
    parser = argparse.ArgumentParser(
        description='Compile a single file using compile_commands.json with enhanced error display'
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
        '--show-error-context', '-e',
        type=int,
        metavar='N',
        help='Show N lines of context around each error (implies capturing output)'
    )
    parser.add_argument(
        '--show-source-context', '-s',
        action='store_true',
        help='Show source file context around errors'
    )
    parser.add_argument(
        '--errors-only',
        action='store_true',
        help='Only show errors, not warnings'
    )
    parser.add_argument(
        '--compile-db',
        default='build/compile_commands.json',
        help='Path to compile_commands.json (default: build/compile_commands.json)'
    )
    parser.add_argument(
        '--log-level', '-l',
        default='WARNING',
        choices=['DEBUG', 'INFO', 'WARNING', 'ERROR'],
        help='Set logging level (default: WARNING)'
    )
    
    args = parser.parse_args()
    
    # Setup logging
    setup_logging(args.log_level)
    
    # Check if compile_commands.json exists
    if not os.path.exists(args.compile_db):
        print(f"Error: {args.compile_db} not found", file=sys.stderr)
        print("Make sure you've run cmake with -DCMAKE_EXPORT_COMPILE_COMMANDS=ON", file=sys.stderr)
        sys.exit(1)
    
    # Load compile commands
    try:
        with open(args.compile_db, 'r') as f:
            compile_commands = json.load(f)
        logging.info(f"Loaded {len(compile_commands)} compile commands")
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
    source_file = entry['file']
    
    if args.verbose:
        print(f"Directory: {directory}", file=sys.stderr)
        print(f"Command: {command}", file=sys.stderr)
        print("-" * 80, file=sys.stderr)
    
    logging.info(f"Compiling {source_file}")
    logging.debug(f"Working directory: {directory}")
    logging.debug(f"Command: {command}")
    
    # Execute the compile command
    try:
        # If we need to show error context, we must capture output
        capture = not args.dump_output or args.show_error_context is not None
        
        logging.debug(f"Running compiler (capture={capture})")
        result = subprocess.run(
            command,
            shell=True,
            cwd=directory,
            capture_output=capture,
            text=True
        )
        
        logging.info(f"Compiler returned code: {result.returncode}")
        
        if args.dump_output and not args.show_error_context:
            # Output was already printed to stdout/stderr
            pass
        elif args.show_error_context is not None:
            # Parse and display errors with context
            all_output = result.stderr + "\n" + result.stdout
            
            # Log first few lines of output for debugging
            output_lines = all_output.split('\n')[:10]
            for line in output_lines:
                logging.debug(f"Output: {line}")
            
            errors = extract_errors_with_context(all_output, args.file, args.show_error_context)
            
            if args.errors_only:
                errors = [e for e in errors if e['type'] == 'error']
                logging.info(f"Filtered to {len(errors)} errors only")
            
            print(f"\nFound {len(errors)} {'error' if args.errors_only else 'error/warning'}(s) in {args.file}:\n")
            
            for error in errors:
                print(format_error_with_context(error, source_file, args.show_source_context))
            
            if errors:
                print(f"\n{'='*80}")
                print(f"Total: {len(errors)} {'error' if args.errors_only else 'error/warning'}(s)")
        else:
            # Default behavior - show output if there were errors or warnings
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