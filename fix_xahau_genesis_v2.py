#!/usr/bin/env python3

import re
from pathlib import Path

def fix_xahau_genesis():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/XahauGenesis_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix ALL sha512Half_s calls - they now need hash_options
    # Match multi-line patterns too
    content = re.sub(
        r'ripple::sha512Half_s\(',
        r'ripple::sha512Half_s(hash_options{0, LEDGER_INDEX_UNNEEDED}, ',
        content
    )
    
    # Fix keylet::hookDefinition without hash_options
    content = re.sub(
        r'keylet::hookDefinition\(([^,)]+)\)(?!\.)',
        r'keylet::hookDefinition(hash_options{0, KEYLET_HOOK_DEFINITION}, \1)',
        content
    )
    
    # Fix env.le(keylet::hookDefinition calls that might have been missed
    content = re.sub(
        r'env\.le\(keylet::hookDefinition\(hash_options\{0, KEYLET_HOOK_DEFINITION\}, ([^)]+)\)\)',
        r'env.le(keylet::hookDefinition(hash_options{(env.current()->seq()), KEYLET_HOOK_DEFINITION}, \1))',
        content
    )
    
    # Fix keylet::account in view.read() calls
    content = re.sub(
        r'view->read\(keylet::account\((\w+)\)\)',
        r'view->read(keylet::account(hash_options{(view->seq()), KEYLET_ACCOUNT}, \1))',
        content
    )
    
    # Fix env.current()->read(keylet::account calls
    content = re.sub(
        r'env\.current\(\)->read\(keylet::account\((\w+)\)\)',
        r'env.current()->read(keylet::account(hash_options{(env.current()->seq()), KEYLET_ACCOUNT}, \1))',
        content
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_xahau_genesis()