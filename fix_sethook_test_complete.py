#!/usr/bin/env python3

import re
from pathlib import Path

def fix_sethook_test():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/SetHook_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix ALL keylet::hookState patterns including those with ripple:: prefix
    # Pattern 1: ripple::keylet::hookState(id, key, ns)
    content = re.sub(
        r'ripple::keylet::hookState\(([^,]+),\s*([^,]+),\s*([^)]+)\)',
        r'ripple::keylet::hookState(hash_options{(env.current()->seq()), KEYLET_HOOK_STATE}, \1, \2, \3)',
        content
    )
    
    # Pattern 2: keylet::hookState without ripple:: prefix (if not already fixed)
    content = re.sub(
        r'(?<!ripple::)keylet::hookState\(([^h][^,]+),\s*([^,]+),\s*([^)]+)\)',
        r'keylet::hookState(hash_options{(env.current()->seq()), KEYLET_HOOK_STATE}, \1, \2, \3)',
        content
    )
    
    # Fix ripple::keylet::hookStateDir patterns
    content = re.sub(
        r'ripple::keylet::hookStateDir\(([^,]+),\s*([^)]+)\)',
        r'ripple::keylet::hookStateDir(hash_options{(env.current()->seq()), KEYLET_HOOK_STATE_DIR}, \1, \2)',
        content
    )
    
    # Fix ripple::keylet::hook patterns
    content = re.sub(
        r'ripple::keylet::hook\(([^)]+)\)(?!\))',
        r'ripple::keylet::hook(hash_options{(env.current()->seq()), KEYLET_HOOK}, \1)',
        content
    )
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")

if __name__ == "__main__":
    fix_sethook_test()