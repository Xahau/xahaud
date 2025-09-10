#!/usr/bin/env python3

import re
from pathlib import Path

def fix_keylet_without_hash_options(filepath: Path) -> int:
    """Fix keylet calls without hash_options in test files."""
    
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            content = f.read()
    except Exception as e:
        print(f"Error reading {filepath}: {e}")
        return 0
    
    original_content = content
    replacements = 0
    
    # Pattern to match keylet calls without hash_options
    # E.g., keylet::ownerDir(acct.id()) or keylet::account(alice)
    keylet_funcs = {
        'ownerDir': 'KEYLET_OWNER_DIR',
        'account': 'KEYLET_ACCOUNT', 
        'signers': 'KEYLET_SIGNERS',
        'offer': 'KEYLET_OFFER',
        'line': 'KEYLET_TRUSTLINE',
        'check': 'KEYLET_CHECK',
        'escrow': 'KEYLET_ESCROW',
        'payChan': 'KEYLET_PAYCHAN',
        'depositPreauth': 'KEYLET_DEPOSIT_PREAUTH',
        'ticket': 'KEYLET_TICKET',
        'nftoffer': 'KEYLET_NFT_OFFER',
        'fees': 'KEYLET_FEES',
        'amendments': 'KEYLET_AMENDMENTS',
        'negativeUNL': 'KEYLET_NEGATIVE_UNL',
        'skip': 'KEYLET_SKIP_LIST',
        'hook': 'KEYLET_HOOK',
        'hookDefinition': 'KEYLET_HOOK_DEFINITION',
        'hookState': 'KEYLET_HOOK_STATE',
        'hookStateDir': 'KEYLET_HOOK_STATE_DIR',
        'emittedDir': 'KEYLET_EMITTED_DIR',
        'emittedTxn': 'KEYLET_EMITTED_TXN',
        'import_vlseq': 'KEYLET_IMPORT_VLSEQ',
        'unchecked': 'KEYLET_UNCHECKED',
        'uritoken': 'KEYLET_URI_TOKEN',
        'nftpage': 'KEYLET_NFT_PAGE',
        'nftpage_min': 'KEYLET_NFT_PAGE',
        'nftpage_max': 'KEYLET_NFT_PAGE',
        'nft_buys': 'KEYLET_NFT_BUYS',
        'nft_sells': 'KEYLET_NFT_SELLS',
        'child': 'KEYLET_CHILD',
        'page': 'KEYLET_DIR_PAGE',
        'UNLReport': 'KEYLET_UNL_REPORT',
        'book': 'KEYLET_BOOK'
    }
    
    for func, classifier in keylet_funcs.items():
        # Pattern to match keylet::<func>(...) where the args don't start with hash_options
        pattern = re.compile(
            rf'\bkeylet::{re.escape(func)}\s*\(\s*(?!hash_options)',
            re.MULTILINE
        )
        
        matches = list(pattern.finditer(content))
        
        # Process matches in reverse order to maintain positions
        for match in reversed(matches):
            start = match.start()
            # Find the matching closing parenthesis
            paren_count = 1
            pos = match.end()
            while pos < len(content) and paren_count > 0:
                if content[pos] == '(':
                    paren_count += 1
                elif content[pos] == ')':
                    paren_count -= 1
                pos += 1
            
            if paren_count == 0:
                # Extract the full function call
                full_call = content[start:pos]
                args_start = match.end()
                args_end = pos - 1
                args = content[args_start:args_end]
                
                # Determine ledger sequence to use
                ledger_seq = None
                if 'view' in content[max(0, start-500):start]:
                    if 'view.seq()' in content[max(0, start-500):start]:
                        ledger_seq = '(view.seq())'
                    else:
                        ledger_seq = '0'
                elif 'env' in content[max(0, start-500):start]:
                    if 'env.current()' in content[max(0, start-500):start]:
                        ledger_seq = '(env.current()->seq())'
                    else:
                        ledger_seq = '0'
                else:
                    ledger_seq = '0'
                
                # Build the new call
                new_call = f'keylet::{func}(hash_options{{{ledger_seq}, {classifier}}}, {args})'
                
                # Replace in content
                content = content[:start] + new_call + content[pos:]
                replacements += 1
    
    if replacements > 0 and content != original_content:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(content)
        return replacements
    
    return 0

def main():
    project_root = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc")
    
    # Files to fix
    test_files = [
        "src/test/app/URIToken_test.cpp",
        "src/test/app/Touch_test.cpp",
        "src/test/app/SetRemarks_test.cpp",
        "src/test/app/Remit_test.cpp",
        "src/test/app/HookNegatives_test.cpp",
        "src/test/app/Hook_test.cpp", 
        "src/test/app/NFTokenBurn_test.cpp",
        "src/test/app/NFToken_test.cpp",
        "src/test/app/TxMeta_test.cpp",
        "src/test/app/AccountTxPaging_test.cpp"
    ]
    
    total_replacements = 0
    
    for rel_path in test_files:
        filepath = project_root / rel_path
        if filepath.exists():
            replacements = fix_keylet_without_hash_options(filepath)
            if replacements > 0:
                print(f"Fixed {rel_path}: {replacements} replacements")
                total_replacements += replacements
    
    print(f"\nTotal replacements: {total_replacements}")

if __name__ == "__main__":
    main()