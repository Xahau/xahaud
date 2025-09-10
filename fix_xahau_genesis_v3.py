#!/usr/bin/env python3

import re
from pathlib import Path

def fix_xahau_genesis():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/XahauGenesis_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix keylet::signers without hash_options
    content = re.sub(
        r'env\.le\(keylet::signers\((\w+)\)\)',
        r'env.le(keylet::signers(hash_options{(env.current()->seq()), KEYLET_SIGNERS}, \1))',
        content
    )
    
    # Fix keylet::hookState - it takes 4 arguments now (hash_options + 3 original)
    content = re.sub(
        r'keylet::hookState\(\s*([^,]+),\s*([^,]+),\s*([^)]+)\)',
        r'keylet::hookState(hash_options{(env.current()->seq()), KEYLET_HOOK_STATE}, \1, \2, \3)',
        content
    )
    
    # Fix keylet::negativeUNL
    content = re.sub(
        r'env\.le\(keylet::negativeUNL\(\)\)',
        r'env.le(keylet::negativeUNL(hash_options{(env.current()->seq()), KEYLET_NEGATIVE_UNL}))',
        content
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_xahau_genesis()