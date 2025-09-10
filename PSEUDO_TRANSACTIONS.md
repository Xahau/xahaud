# Pseudo Transactions in Xahau/Ripple

## Overview

Pseudo transactions are special system-level transactions that are automatically applied by the network at specific ledger sequences. Unlike regular transactions that are submitted by users and go through the transaction queue, pseudo transactions are generated and applied deterministically by the protocol itself.

## Current Pseudo Transaction Types

Based on the codebase analysis from `PseudoTx_test.cpp`:

### 1. **Fee Pseudo Transaction (ttFEE)**
- Applied to set or update the base fee and reserve amounts
- Contains fields:
  - `sfLedgerSequence`: The ledger where this applies
  - Fee fields (depending on `featureXRPFees`):
    - Modern: `sfBaseFeeDrops`, `sfReserveBaseDrops`, `sfReserveIncrementDrops`
    - Legacy: `sfBaseFee`, `sfReserveBase`, `sfReserveIncrement`, `sfReferenceFeeUnits`

### 2. **Amendment Pseudo Transaction (ttAMENDMENT)**
- Applied to activate or deactivate protocol amendments
- Contains fields:
  - `sfAccount`: Set to AccountID() (null account)
  - `sfAmendment`: The amendment hash
  - `sfLedgerSequence`: The activation ledger

## How Pseudo Transactions Work

1. **Generation**: Created deterministically at specific ledger sequences
2. **Application**: Applied automatically during ledger closing
3. **Validation**: Cannot be submitted by users (blocked by `passesLocalChecks`)
4. **Consensus**: All validators generate identical pseudo transactions

## Using Pseudo Transactions for Hash Migration

### The Hash Migration Pseudo Transaction Proposal

Create a new pseudo transaction type `ttHASH_MIGRATION` that would:

```cpp
// Pseudo transaction structure
ttHASH_MIGRATION {
    sfAccount: AccountID(),  // null account
    sfLedgerSequence: HASH_MIGRATION_LEDGER,  // e.g., 20,000,000
    sfHashAlgorithm: "BLAKE3",
    sfPreviousAlgorithm: "SHA512_HALF",
    sfMigrationFlags: {
        REHASH_STATE_MAP: true,
        INVALIDATE_CACHES: true,
        CHECKPOINT_REQUIRED: true
    }
}
```

### Migration Strategy Using Pseudo Transactions

#### Phase 1: Pre-Migration (Before Ledger 20,000,000)
- All nodes use SHA-512 Half
- Code already contains Blake3 implementation
- Hash context classifiers are in place (as we've just implemented)

#### Phase 2: Migration Ledger (Ledger 20,000,000)
```
Ledger 19,999,999 closes with SHA-512 Half
↓
ttHASH_MIGRATION pseudo transaction triggers
↓
All state is rehashed with Blake3
↓
Ledger 20,000,000 opens with Blake3
```

#### Phase 3: Post-Migration (After Ledger 20,000,000)
- All new hashes use Blake3
- Historical data before migration still verifiable with SHA-512

### Implementation Details

#### 1. Add Hash Migration Transaction Type
```cpp
// In TxFormats.h
enum TxType : std::uint16_t {
    // ... existing types ...
    ttHASH_MIGRATION = 21,  // New pseudo transaction type
};
```

#### 2. Migration Pseudo Transaction Handler
```cpp
class HashMigration {
public:
    static TER
    apply(ApplyView& view, STTx const& tx, beast::Journal j) {
        // 1. Verify this is the correct migration ledger
        if (view.seq() != HASH_MIGRATION_LEDGER)
            return tefBAD_LEDGER;
        
        // 2. Trigger state map rehashing
        view.rawView().rehashStateMap(HashAlgorithm::BLAKE3);
        
        // 3. Invalidate all cached nodes
        view.rawView().invalidateHashCaches();
        
        // 4. Set global hash algorithm flag
        view.rawView().setHashAlgorithm(HashAlgorithm::BLAKE3);
        
        // 5. Create migration checkpoint
        createMigrationCheckpoint(view);
        
        return tesSUCCESS;
    }
};
```

#### 3. Hash Function Selection Based on Ledger
```cpp
// In digest.h/cpp
HashAlgorithm selectHashAlgorithm(hash_options const& opts) {
    if (!opts.ledger_index.has_value())
        return HashAlgorithm::SHA512_HALF;  // Default for non-ledger
    
    if (opts.ledger_index.value() >= HASH_MIGRATION_LEDGER) {
        // Post-migration ledgers use Blake3
        return HashAlgorithm::BLAKE3;
    } else {
        // Pre-migration ledgers use SHA-512 Half
        return HashAlgorithm::SHA512_HALF;
    }
}
```

### Advantages of Pseudo Transaction Approach

1. **Deterministic**: All nodes execute the same migration at the same ledger
2. **Atomic**: The entire state transitions in one ledger close
3. **Auditable**: The migration appears in the ledger history
4. **Reversible**: Could theoretically migrate back if needed
5. **Clean**: No ambiguity about which algorithm to use

### Challenges and Solutions

#### Challenge 1: Performance Impact
**Problem**: Rehashing the entire state map could take significant time.
**Solution**: Pre-compute Blake3 hashes in background before migration ledger.

#### Challenge 2: Network Synchronization
**Problem**: Nodes must stay in sync during migration.
**Solution**: Require supermajority agreement before migration proceeds.

#### Challenge 3: Historical Verification
**Problem**: Need to verify pre-migration data with old algorithm.
**Solution**: Use ledger sequence from hash_options to select correct algorithm.

### Alternative: Amendment-Based Migration

Instead of a dedicated pseudo transaction, use the existing amendment mechanism:

```cpp
// Activate hash migration via amendment
if (view.rules().enabled(featureBLAKE3Migration)) {
    if (view.seq() == firstLedgerWithFeature(featureBLAKE3Migration)) {
        // This is the transition ledger
        applyHashMigration(view);
    }
}
```

## Testing Strategy

1. **Unit Tests**: Verify pseudo transaction generation and application
2. **Integration Tests**: Test full migration on test networks
3. **Performance Tests**: Measure migration time for various state sizes
4. **Consensus Tests**: Ensure all nodes reach same post-migration state

## Rollout Plan

1. **Phase 1**: Deploy code with Blake3 support (dormant)
2. **Phase 2**: Activate on test networks
3. **Phase 3**: Set migration ledger far in future on mainnet
4. **Phase 4**: Monitor and prepare for migration
5. **Phase 5**: Migration occurs automatically at designated ledger

## Conclusion

Using pseudo transactions for hash migration provides a clean, deterministic, and auditable way to transition the entire network from SHA-512 Half to Blake3. The migration appears as a historical event in the ledger, maintaining the blockchain's integrity and auditability while modernizing its cryptographic foundation.