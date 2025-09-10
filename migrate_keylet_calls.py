#!/usr/bin/env python3

import re
import os
import sys
import argparse
from pathlib import Path
from collections import defaultdict
from typing import Dict, List, Tuple, Optional

# Mapping of keylet functions to their specific HashContext classifiers
KEYLET_CLASSIFIERS = {
    'account': 'KEYLET_ACCOUNT',
    'amendments': 'KEYLET_AMENDMENTS',
    'book': 'KEYLET_BOOK',
    'check': 'KEYLET_CHECK',
    'child': 'KEYLET_CHILD',
    'depositPreauth': 'KEYLET_DEPOSIT_PREAUTH',
    'emittedDir': 'KEYLET_EMITTED_DIR',
    'emittedTxn': 'KEYLET_EMITTED_TXN',
    'escrow': 'KEYLET_ESCROW',
    'fees': 'KEYLET_FEES',
    'hook': 'KEYLET_HOOK',
    'hookDefinition': 'KEYLET_HOOK_DEFINITION',
    'hookState': 'KEYLET_HOOK_STATE',
    'hookStateDir': 'KEYLET_HOOK_STATE_DIR',
    'import_vlseq': 'KEYLET_IMPORT_VLSEQ',
    'line': 'KEYLET_TRUSTLINE',
    'negativeUNL': 'KEYLET_NEGATIVE_UNL',
    'nft_buys': 'KEYLET_NFT_BUYS',
    'nft_sells': 'KEYLET_NFT_SELLS',
    'nftoffer': 'KEYLET_NFT_OFFER',
    'nftpage': 'KEYLET_NFT_PAGE',
    'nftpage_max': 'KEYLET_NFT_PAGE',
    'nftpage_min': 'KEYLET_NFT_PAGE',
    'offer': 'KEYLET_OFFER',
    'ownerDir': 'KEYLET_OWNER_DIR',
    'page': 'KEYLET_DIR_PAGE',
    'payChan': 'KEYLET_PAYCHAN',
    'signers': 'KEYLET_SIGNERS',
    'skip': 'KEYLET_SKIP_LIST',
    'ticket': 'KEYLET_TICKET',
    'UNLReport': 'KEYLET_UNL_REPORT',
    'unchecked': 'KEYLET_UNCHECKED',
    'uritoken': 'KEYLET_URI_TOKEN',
}

def add_classifiers_to_digest_h(digest_h_path: str, dry_run: bool = True) -> bool:
    """Add the new KEYLET_ classifiers to digest.h if they don't exist."""
    
    # Read the file
    with open(digest_h_path, 'r') as f:
        content = f.read()
    
    # Check if we already have KEYLET_ classifiers
    if 'KEYLET_ACCOUNT' in content:
        print("KEYLET classifiers already exist in digest.h")
        return True
    
    # Find the end of the HashContext enum (before the closing brace and semicolon)
    pattern = r'(enum HashContext[^{]*\{[^}]*)(HOOK_DEFINITION\s*=\s*\d+,?)([^}]*\};)'
    
    match = re.search(pattern, content, re.DOTALL)
    if not match:
        print("ERROR: Could not find HashContext enum in digest.h")
        return False
    
    # Build the new classifiers text
    new_classifiers = []
    
    # Get the last number used (HOOK_DEFINITION = 17)
    last_num = 17
    
    # Add all KEYLET classifiers
    unique_classifiers = sorted(set(KEYLET_CLASSIFIERS.values()))
    for i, classifier in enumerate(unique_classifiers, start=1):
        new_classifiers.append(f"    {classifier} = {last_num + i},")
    
    # Join them with newlines
    new_text = '\n'.join(new_classifiers)
    
    # Create the replacement
    replacement = match.group(1) + match.group(2) + ',\n\n    // Keylet-specific hash contexts\n' + new_text + match.group(3)
    
    # Replace in content
    new_content = content[:match.start()] + replacement + content[match.end():]
    
    if dry_run:
        print("=" * 80)
        print("WOULD ADD TO digest.h:")
        print("=" * 80)
        print(new_text)
        print("=" * 80)
    else:
        with open(digest_h_path, 'w') as f:
            f.write(new_content)
        print(f"Updated {digest_h_path} with KEYLET classifiers")
    
    return True

def migrate_keylet_call(content: str, func_name: str, dry_run: bool = True) -> Tuple[str, int]:
    """
    Migrate keylet calls from single ledger_index to ledger_index + classifier.
    Returns (modified_content, number_of_replacements)
    """
    
    classifier = KEYLET_CLASSIFIERS.get(func_name)
    if not classifier:
        print(f"WARNING: No classifier mapping for keylet::{func_name}")
        return content, 0
    
    # Pattern to match keylet::<func>(hash_options{<ledger_seq>}, ...)
    # where ledger_seq doesn't already contain a comma (no classifier yet)
    pattern = re.compile(
        rf'keylet::{re.escape(func_name)}\s*\(\s*hash_options\s*\{{\s*([^,}}]+)\s*\}}',
        re.MULTILINE
    )
    
    count = 0
    
    def replacer(match):
        nonlocal count
        ledger_seq = match.group(1).strip()
        # Check if it already has a classifier (contains comma)
        if ',' in ledger_seq:
            return match.group(0)  # Already migrated
        
        count += 1
        # Add the classifier
        return f'keylet::{func_name}(hash_options{{{ledger_seq}, {classifier}}}'
    
    new_content = pattern.sub(replacer, content)
    
    return new_content, count

def process_file(filepath: str, dry_run: bool = True) -> int:
    """Process a single file. Returns number of replacements made."""
    
    with open(filepath, 'r', encoding='utf-8') as f:
        original_content = f.read()
    
    content = original_content
    total_replacements = 0
    replacements_by_func = {}
    
    # Process each keylet function
    for func_name in KEYLET_CLASSIFIERS.keys():
        new_content, count = migrate_keylet_call(content, func_name, dry_run)
        if count > 0:
            content = new_content
            total_replacements += count
            replacements_by_func[func_name] = count
    
    if total_replacements > 0:
        if dry_run:
            print(f"Would modify {filepath}: {total_replacements} replacements")
            for func, count in sorted(replacements_by_func.items()):
                print(f"  - keylet::{func}: {count}")
        else:
            with open(filepath, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"Modified {filepath}: {total_replacements} replacements")
            for func, count in sorted(replacements_by_func.items()):
                print(f"  - keylet::{func}: {count}")
    
    return total_replacements

def main():
    parser = argparse.ArgumentParser(
        description='Migrate keylet calls to use HashContext classifiers'
    )
    parser.add_argument(
        '--dry-run',
        action='store_true',
        default=True,
        help='Show what would be changed without modifying files (default: True)'
    )
    parser.add_argument(
        '--apply',
        action='store_true',
        help='Actually apply the changes (disables dry-run)'
    )
    parser.add_argument(
        '--file',
        help='Process a specific file only'
    )
    parser.add_argument(
        '--add-classifiers',
        action='store_true',
        help='Add KEYLET_ classifiers to digest.h'
    )
    
    args = parser.parse_args()
    
    if args.apply:
        args.dry_run = False
    
    project_root = "/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc"
    
    # First, optionally add classifiers to digest.h
    if args.add_classifiers:
        digest_h = os.path.join(project_root, "src/ripple/protocol/digest.h")
        if not add_classifiers_to_digest_h(digest_h, args.dry_run):
            return 1
        print()
    
    # Process files
    if args.file:
        # Process single file
        filepath = os.path.join(project_root, args.file)
        if not os.path.exists(filepath):
            print(f"ERROR: File not found: {filepath}")
            return 1
        
        process_file(filepath, args.dry_run)
    else:
        # Process all files
        total_files = 0
        total_replacements = 0
        
        print(f"{'DRY RUN: ' if args.dry_run else ''}Processing files in {project_root}/src/ripple")
        print("=" * 80)
        
        for root, dirs, files in os.walk(Path(project_root) / "src" / "ripple"):
            dirs[:] = [d for d in dirs if d not in ['.git', 'build', '__pycache__']]
            
            for file in files:
                if file.endswith(('.cpp', '.h', '.hpp')):
                    filepath = os.path.join(root, file)
                    count = process_file(filepath, args.dry_run)
                    if count > 0:
                        total_files += 1
                        total_replacements += count
        
        print("=" * 80)
        print(f"{'Would modify' if args.dry_run else 'Modified'} {total_files} files")
        print(f"Total replacements: {total_replacements}")
        
        if args.dry_run:
            print("\nTo apply these changes, run with --apply flag:")
            print(f"  python3 {sys.argv[0]} --apply")
            print("\nTo first add classifiers to digest.h:")
            print(f"  python3 {sys.argv[0]} --add-classifiers --apply")

if __name__ == "__main__":
    sys.exit(main())