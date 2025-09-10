#!/usr/bin/env python3

import re
from pathlib import Path

def fix_sethook_test():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/SetHook_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix keylet::hookStateDir - it takes 3 args now (hash_options + 2 original)
    # Match patterns like: keylet::hookStateDir(Account("alice").id(), ns)
    content = re.sub(
        r'keylet::hookStateDir\(Account\(([^)]+)\)\.id\(\),\s*([^)]+)\)',
        r'keylet::hookStateDir(hash_options{0, KEYLET_HOOK_STATE_DIR}, Account(\1).id(), \2)',
        content
    )
    
    # Match with variables
    content = re.sub(
        r'keylet::hookStateDir\((\w+)\.id\(\),\s*([^)]+)\)',
        r'keylet::hookStateDir(hash_options{0, KEYLET_HOOK_STATE_DIR}, \1.id(), \2)',
        content
    )
    
    # Fix multiline hookStateDir patterns
    content = re.sub(
        r'keylet::hookStateDir\(\s*\n\s*Account\(([^)]+)\)\.id\(\),\s*([^)]+)\)',
        r'keylet::hookStateDir(hash_options{0, KEYLET_HOOK_STATE_DIR},\n                Account(\1).id(), \2)',
        content,
        flags=re.MULTILINE
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_sethook_test()