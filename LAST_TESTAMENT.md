# Last Testament: SHA-512 to BLAKE3 Migration Deep Dive

## The Journey
Started with "just change the hash function" - ended up discovering why hash functions are permanent blockchain decisions.

## Key Discoveries

### 1. Ledger Implementation Added
- `Ledger::shouldMigrateToBlake3()` - checks migration window/flags
- `Ledger::migrateToBlake3()` - placeholder for actual migration
- Hook in `BuildLedger.cpp` after transaction processing
- Migration happens OUTSIDE transaction pipeline to avoid metadata explosion

### 2. The Rekeying Nightmare (REKEYING_ISSUES.md)
Every object key changes SHA512→BLAKE3, but keys are EVERYWHERE:
- **DirectoryNode.sfIndexes** - vectors of keys
- **sfPreviousTxnID, sfIndexNext, sfIndexPrevious** - direct key refs  
- **Order books** - sorted by key value (order changes!)
- **Hook state** - arbitrary blobs with embedded keys
- **620k objects** with millions of interconnected references

### 3. The LUT Approach
```cpp
// Build lookup table: old_key → new_key (40MB for 620k entries)
// O(n) to build, O(n×m) to update all fields
// Must check EVERY uint256 field in EVERY object
```
**Problems:**
- LUT check on EVERY lookup forever (performance tax)
- Can't know if key is old/new without checking
- Might need bidirectional LUT (80MB)
- Can NEVER remove it (WASM has hardcoded keys!)

### 4. The WASM Hook Bomb
Hook code can hardcode keys in compiled WASM:
- Can't modify without changing hook hash
- Changing hash breaks all references to hook
- Literally unfixable without breaking hooks

### 5. MapStats Enhancement
Added ledger entry type tracking:
- Count, total bytes, avg size per type
- Uses `LedgerFormats::getInstance().findByType()` for names
- Shows 124k HookState entries (potential key references!)

### 6. Current Ledger Stats
- 620k total objects, 98MB total
- 117k DirectoryNodes (full of references)
- 124k HookState entries (arbitrary data)
- 80 HookDefinitions (WASM code)
- Millions of internal key references

## Why It's Impossible

### The Lookup Problem
```cpp
auto key = keylet::account(Alice).key;
// Which hash function? SHA512 or BLAKE3?
// NO WAY TO KNOW without timestamp/LUT/double-lookup
```

### The Fundamental Issues
1. **No key timestamp** - Can't tell if key is pre/post migration
2. **Embedded references everywhere** - sfIndexes, hook state, WASM code
3. **Permanent LUT required** - Check on every operation forever
4. **Performance death** - 2x lookups or LUT check on everything
5. **Dual-key SHAMap impossible** - Breaks ordering/structure

### The "Solutions" That Don't Work
- **Lazy migration**: LUT forever, complexity forever
- **Big bang**: Still need permanent LUT for old references  
- **Heterogeneous tree**: Can't mix hash functions in Merkle tree
- **Binary search-replace**: Could corrupt data, no validation
- **Import to v2 chain**: Same reference update problems

## The Verdict

After threading ledger_index through 1000+ functions for hash context, the migration faces insurmountable challenges:

1. **WASM hooks** contain unfixable hardcoded keys
2. **Every lookup** needs LUT check forever (performance tax)
3. **Can't determine key age** from identifier alone
4. **Millions of references** need perfect updating
5. **One mistake** = corrupted ledger, lost funds

**Conclusion**: This is a "v2 blockchain" problem, not a migration problem. SHA-512 is forever.

## Lessons Learned

- Hash functions are THE fundamental addressing system
- Deeper than consensus, deeper than data structures
- Once chosen, essentially permanent
- The attempt revealed the true complexity of blockchain internals
- Even 98MB of data has millions of interconnected references
- WASM hooks make any migration effectively impossible

**Final Status**: Technically possible with permanent LUT and massive complexity. Practically impossible due to hooks, performance, and risk. SHA-512 until heat death of universe.

## Code Artifacts

- Hash classification system in `digest.h`
- `hash_options` threaded through Indexes/keylets
- Ledger migration methods (placeholder)
- MapStats with entry type breakdown
- REKEYING_ISSUES.md with full analysis
- 100+ files modified to thread ledger context

The migration died not from lack of effort, but from the discovery that some architectural decisions are truly permanent.