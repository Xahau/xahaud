# Hash Migration Rekeying Issues

## The Fundamental Problem

When migrating from SHA-512 Half to BLAKE3, we're not just changing a hash function - we're changing the **keys** that identify every object in the ledger's state map. Since the hash IS the key in the SHAMap, every object needs a new address.

## What Needs to be Rekeyed

### 1. Primary Ledger Objects
Every SLE (STLedgerEntry) in the state map has its key computed from:
- Its type (Account, Offer, RippleState, etc.)
- Its identifying data (AccountID, offer sequence, etc.)

When we change the hash function, EVERY object gets a new key.

### 2. Directory Structures
Directories are ledger objects that contain **lists of other objects' keys**:

#### Owner Directories (`keylet::ownerDir`)
- Contains keys of all objects owned by an account
- Every offer, escrow, check, payment channel, etc. key stored here
- When those objects are rekeyed, ALL these references must be updated

#### Order Book Directories (`keylet::book`, `keylet::quality`)
- Contains keys of offers at specific quality levels
- All offer keys must be updated to their new BLAKE3 values

#### NFT Directories (`keylet::nft_buys`, `keylet::nft_sells`)
- Contains keys of NFT offers
- All NFT offer keys must be updated

#### Hook State Directories (`keylet::hookStateDir`)
- Contains keys of hook state entries
- All hook state keys must be updated

### 3. Cross-References Between Objects
Many objects contain direct references to other objects:

#### Account Objects
- `sfNFTokenPage` - References to NFT page keys
- Previous/Next links in directory pages

#### Directory Pages
- `sfIndexes` - Vector of object keys
- `sfPreviousTxnID` - Transaction hash references
- `sfIndexPrevious`/`sfIndexNext` - Links to other directory pages

#### NFT Pages
- References to previous/next pages in the chain

## The Cascade Effect

Re-keying isn't a simple one-pass operation:

```
1. Rekey Account A (SHA512 → BLAKE3)
   ↓
2. Update Account A's owner directory
   ↓
3. Rekey the owner directory itself
   ↓
4. Update all objects IN the directory with new keys
   ↓
5. Update any directories THOSE objects appear in
   ↓
6. Continue cascading...
```

## Implementation Challenges

### Challenge 1: Directory Entry Updates
```cpp
// Current directory structure
STVector256 indexes = directory->getFieldV256(sfIndexes);
// Contains: [sha512_key1, sha512_key2, sha512_key3, ...]

// After migration needs to be:
// [blake3_key1, blake3_key2, blake3_key3, ...]
```

### Challenge 2: Finding All References
There's no reverse index - given an object's key, you can't easily find all directories that reference it. You'd need to:
1. Walk the entire state map
2. Check every directory's `sfIndexes` field
3. Update any matching keys

### Challenge 3: Maintaining Consistency
During migration, you need to ensure:
- No orphaned references (keys pointing to non-existent objects)
- No duplicate entries
- Proper ordering in sorted structures (offer books)

### Challenge 4: Page Links
Directory pages link to each other:
```cpp
uint256 prevPage = dir->getFieldU256(sfIndexPrevious);
uint256 nextPage = dir->getFieldU256(sfIndexNext);
```
These links are also keys that need updating!

## Why This Makes Migration Complex

### Option A: Big Bang Migration
- Must update EVERYTHING atomically
- Need to track old→new key mappings for entire ledger
- Memory requirements: ~2x the state size for mapping table
- Risk: Any missed reference breaks the ledger

### Option B: Heterogeneous Tree
- Old nodes keep SHA-512 keys
- New/modified nodes use BLAKE3
- Problem: How do you know which hash to use for lookups?
- Problem: Directory contains mix of old and new keys?

### Option C: Double Storage
- Store objects under BOTH keys temporarily
- Gradually migrate references
- Problem: Massive storage overhead
- Problem: Synchronization between copies

## Example: Rekeying an Offer

Consider rekeying a single offer:

1. **The Offer Itself**
   - Old key: `sha512Half(OFFER, account, sequence)`
   - New key: `blake3(OFFER, account, sequence)`

2. **Owner Directory**
   - Must update `sfIndexes` to replace old offer key with new

3. **Order Book Directory**
   - Must update `sfIndexes` in the quality directory
   - May need to update multiple quality levels if offer moved

4. **Account Object**
   - Update offer count/reserve tracking if needed

5. **The Directories Themselves**
   - Owner directory key: `sha512Half(OWNER_DIR, account, page)`
   - New key: `blake3(OWNER_DIR, account, page)`
   - Order book key: `sha512Half(BOOK_DIR, ...)`
   - New key: `blake3(BOOK_DIR, ...)`

## Potential Solutions

### 1. Migration Ledger Object
Create a temporary "migration map" ledger object:
```cpp
sfOldKey → sfNewKey mappings
```
But this could be gigabytes for millions of objects.

### 2. Deterministic Rekeying
Since we can determine an object's type from its `LedgerEntryType`, we could:
1. Load each SLE
2. Determine its type
3. Recompute its key with BLAKE3
4. Track the mapping

But we still need to update all references.

### 3. Lazy Migration
Only rekey objects when they're modified:
- Pro: Spreads migration over time
- Con: Permanent complexity in codebase
- Con: Must support both hash types forever

### 4. New State Structure
Instead of migrating in-place, build an entirely new state map:
1. Create new empty BLAKE3 SHAMap
2. Walk old map, inserting with new keys
3. Update all references during copy
4. Atomically swap maps

This is essentially what BUILD_LEDGER.md suggests, but the reference updating remains complex.

## The Lookup Table Approach

After further analysis, a lookup table (LUT) based approach might actually be feasible:

### Algorithm Overview

#### Phase 1: Build the LUT (O(n))
```cpp
std::unordered_map<uint256, uint256> old_to_new;

stateMap_.visitLeaves([&](SHAMapItem const& item) {
    SerialIter sit(item.slice());
    auto sle = std::make_shared<SLE>(sit, item.key());
    
    // Determine type from the SLE
    LedgerEntryType type = sle->getType();
    
    // Recompute key with BLAKE3 based on type
    uint256 newKey = computeBlake3Key(sle, type);
    old_to_new[item.key()] = newKey;
});
// Results in ~620k entries in the LUT
```

#### Phase 2: Update ALL uint256 Fields (O(n × m))
Walk every object and check every uint256 field against the LUT:

```cpp
stateMap_.visitLeaves([&](SHAMapItem& item) {
    SerialIter sit(item.slice());
    auto sle = std::make_shared<SLE>(sit, item.key());
    bool modified = false;
    
    // Check every possible uint256 field
    modified |= updateField(sle, sfPreviousTxnID, old_to_new);
    modified |= updateField(sle, sfIndexPrevious, old_to_new);
    modified |= updateField(sle, sfIndexNext, old_to_new);
    modified |= updateField(sle, sfBookNode, old_to_new);
    
    // Vector fields
    modified |= updateVector(sle, sfIndexes, old_to_new);
    modified |= updateVector(sle, sfHashes, old_to_new);
    modified |= updateVector(sle, sfAmendments, old_to_new);
    modified |= updateVector(sle, sfNFTokenOffers, old_to_new);
    
    if (modified) {
        // Re-serialize with updated references
        Serializer s;
        sle->add(s);
        // Create new item with new key
        item = make_shamapitem(old_to_new[item.key()], s.slice());
    }
});
```

### Complexity Analysis
- **Phase 1**: O(n) where n = number of objects (~620k)
- **Phase 2**: O(n × m) where m = average fields per object
- **Hash lookups**: O(1) average case
- **Total**: Linear in the number of objects!

### Memory Requirements
- LUT size: 620k entries × (32 bytes + 32 bytes) = ~40 MB
- Reasonable to keep in memory during migration

### Implementation Challenges

#### 1. Comprehensive Field Coverage
Must check EVERY field that could contain a key:
```cpp
// Singleton uint256 fields
sfPreviousTxnID, sfIndexPrevious, sfIndexNext, sfBookNode,
sfNFTokenID, sfEmitParentTxnID, sfHookOn, sfHookStateKey...

// Vector256 fields  
sfIndexes, sfHashes, sfAmendments, sfNFTokenOffers,
sfHookNamespaces, sfURITokenIDs...

// Nested structures
STArray fields containing STObjects with uint256 fields
```

#### 2. False Positive Risk
Any uint256 that happens to match a key would be updated:
- Could corrupt data if a non-key field matches
- Mitigation: Only update known reference fields
- Risk: Missing custom fields added by hooks

#### 3. Order Book Sorting
Order books are sorted by key value. After rekeying:
- Sort order changes completely
- Need to rebuild book directories
- Quality levels might shift

### Alternative: Persistent Migration Map

Instead of one-time migration, store the mapping permanently:

```cpp
// Special ledger entries (one per ~1000 mappings)
MigrationMap_0000: {
    sfOldKeys: [old_hash_0, old_hash_1, ...],
    sfNewKeys: [new_hash_0, new_hash_1, ...]
}
MigrationMap_0001: { ... }
// ~620 of these objects
```

Pros:
- Can verify historical references
- Debugging is easier
- Can be pruned later if needed

Cons:
- Permanent state bloat (~40MB)
- Must be loaded on every node forever
- Lookup overhead for historical operations

### The Nuclear Option: Binary Search-Replace

For maximum chaos (don't actually do this):
```cpp
// Build LUT
std::map<std::array<uint8_t, 32>, std::array<uint8_t, 32>> binary_lut;

// Scan serialized blobs and replace
for (auto& node : shamap) {
    auto data = node.getData();
    for (size_t i = 0; i <= data.size() - 32; i++) {
        if (binary_lut.count(data[i..i+31])) {
            memcpy(&data[i], binary_lut[data[i..i+31]], 32);
        }
    }
}
```

Why this is insane:
- False positives would corrupt data
- No validation of what you're replacing
- Breaks all checksums and signatures
- Impossible to debug when it goes wrong

## Conclusion

The rekeying problem is not just about changing hash functions - it's about maintaining referential integrity across millions of interlinked objects. Every key change cascades through the reference graph, making this one of the most complex migrations possible in a blockchain system.

The lookup table approach makes it algorithmically feasible (linear time rather than quadratic), but the implementation complexity and risk remain enormous. You need to:
1. Find every single field that could contain a key
2. Update them all correctly
3. Handle sorting changes in order books
4. Avoid false positives
5. Deal with custom fields from hooks
6. Maintain consistency across the entire state

This is likely why most blockchains never change their hash functions after genesis - even with an efficient algorithm, the complexity and risk are enormous.