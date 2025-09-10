#!/usr/bin/env python3

import re
import os
import sys
from pathlib import Path

def fix_test_files(root_dir: str):
    """Fix hash_options in test files by adding appropriate classifiers."""
    
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
        'fees': 'KEYLET_FEES',
        'amendments': 'KEYLET_AMENDMENTS',
        'negativeUNL': 'KEYLET_NEGATIVE_UNL',
        'skip': 'KEYLET_SKIP_LIST',
        'unchecked': 'KEYLET_UNCHECKED',
        'import_vlseq': 'KEYLET_IMPORT_VLSEQ',
        'UNLReport': 'KEYLET_UNL_REPORT',
        'emittedDir': 'KEYLET_EMITTED_DIR',
        'emittedTxn': 'KEYLET_EMITTED_TXN',
        'book': 'KEYLET_BOOK',
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
            
            # Process line by line for better control
            lines = content.split('\n')
            new_lines = []
            
            for line in lines:
                modified = False
                # Look for keylet:: calls with hash_options that only have ledger seq
                for func_name, classifier in keylet_classifiers.items():
                    # Pattern for keylet::func(...hash_options{(ledger_seq)}...)
                    pattern = f'keylet::{func_name}\\s*\\([^)]*hash_options\\s*\\{{\\s*\\(([^}}]+)\\)\\s*\\}}'
                    
                    matches = list(re.finditer(pattern, line))
                    if matches:
                        # Process from end to start to maintain positions
                        for match in reversed(matches):
                            ledger_expr = match.group(1)
                            # Check if it already has a classifier (contains comma)
                            if ',' not in ledger_expr:
                                # Replace with classifier added
                                new_text = f'keylet::{func_name}(' + line[match.start():match.end()].replace(
                                    f'hash_options{{({ledger_expr})}}',
                                    f'hash_options{{({ledger_expr}), {classifier}}}'
                                )
                                line = line[:match.start()] + new_text + line[match.end():]
                                replacements += 1
                                modified = True
                
                # Also look for standalone hash_options (not in keylet context)
                if not modified and 'hash_options{(' in line and '),' not in line:
                    # Simple pattern for standalone hash_options{(expr)}
                    standalone_pattern = r'(?<!keylet::\w{1,30}\([^)]*\s{0,5})hash_options\s*\{\s*\(([^}]+)\)\s*\}'
                    matches = list(re.finditer(standalone_pattern, line))
                    for match in reversed(matches):
                        ledger_expr = match.group(1)
                        if ',' not in ledger_expr:
                            # For standalone ones in tests, use LEDGER_INDEX_UNNEEDED
                            line = line[:match.start()] + f'hash_options{{({ledger_expr}), LEDGER_INDEX_UNNEEDED}}' + line[match.end():]
                            replacements += 1
                
                new_lines.append(line)
            
            if replacements > 0:
                new_content = '\n'.join(new_lines)
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