#!/usr/bin/env python3

import re
import os
from pathlib import Path
from collections import defaultdict
from typing import Set, Dict, List, Tuple

def find_keylet_calls(root_dir: str) -> Dict[str, List[Tuple[str, int, str]]]:
    """
    Find all keylet:: function calls with hash_options as first parameter.
    Returns a dict mapping keylet function names to list of (file, line, full_match) tuples.
    """
    # Pattern to match keylet::<function>(hash_options{...}, ...) calls
    # This captures:
    # 1. The keylet function name
    # 2. The entire first argument (hash_options{...})
    # 3. The content inside hash_options{...}
    pattern = re.compile(
        r'keylet::(\w+)\s*\(\s*(hash_options\s*\{([^}]*)\})',
        re.MULTILINE | re.DOTALL
    )
    
    results = defaultdict(list)
    unique_first_args = set()
    
    # Walk through all C++ source files
    for root, dirs, files in os.walk(Path(root_dir) / "src" / "ripple"):
        # Skip certain directories
        dirs[:] = [d for d in dirs if d not in ['.git', 'build', '__pycache__']]
        
        for file in files:
            if file.endswith(('.cpp', '.h', '.hpp')):
                filepath = os.path.join(root, file)
                try:
                    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
                        content = f.read()
                        
                    # Find all matches in this file
                    for match in pattern.finditer(content):
                        func_name = match.group(1)
                        full_first_arg = match.group(2)
                        inner_content = match.group(3).strip()
                        
                        # Get line number
                        line_num = content[:match.start()].count('\n') + 1
                        
                        # Store the result
                        rel_path = os.path.relpath(filepath, root_dir)
                        results[func_name].append((rel_path, line_num, full_first_arg))
                        unique_first_args.add(inner_content)
                        
                except Exception as e:
                    print(f"Error reading {filepath}: {e}")
    
    return results, unique_first_args

def analyze_hash_options_content(unique_args: Set[str]) -> Dict[str, int]:
    """Analyze the content of hash_options{...} arguments."""
    categories = {
        'literal_0': 0,
        'literal_number': 0,
        'view_seq': 0,
        'ledger_seq': 0,
        'info_seq': 0,
        'ctx_view_seq': 0,
        'sb_seq': 0,
        'env_current_seq': 0,
        'other': 0
    }
    
    other_patterns = []
    
    for arg in unique_args:
        arg_clean = arg.strip()
        
        if arg_clean == '0':
            categories['literal_0'] += 1
        elif arg_clean.isdigit():
            categories['literal_number'] += 1
        elif 'view.seq()' in arg_clean or 'view().seq()' in arg_clean:
            categories['view_seq'] += 1
        elif 'ledger->seq()' in arg_clean or 'ledger.seq()' in arg_clean:
            categories['ledger_seq'] += 1
        elif 'info.seq' in arg_clean or 'info_.seq' in arg_clean:
            categories['info_seq'] += 1
        elif 'ctx.view.seq()' in arg_clean:
            categories['ctx_view_seq'] += 1
        elif 'sb.seq()' in arg_clean:
            categories['sb_seq'] += 1
        elif 'env.current()->seq()' in arg_clean:
            categories['env_current_seq'] += 1
        else:
            categories['other'] += 1
            other_patterns.append(arg_clean)
    
    return categories, other_patterns

def print_report(results: Dict[str, List], unique_args: Set[str]):
    """Print a detailed report of findings."""
    print("=" * 80)
    print("KEYLET FUNCTION CALL ANALYSIS")
    print("=" * 80)
    
    # Summary
    total_calls = sum(len(calls) for calls in results.values())
    print(f"\nTotal keylet calls found: {total_calls}")
    print(f"Unique keylet functions: {len(results)}")
    print(f"Unique hash_options arguments: {len(unique_args)}")
    
    # Function frequency
    print("\n" + "=" * 80)
    print("KEYLET FUNCTIONS BY FREQUENCY:")
    print("=" * 80)
    
    sorted_funcs = sorted(results.items(), key=lambda x: len(x[1]), reverse=True)
    for func_name, calls in sorted_funcs[:20]:  # Top 20
        print(f"  {func_name:30} {len(calls):4} calls")
    
    if len(sorted_funcs) > 20:
        print(f"  ... and {len(sorted_funcs) - 20} more functions")
    
    # Analyze hash_options content
    print("\n" + "=" * 80)
    print("HASH_OPTIONS ARGUMENT PATTERNS:")
    print("=" * 80)
    
    categories, other_patterns = analyze_hash_options_content(unique_args)
    
    for category, count in sorted(categories.items(), key=lambda x: x[1], reverse=True):
        if count > 0:
            print(f"  {category:25} {count:4} occurrences")
    
    if other_patterns:
        print("\n" + "=" * 80)
        print("OTHER PATTERNS (need review):")
        print("=" * 80)
        for i, pattern in enumerate(sorted(set(other_patterns))[:10], 1):
            # Truncate long patterns
            display = pattern if len(pattern) <= 60 else pattern[:57] + "..."
            print(f"  {i:2}. {display}")
    
    # Sample calls for most common functions
    print("\n" + "=" * 80)
    print("SAMPLE CALLS (top 5 functions):")
    print("=" * 80)
    
    for func_name, calls in sorted_funcs[:5]:
        print(f"\n{func_name}:")
        for filepath, line_num, arg in calls[:3]:  # Show first 3 examples
            print(f"  {filepath}:{line_num}")
            print(f"    {arg}")
        if len(calls) > 3:
            print(f"  ... and {len(calls) - 3} more")

def generate_replacement_script(results: Dict[str, List], unique_args: Set[str]):
    """Generate a script to help with replacements."""
    print("\n" + "=" * 80)
    print("SUGGESTED MIGRATION STRATEGY:")
    print("=" * 80)
    
    print("""
The goal is to migrate from:
  keylet::func(hash_options{ledger_seq})
  
To either:
  keylet::func(hash_options{ledger_seq, KEYLET_CLASSIFIER})
  
Where KEYLET_CLASSIFIER would be a specific HashContext enum value
based on the keylet function type.

Suggested mappings:
  - keylet::account()     -> LEDGER_HEADER_HASH (or new KEYLET_ACCOUNT)
  - keylet::line()        -> LEDGER_HEADER_HASH (or new KEYLET_TRUSTLINE)
  - keylet::offer()       -> LEDGER_HEADER_HASH (or new KEYLET_OFFER)
  - keylet::ownerDir()    -> LEDGER_HEADER_HASH (or new KEYLET_OWNER_DIR)
  - keylet::page()        -> LEDGER_HEADER_HASH (or new KEYLET_DIR_PAGE)
  - keylet::fees()        -> LEDGER_HEADER_HASH (or new KEYLET_FEES)
  - keylet::amendments()  -> LEDGER_HEADER_HASH (or new KEYLET_AMENDMENTS)
  - keylet::check()       -> LEDGER_HEADER_HASH (or new KEYLET_CHECK)
  - keylet::escrow()      -> LEDGER_HEADER_HASH (or new KEYLET_ESCROW)
  - keylet::payChan()     -> LEDGER_HEADER_HASH (or new KEYLET_PAYCHAN)
  - keylet::signers()     -> LEDGER_HEADER_HASH (or new KEYLET_SIGNERS)
  - keylet::ticket()      -> LEDGER_HEADER_HASH (or new KEYLET_TICKET)
  - keylet::nftpage_*()   -> LEDGER_HEADER_HASH (or new KEYLET_NFT_PAGE)
  - keylet::nftoffer()    -> LEDGER_HEADER_HASH (or new KEYLET_NFT_OFFER)
  - keylet::depositPreauth() -> LEDGER_HEADER_HASH (or new KEYLET_DEPOSIT_PREAUTH)
""")

if __name__ == "__main__":
    # Get the project root directory
    project_root = "/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc"
    
    print(f"Analyzing keylet calls in: {project_root}")
    print("This may take a moment...\n")
    
    # Find all keylet calls
    results, unique_args = find_keylet_calls(project_root)
    
    # Print the report
    print_report(results, unique_args)
    
    # Generate replacement suggestions
    generate_replacement_script(results, unique_args)