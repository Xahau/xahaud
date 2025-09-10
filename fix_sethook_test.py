#!/usr/bin/env python3

import re
from pathlib import Path

def fix_sethook_test():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/SetHook_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix keylet::hook(Account(...).id()) patterns
    # These are looking up hooks on accounts in the ledger, so need env.current()->seq()
    content = re.sub(
        r'env\.le\(keylet::hook\(Account\(([^)]+)\)\.id\(\)\)\)',
        r'env.le(keylet::hook(hash_options{(env.current()->seq()), KEYLET_HOOK}, Account(\1).id()))',
        content
    )
    
    # Fix other keylet::hook patterns with just an ID
    content = re.sub(
        r'env\.le\(keylet::hook\((\w+)\.id\(\)\)\)',
        r'env.le(keylet::hook(hash_options{(env.current()->seq()), KEYLET_HOOK}, \1.id()))',
        content
    )
    
    # Fix patterns like keylet::hook(alice)
    content = re.sub(
        r'env\.le\(keylet::hook\((\w+)\)\)',
        r'env.le(keylet::hook(hash_options{(env.current()->seq()), KEYLET_HOOK}, \1))',
        content
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_sethook_test()