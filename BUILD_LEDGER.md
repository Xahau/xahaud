# Hash Migration Implementation via BuildLedger

## Overview

This document outlines the approach for implementing SHA-512 Half to BLAKE3 hash migration by performing the state map rekeying operation in the ledger building process, bypassing the metadata generation problem inherent in the transaction processing pipeline.

## The Core Problem

When switching from SHA-512 Half to BLAKE3, every object in the state map needs to be rekeyed because the hash (which IS the key in the SHAMap) changes. This would generate metadata showing:
- Every object deleted at its old SHA-512 key
- Every object created at its new BLAKE3 key
- Total metadata size: 2× the entire state size (potentially gigabytes)

## The Solution: Bypass Transaction Processing

Instead of trying to rekey within the transaction processor (which tracks all changes for metadata), perform the rekeying AFTER transaction processing but BEFORE ledger finalization.

## Implementation Location

The key intervention point is in `buildLedgerImpl()` at line 63 of `BuildLedger.cpp`:

```cpp
// BuildLedger.cpp, lines 58-65
{
    OpenView accum(&*built);
    assert(!accum.open());
    applyTxs(accum, built);  // Apply transactions (including pseudo-txns)
    accum.apply(*built);      // Apply accumulated changes to the ledger
}
// <-- INTERVENTION POINT HERE
built->updateSkipList();
```

## Detailed Implementation

### 1. Pseudo-Transaction Role (Simple Flag Setting)

```cpp
// In Change.cpp
TER Change::applyHashMigration()
{
    // The pseudo-transaction just sets a flag
    // The actual migration happens in BuildLedger
    
    JLOG(j_.warn()) << "Hash migration pseudo transaction triggered at ledger "
                    << view().seq();
    
    // Create a migration flag object
    auto migrationFlag = std::make_shared<SLE>(
        keylet::hashMigrationFlag(
            hash_options{view().seq(), KEYLET_MIGRATION_FLAG}));
    
    migrationFlag->setFieldU32(sfLedgerSequence, view().seq());
    migrationFlag->setFieldU8(sfMigrationStatus, 1); // 1 = pending
    
    view().insert(migrationFlag);
    
    return tesSUCCESS;
}
```

### 2. BuildLedger Modification

```cpp
// In BuildLedger.cpp, after line 63
template <class ApplyTxs>
std::shared_ptr<Ledger>
buildLedgerImpl(
    std::shared_ptr<Ledger const> const& parent,
    NetClock::time_point closeTime,
    const bool closeTimeCorrect,
    NetClock::duration closeResolution,
    Application& app,
    beast::Journal j,
    ApplyTxs&& applyTxs)
{
    auto built = std::make_shared<Ledger>(*parent, closeTime);

    if (built->isFlagLedger() && built->rules().enabled(featureNegativeUNL))
    {
        built->updateNegativeUNL();
    }

    {
        OpenView accum(&*built);
        assert(!accum.open());
        applyTxs(accum, built);
        accum.apply(*built);
    }
    
    // NEW: Check for hash migration flag
    if (shouldPerformHashMigration(built, app, j))
    {
        performHashMigration(built, app, j);
    }

    built->updateSkipList();
    // ... rest of function
}

// New helper functions
bool shouldPerformHashMigration(
    std::shared_ptr<Ledger> const& ledger,
    Application& app,
    beast::Journal j)
{
    // Check if we're in the migration window
    constexpr LedgerIndex MIGRATION_START = 20'000'000;
    constexpr LedgerIndex MIGRATION_END = 20'000'010;
    
    if (ledger->seq() < MIGRATION_START || ledger->seq() >= MIGRATION_END)
        return false;
    
    // Check for migration flag
    auto const flag = ledger->read(keylet::hashMigrationFlag(
        hash_options{ledger->seq(), KEYLET_MIGRATION_FLAG}));
    
    if (!flag)
        return false;
    
    return flag->getFieldU8(sfMigrationStatus) == 1; // 1 = pending
}

void performHashMigration(
    std::shared_ptr<Ledger> const& ledger,
    Application& app,
    beast::Journal j)
{
    JLOG(j.warn()) << "PERFORMING HASH MIGRATION at ledger " << ledger->seq();
    
    auto& oldStateMap = ledger->stateMap();
    
    // Create new state map with BLAKE3 hashing
    SHAMap newStateMap(SHAMapType::STATE, ledger->family());
    newStateMap.setLedgerSeq(ledger->seq());
    
    // Track statistics
    std::size_t objectCount = 0;
    auto startTime = std::chrono::steady_clock::now();
    
    // Walk the entire state map and rekey everything
    oldStateMap.visitLeaves([&](SHAMapItem const& item) {
        try {
            // Deserialize the ledger entry
            SerialIter sit(item.slice());
            auto sle = std::make_shared<SLE>(sit, item.key());
            
            // The new key would be calculated with BLAKE3
            // For now, we'd need the actual BLAKE3 implementation
            // uint256 newKey = calculateBlake3Key(sle);
            
            // For this example, let's assume we have a function that 
            // computes the new key based on the SLE type and contents
            uint256 newKey = computeNewHashKey(sle, ledger->seq());
            
            // Re-serialize the SLE
            Serializer s;
            sle->add(s);
            
            // Add to new map with new key
            newStateMap.addGiveItem(
                SHAMapNodeType::tnACCOUNT_STATE,
                make_shamapitem(newKey, s.slice()));
            
            objectCount++;
            
            if (objectCount % 10000 == 0) {
                JLOG(j.info()) << "Migration progress: " << objectCount 
                              << " objects rekeyed";
            }
        }
        catch (std::exception const& e) {
            JLOG(j.error()) << "Failed to migrate object " << item.key()
                           << ": " << e.what();
            throw;
        }
    });
    
    auto endTime = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        endTime - startTime);
    
    JLOG(j.warn()) << "Hash migration completed: " << objectCount 
                   << " objects rekeyed in " << duration.count() << "ms";
    
    // Swap the state maps
    oldStateMap = std::move(newStateMap);
    
    // Update the migration flag to completed
    auto flag = ledger->peek(keylet::hashMigrationFlag(
        hash_options{ledger->seq(), KEYLET_MIGRATION_FLAG}));
    if (flag) {
        flag->setFieldU8(sfMigrationStatus, 2); // 2 = completed
        ledger->rawReplace(flag);
    }
}

uint256 computeNewHashKey(
    std::shared_ptr<SLE const> const& sle,
    LedgerIndex ledgerSeq)
{
    // This would use BLAKE3 instead of SHA-512 Half
    // Implementation depends on the BLAKE3 integration
    // For now, this is a placeholder
    
    // The actual implementation would:
    // 1. Determine the object type
    // 2. Extract the identifying fields
    // 3. Hash them with BLAKE3
    // 4. Return the new key
    
    return uint256(); // Placeholder
}
```

## Why This Approach Works

### 1. No Metadata Explosion
- The rekeying happens AFTER the `OpenView` is destroyed
- No change tracking occurs during the rekeying
- Only the migration flag generates metadata (minimal)

### 2. Direct SHAMap Access
- We have direct access to `built->stateMap()`
- Can manipulate the raw data structure without going through ApplyView
- Can create a new SHAMap and swap it in

### 3. Clean Separation of Concerns
- Pseudo-transaction: "Signal that migration should happen"
- BuildLedger: "Actually perform the migration"
- Transaction processor: Unchanged, doesn't need to handle massive rekeying

### 4. Timing is Perfect
- After all transactions are applied
- Before the ledger is finalized
- Before the skip list is updated
- Before the SHAMap is flushed to disk

## Files Referenced in This Analysis

### Core Implementation Files
- `src/ripple/app/ledger/impl/BuildLedger.cpp` - Main implementation location
- `src/ripple/app/ledger/BuildLedger.h` - Header for build functions
- `src/ripple/app/tx/impl/Change.cpp` - Pseudo-transaction handler
- `src/ripple/app/tx/impl/Change.h` - Change transactor header

### Transaction Processing Pipeline (analyzed but bypassed)
- `src/ripple/app/tx/impl/Transactor.cpp` - Base transaction processor
- `src/ripple/app/tx/impl/Transactor.h` - Transactor header
- `src/ripple/app/tx/impl/apply.cpp` - Transaction application
- `src/ripple/app/tx/impl/applySteps.cpp` - Transaction routing
- `src/ripple/app/tx/impl/ApplyContext.h` - Application context

### Ledger and View Classes
- `src/ripple/app/ledger/Ledger.h` - Ledger class definition
- `src/ripple/app/ledger/Ledger.cpp` - Ledger implementation
- `src/ripple/ledger/ApplyView.h` - View interface
- `src/ripple/ledger/ApplyViewImpl.h` - View implementation header
- `src/ripple/ledger/impl/ApplyViewImpl.cpp` - View implementation
- `src/ripple/ledger/impl/ApplyViewBase.cpp` - Base view implementation
- `src/ripple/ledger/detail/ApplyViewBase.h` - Base view header
- `src/ripple/ledger/OpenView.h` - Open ledger view
- `src/ripple/ledger/RawView.h` - Raw view interface

### SHAMap and Data Structures
- `src/ripple/shamap/SHAMap.h` - SHAMap class definition

### Metadata Generation
- `src/ripple/protocol/TxMeta.h` - Transaction metadata header
- `src/ripple/protocol/impl/TxMeta.cpp` - Metadata implementation

### Consensus and Pseudo-Transaction Injection
- `src/ripple/app/consensus/RCLConsensus.cpp` - Consensus implementation

### Supporting Documents
- `PSEUDO_TRANSACTIONS.md` - Documentation on pseudo-transactions
- `HASH_MIGRATION_CONTEXT.md` - Context for hash migration work

## Key Advantages

1. **Architecturally Clean**: Works within existing ledger building framework
2. **No Metadata Issues**: Completely bypasses the metadata generation problem
3. **Atomic Operation**: Either the entire state is rekeyed or none of it is
4. **Fail-Safe**: Can be wrapped in try-catch for error handling
5. **Observable**: Can log progress for large state maps
6. **Testable**: Can be tested independently of transaction processing

## Challenges and Considerations

1. **Performance**: Rekeying millions of objects will take time
   - Solution: This happens during consensus, all nodes do it simultaneously
   
2. **Memory Usage**: Need to hold both old and new SHAMaps temporarily
   - Solution: Could potentially do in-place updates with careful ordering
   
3. **Verification**: Need to ensure all nodes get the same result
   - Solution: Deterministic rekeying based on ledger sequence

4. **Rollback**: If migration fails, need to handle gracefully
   - Solution: Keep old map until new map is fully built and verified

## Conclusion

By performing the hash migration at the ledger building level rather than within the transaction processing pipeline, we can successfully rekey the entire state map without generating massive metadata. This approach leverages the existing architecture's separation between transaction processing and ledger construction, providing a clean and efficient solution to what initially appeared to be an intractable problem.

---

## APPENDIX: Revised Implementation Following Ledger Pattern

After reviewing the existing pattern in `BuildLedger.cpp`, it's clear that special ledger operations are implemented as methods on the `Ledger` class itself (e.g., `built->updateNegativeUNL()`). Following this pattern, the hash migration should be implemented as `Ledger::migrateToBlake3()`.

### Updated BuildLedger.cpp Implementation

```cpp
// In BuildLedger.cpp, following the existing pattern
template <class ApplyTxs>
std::shared_ptr<Ledger>
buildLedgerImpl(
    std::shared_ptr<Ledger const> const& parent,
    NetClock::time_point closeTime,
    const bool closeTimeCorrect,
    NetClock::duration closeResolution,
    Application& app,
    beast::Journal j,
    ApplyTxs&& applyTxs)
{
    auto built = std::make_shared<Ledger>(*parent, closeTime);

    if (built->isFlagLedger() && built->rules().enabled(featureNegativeUNL))
    {
        built->updateNegativeUNL();
    }

    {
        OpenView accum(&*built);
        assert(!accum.open());
        applyTxs(accum, built);
        accum.apply(*built);
    }
    
    // NEW: Check and perform hash migration following the pattern
    if (built->rules().enabled(featureBLAKE3Migration) && 
        built->shouldMigrateToBlake3())
    {
        built->migrateToBlake3();
    }

    built->updateSkipList();
    // ... rest of function
}
```

### Ledger.h Addition

```cpp
// In src/ripple/app/ledger/Ledger.h
class Ledger final : public std::enable_shared_from_this<Ledger>,
                     public DigestAwareReadView,
                     public TxsRawView,
                     public CountedObject<Ledger>
{
public:
    // ... existing methods ...

    /** Update the Negative UNL ledger component. */
    void
    updateNegativeUNL();

    /** Check if hash migration to BLAKE3 should be performed */
    bool
    shouldMigrateToBlake3() const;

    /** Perform hash migration from SHA-512 Half to BLAKE3
     *  This rekeys all objects in the state map with new BLAKE3 hashes.
     *  Must be called after transactions are applied but before the
     *  ledger is finalized.
     */
    void
    migrateToBlake3();

    // ... rest of class ...
};
```

### Ledger.cpp Implementation

```cpp
// In src/ripple/app/ledger/Ledger.cpp

bool
Ledger::shouldMigrateToBlake3() const
{
    // Check if we're in the migration window
    constexpr LedgerIndex MIGRATION_START = 20'000'000;
    constexpr LedgerIndex MIGRATION_END = 20'000'010;
    
    if (seq() < MIGRATION_START || seq() >= MIGRATION_END)
        return false;
    
    // Check for migration flag set by pseudo-transaction
    auto const flag = read(keylet::hashMigrationFlag(
        hash_options{seq(), KEYLET_MIGRATION_FLAG}));
    
    if (!flag)
        return false;
    
    return flag->getFieldU8(sfMigrationStatus) == 1; // 1 = pending
}

void
Ledger::migrateToBlake3()
{
    JLOG(j_.warn()) << "Performing BLAKE3 hash migration at ledger " << seq();
    
    // Create new state map with BLAKE3 hashing
    SHAMap newStateMap(SHAMapType::STATE, stateMap_.family());
    newStateMap.setLedgerSeq(seq());
    
    std::size_t objectCount = 0;
    auto startTime = std::chrono::steady_clock::now();
    
    // Walk the entire state map and rekey everything
    stateMap_.visitLeaves([&](SHAMapItem const& item) {
        // Deserialize the ledger entry
        SerialIter sit(item.slice());
        auto sle = std::make_shared<SLE>(sit, item.key());
        
        // Calculate new BLAKE3-based key
        // This would use the actual BLAKE3 implementation
        uint256 newKey = computeBlake3Key(sle);
        
        // Re-serialize and add to new map
        Serializer s;
        sle->add(s);
        
        newStateMap.addGiveItem(
            SHAMapNodeType::tnACCOUNT_STATE,
            make_shamapitem(newKey, s.slice()));
        
        if (++objectCount % 10000 == 0) {
            JLOG(j_.info()) << "Migration progress: " << objectCount 
                          << " objects rekeyed";
        }
    });
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);
    
    JLOG(j_.warn()) << "BLAKE3 migration completed: " << objectCount 
                   << " objects rekeyed in " << duration.count() << "ms";
    
    // Swap the state maps
    stateMap_ = std::move(newStateMap);
    
    // Update the migration flag to completed
    auto flag = peek(keylet::hashMigrationFlag(
        hash_options{seq(), KEYLET_MIGRATION_FLAG}));
    if (flag) {
        flag->setFieldU8(sfMigrationStatus, 2); // 2 = completed
        rawReplace(flag);
    }
}
```

This approach follows the established pattern in the codebase where special ledger operations are encapsulated as methods on the `Ledger` class itself, making the code more maintainable and consistent with the existing architecture.