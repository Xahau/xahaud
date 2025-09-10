#!/usr/bin/env python3

import re
from pathlib import Path

def fix_sethook_test():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/SetHook_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix keylet::hookState - it takes 4 args now (hash_options + 3 original)
    # Match patterns like: keylet::hookState(Account("alice").id(), key, ns)
    content = re.sub(
        r'keylet::hookState\(Account\(([^)]+)\)\.id\(\),\s*([^,]+),\s*([^)]+)\)',
        r'keylet::hookState(hash_options{0, KEYLET_HOOK_STATE}, Account(\1).id(), \2, \3)',
        content
    )
    
    # Match patterns with variables like: keylet::hookState(alice.id(), key, ns)
    content = re.sub(
        r'keylet::hookState\((\w+)\.id\(\),\s*([^,]+),\s*([^)]+)\)',
        r'keylet::hookState(hash_options{0, KEYLET_HOOK_STATE}, \1.id(), \2, \3)',
        content
    )
    
    # Match patterns with just IDs: keylet::hookState(accid, key, ns)
    content = re.sub(
        r'keylet::hookState\((\w+),\s*([^,]+),\s*([^)]+)\)(?!\))',
        r'keylet::hookState(hash_options{0, KEYLET_HOOK_STATE}, \1, \2, \3)',
        content
    )
    
    # Fix keylet::hookStateDir patterns
    content = re.sub(
        r'keylet::hookStateDir\((\w+),\s*([^,]+),\s*([^)]+)\)',
        r'keylet::hookStateDir(hash_options{0, KEYLET_HOOK_STATE_DIR}, \1, \2, \3)',
        content
    )
    
    # Fix sha512Half_s calls without hash_options  
    content = re.sub(
        r'sha512Half_s\(ripple::Slice\(',
        r'sha512Half_s(hash_options{0, LEDGER_INDEX_UNNEEDED}, ripple::Slice(',
        content
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_sethook_test()