#!/usr/bin/env python3

import re
import os
import sys
from pathlib import Path

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

def fix_keylet_calls_in_file(filepath: Path) -> int:
    """Fix hash_options in a single file by adding appropriate classifiers."""
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return 0
    
    original_content = content
    replacements = 0
    
    # Process each keylet function
    for func_name, classifier in KEYLET_CLASSIFIERS.items():
        # Pattern to match keylet::<func>(hash_options{<ledger_seq>}, ...)
        # where ledger_seq doesn't already contain a comma (no classifier yet)
        pattern = re.compile(
            rf'keylet::{re.escape(func_name)}\s*\(\s*hash_options\s*\{{\s*\(([^}}]+)\)\s*\}}',
            re.MULTILINE
        )
        
        def replacer(match):
            nonlocal replacements
            ledger_seq = match.group(1).strip()
            # Check if it already has a classifier (contains comma after the ledger expression)
            # But be careful - the ledger expression itself might contain commas
            # Look for a comma followed by a KEYLET_ or other classifier
            if ', KEYLET_' in match.group(0) or ', LEDGER_' in match.group(0):
                return match.group(0)
            replacements += 1
            # Add the classifier
            return f'keylet::{func_name}(hash_options{{({ledger_seq}), {classifier}}}'
        
        content = pattern.sub(replacer, content)
    
    if replacements > 0 and content != original_content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        return replacements
    
    return 0

def main():
    project_root = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc")
    
    # Find all test cpp files that might have hash_options
    test_files = list((project_root / "src" / "test").rglob("*.cpp"))
    
    print(f"Found {len(test_files)} test files to check...")
    
    total_files_fixed = 0
    total_replacements = 0
    
    for filepath in test_files:
        replacements = fix_keylet_calls_in_file(filepath)
        if replacements > 0:
            rel_path = filepath.relative_to(project_root)
            print(f"Fixed {rel_path}: {replacements} replacements")
            total_files_fixed += 1
            total_replacements += replacements
    
    print(f"\n{'='*60}")
    print(f"Fixed {total_files_fixed} test files")
    print(f"Total replacements: {total_replacements}")
    
    if total_files_fixed > 0:
        print("\nNow rebuild to see if there are any remaining issues.")
    else:
        print("\nNo test files needed fixing.")

if __name__ == "__main__":
    main()