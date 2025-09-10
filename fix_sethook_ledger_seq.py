#!/usr/bin/env python3

import re
from pathlib import Path

def fix_sethook_ledger_sequences():
    filepath = Path("/Users/nicholasdudfield/projects/xahaud-worktrees/xahaud-map-stats-rpc/src/test/app/SetHook_test.cpp")
    
    with open(filepath, 'r') as f:
        content = f.read()
    
    # Fix keylet::hookState calls with hash_options{0, KEYLET_HOOK_STATE}
    # These are inside test functions where env is available
    content = re.sub(
        r'keylet::hookState\(hash_options\{0, KEYLET_HOOK_STATE\}',
        r'keylet::hookState(hash_options{(env.current()->seq()), KEYLET_HOOK_STATE}',
        content
    )
    
    # Fix keylet::hookStateDir calls with hash_options{0, KEYLET_HOOK_STATE_DIR}
    content = re.sub(
        r'keylet::hookStateDir\(hash_options\{0, KEYLET_HOOK_STATE_DIR\}',
        r'keylet::hookStateDir(hash_options{(env.current()->seq()), KEYLET_HOOK_STATE_DIR}',
        content
    )
    
    # The sha512Half_s and sha512Half calls with LEDGER_INDEX_UNNEEDED are CORRECT
    # because they're hashing non-ledger data (WASM bytecode, nonces, etc.)
    # So we leave those alone.
    
    # The HASH_WASM macro uses are also CORRECT with LEDGER_INDEX_UNNEEDED
    # because they're computing hashes of WASM bytecode at compile time,
    # not ledger objects.
    
    with open(filepath, 'w') as f:
        f.write(content)
    
    print(f"Fixed {filepath}")
    print("Note: sha512Half* calls with LEDGER_INDEX_UNNEEDED are correct (hashing non-ledger data)")
    print("Note: HASH_WASM macro uses are correct (compile-time WASM bytecode hashing)")

if __name__ == "__main__":
    fix_sethook_ledger_sequences()