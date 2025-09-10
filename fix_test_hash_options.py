#!/usr/bin/env python3

import re
import os
import sys
from pathlib import Path

def fix_test_files(root_dir: str):
    """Fix hash_options in test files by adding appropriate classifiers."""
    
    # Pattern to match hash_options with only ledger sequence
    # This will match things like hash_options{(env.current()->seq())}
    pattern = re.compile(
        r'(keylet::(\w+)\s*\([^)]*\s*)hash_options\s*\{\s*\(([^}]+)\)\s*\}',
        re.MULTILINE
    )
    
    # Mapping of keylet functions to their classifiers
    keylet_classifiers = {
        'account': 'KEYLET_ACCOUNT',
        'ownerDir': 'KEYLET_OWNER_DIR',
        'signers': 'KEYLET_SIGNERS',
        'offer': 'KEYLET_OFFER',
        'check': 'KEYLET_CHECK',
        'depositPreauth': 'KEYLET_DEPOSIT_PREAUTH',
        'escrow': 'KEYLET_ESCROW',
        'payChan': 'KEYLET_PAYCHAN',
        'line': 'KEYLET_TRUSTLINE',
        'ticket': 'KEYLET_TICKET',
        'hook': 'KEYLET_HOOK',
        'hookDefinition': 'KEYLET_HOOK_DEFINITION',
        'hookState': 'KEYLET_HOOK_STATE',
        'hookStateDir': 'KEYLET_HOOK_STATE_DIR',
        'child': 'KEYLET_CHILD',
        'page': 'KEYLET_DIR_PAGE',
        'nftpage_min': 'KEYLET_NFT_PAGE',
        'nftpage_max': 'KEYLET_NFT_PAGE',
        'nftoffer': 'KEYLET_NFT_OFFER',
        'nft_buys': 'KEYLET_NFT_BUYS',
        'nft_sells': 'KEYLET_NFT_SELLS',
        'uritoken': 'KEYLET_URI_TOKEN',
    }
    
    files_fixed = 0
    total_replacements = 0
    
    # Find all test files
    test_dir = Path(root_dir) / "src" / "test"
    
    for filepath in test_dir.rglob("*.cpp"):
        try:
            with open(filepath, 'r', encoding='utf-8') as f:
                original_content = f.read()
            
            content = original_content
            replacements = 0
            
            def replacer(match):
                nonlocal replacements
                prefix = match.group(1)
                keylet_func = match.group(2)
                ledger_expr = match.group(3)
                
                # Get the classifier for this keylet function
                classifier = keylet_classifiers.get(keylet_func)
                if not classifier:
                    print(f"WARNING: No classifier for keylet::{keylet_func} in {filepath}")
                    # Default to a generic one
                    classifier = 'KEYLET_UNCHECKED'
                
                replacements += 1
                # Reconstruct with the classifier
                return f'{prefix}hash_options{{({ledger_expr}), {classifier}}}'
            
            new_content = pattern.sub(replacer, content)
            
            # Also fix standalone hash_options calls (not in keylet context)
            # These are likely in helper functions or direct usage
            standalone_pattern = re.compile(
                r'(?<!keylet::\w{1,50}\s{0,10}\([^)]*\s{0,10})hash_options\s*\{\s*\(([^}]+)\)\s*\}',
                re.MULTILINE
            )
            
            def standalone_replacer(match):
                nonlocal replacements
                ledger_expr = match.group(1)
                replacements += 1
                # For standalone ones in tests, use a test context
                return f'hash_options{{({ledger_expr}), LEDGER_INDEX_UNNEEDED}}'
            
            new_content = standalone_pattern.sub(standalone_replacer, new_content)
            
            if replacements > 0:
                with open(filepath, 'w', encoding='utf-8') as f:
                    f.write(new_content)
                
                rel_path = filepath.relative_to(root_dir)
                print(f"Fixed {rel_path}: {replacements} replacements")
                files_fixed += 1
                total_replacements += replacements
                
        except Exception as e:
            print(f"Error processing {filepath}: {e}")
    
    print(f"\n{'='*60}")
    print(f"Fixed {files_fixed} test files")
    print(f"Total replacements: {total_replacements}")
    
    return files_fixed, total_replacements

if __name__ == "__main__":
    project_root = "/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc"
    
    print("Fixing hash_options in test files...")
    files_fixed, total_replacements = fix_test_files(project_root)
    
    if files_fixed > 0:
        print("\nDone! Now rebuild to see if there are any remaining issues.")
    else:
        print("\nNo test files needed fixing.")