#!/usr/bin/env python3

import re
from pathlib import Path

def fix_xahau_genesis():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/XahauGenesis_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix sha512Half_s calls - they now need hash_options
    content = re.sub(
        r'ripple::sha512Half_s\(ripple::Slice\(',
        r'ripple::sha512Half_s(hash_options{0, LEDGER_INDEX_UNNEEDED}, ripple::Slice(',
        content
    )
    
    # Fix keylet::amendments()
    content = re.sub(
        r'env\.le\(keylet::amendments\(\)\)',
        r'env.le(keylet::amendments(hash_options{(env.current()->seq()), KEYLET_AMENDMENTS}))',
        content
    )
    
    # Fix keylet::account(id) calls
    content = re.sub(
        r'env\.le\(keylet::account\((\w+)\)\)',
        r'env.le(keylet::account(hash_options{(env.current()->seq()), KEYLET_ACCOUNT}, \1))',
        content
    )
    
    # Fix keylet::hook(id) calls
    content = re.sub(
        r'env\.le\(keylet::hook\((\w+)\)\)',
        r'env.le(keylet::hook(hash_options{(env.current()->seq()), KEYLET_HOOK}, \1))',
        content
    )
    
    # Fix keylet::hookDefinition calls
    content = re.sub(
        r'env\.le\(keylet::hookDefinition\(([^)]+)\)\)',
        r'env.le(keylet::hookDefinition(hash_options{(env.current()->seq()), KEYLET_HOOK_DEFINITION}, \1))',
        content
    )
    
    # Fix keylet::fees() calls
    content = re.sub(
        r'env\.le\(keylet::fees\(\)\)',
        r'env.le(keylet::fees(hash_options{(env.current()->seq()), KEYLET_FEES}))',
        content
    )
    
    # Fix standalone keylet::account assignments
    content = re.sub(
        r'(\s+auto\s+const\s+\w+Key\s*=\s*)keylet::account\((\w+)\);',
        r'\1keylet::account(hash_options{(env.current()->seq()), KEYLET_ACCOUNT}, \2);',
        content
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_xahau_genesis()