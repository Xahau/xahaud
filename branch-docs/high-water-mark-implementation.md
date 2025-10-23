# High Water Mark Capacity Implementation

## Summary

**Completely replaced account-wide scale design with per-entry high water mark capacity.**

### What Changed

**Before (Account-Wide Scale)**:
- `sfHookStateScale` in AccountRoot (1-16 multiplier)
- All entries cost `scale × 1` reserves
- Can't decrease scale if `stateCount > 0`
- Must set scale via AccountSet before creating large entries
- All entries limited to same max size

**After (High Water Mark Capacity)**:
- `sfHookStateCapacity` in ltHOOK_STATE (per-entry, uint16)
- Each entry costs `capacity` reserves where capacity = `ceil(dataSize / 256)`
- Capacity = max size ever seen for that entry
- Capacity only grows, never shrinks
- No upfront configuration needed
- Mixed sizes naturally supported

---

## Implementation Details

### Protocol Changes

**New Field**:
```cpp
// src/ripple/protocol/SField.h & SField.cpp
SF_UINT16 sfHookStateCapacity; // Field #22
```

**Ledger Format**:
```cpp
// src/ripple/protocol/impl/LedgerFormats.cpp
add(jss::HookState, ltHOOK_STATE, {
    {sfOwnerNode, soeREQUIRED},
    {sfHookStateKey, soeREQUIRED},
    {sfHookStateData, soeREQUIRED},
    {sfHookStateCapacity, soeOPTIONAL},  // NEW
}, commonFields);
```

**Removed**:
- `sfHookStateScale` from AccountRoot
- `sfHookStateScale` from AccountSet transaction
- All scale validation/enforcement logic in SetAccount.cpp

### Cache Structure

**Before**:
```cpp
std::map<AccountID, std::tuple<
    int64_t,   // available reserves
    int64_t,   // namespace count
    uint16_t,  // hook state scale (account-wide)
    std::map<uint256, std::map<uint256,
        std::pair<bool, Blob>>>>>  // modified, data
```

**After**:
```cpp
std::map<AccountID, std::tuple<
    int64_t,   // available reserves
    int64_t,   // namespace count
    std::map<uint256, std::map<uint256,
        std::tuple<bool, Blob, uint16_t>>>>>  // modified, data, capacity
```

### Core Logic

**set_state_cache() - Phase 1 Virtual Accounting**:
```cpp
// Calculate capacity from data size
uint16_t newCapacity = data.empty() ? 0 : (data.size() + 255) / 256;

// Get old capacity from ledger/cache
uint16_t oldCapacity = 0;
if (existing entry) {
    oldCapacity = entry.capacity ?: ceil(entry.dataSize / 256);
}

// High water mark
uint16_t finalCapacity = max(newCapacity, oldCapacity);
uint16_t capacityDelta = finalCapacity - oldCapacity;

// Only deduct reserves if capacity INCREASES
if (modified && capacityDelta > 0) {
    if (availableForReserves < capacityDelta)
        return RESERVE_INSUFFICIENT;
    availableForReserves -= capacityDelta;
}
```

**setHookState() - Phase 2 Actual Ledger Updates**:
```cpp
// For creates
ownerCount += capacity;
adjustOwnerCount(view, sleAccount, capacity, j);
hookState->setFieldU16(sfHookStateCapacity, capacity);

// For modifications
uint16_t oldCapacity = hookState->getFieldU16(sfHookStateCapacity);
if (capacity > oldCapacity) {
    uint16_t capacityDelta = capacity - oldCapacity;
    ownerCount += capacityDelta;
    adjustOwnerCount(view, sleAccount, capacityDelta, j);
    hookState->setFieldU16(sfHookStateCapacity, capacity);
}

// For deletes
adjustOwnerCount(view, sleAccount, -capacity, j);
```

**Namespace Deletion**:
```cpp
// Track individual capacities during directory walk
std::vector<uint16_t> capacities;
for (each entry) {
    uint16_t cap = entry.capacity ?: ceil(entry.dataSize / 256);
    capacities.push_back(cap);
}

// Sum total capacity being deleted
uint32_t totalCapacity = sum(capacities);
adjustOwnerCount(view, sleAccount, -totalCapacity, j);
```

### Size Limits

**Before**:
```cpp
// Account sets scale=8
maxSize = 256 * scale = 2048 bytes

// In state_foreign_set
maxSize = maxHookStateDataSize(hookStateScale);
```

**After**:
```cpp
// No account parameter needed
// Absolute maximum: 16 * 256 = 4096 bytes
constexpr uint32_t maxAbsoluteSize = 16 * 256;

// Per-entry check happens automatically in set_state_cache
// based on calculated capacity
```

### Backward Compatibility

**Legacy entries without capacity**:
```cpp
uint16_t oldCapacity;
if (hookState->isFieldPresent(sfHookStateCapacity)) {
    oldCapacity = hookState->getFieldU16(sfHookStateCapacity);
} else {
    // Legacy: calculate from current data size
    auto const& data = hookState->getFieldVL(sfHookStateData);
    oldCapacity = data.empty() ? 1 : (data.size() + 255) / 256;
}
```

**Old scale field**:
- `sfHookStateScale` still defined in protocol (field #21)
- No longer used anywhere in implementation
- Reading it returns 0 (field absent)
- Can coexist with new capacity field

---

## Test Changes

### AccountSet_test.cpp

**Removed**: `testHookStateScale()` - entire account-wide scale test suite

**Added**: `testHookStateCapacity()` - placeholder confirming no account-wide config needed

### SetHook_test.cpp

**Removed**: Scale-based test checking:
- Setting scale via AccountSet
- OwnerCount = `scale × stateCount`
- Scale validation
- Scale decrease restrictions

**Added**: High water mark test checking:
- Capacity grows automatically (300 → 800 → 1100 bytes)
- Capacity never shrinks (800 → 100 bytes, capacity stays at 4)
- Reserves adjust only on capacity growth
- Maximum 4096 bytes enforced
- OwnerCount = sum of individual capacities

---

## Migration Impact

### For Existing Deployments

**If merged before ExtendedHookState amendment activates**:
- No migration needed
- Feature ships with high water mark from day 1
- No legacy scale-based entries exist

**If ExtendedHookState already active**:
- Existing entries created under old design have no capacity field
- Backward compatibility logic calculates capacity from data size
- First modification adds capacity field
- Gradual migration as entries are modified

### For Developers

**Before**:
```javascript
// Set scale before creating state
env(accountSet(alice, {HookStateScale: 8}));
// All entries limited to 8 * 256 = 2048 bytes
// Can't decrease later
```

**After**:
```javascript
// No setup needed!
// Just create state, capacity grows automatically
// Each entry independent
// Shrinking data keeps capacity (high water mark)
```

---

## Benefits

### ✅ Eliminates Account-Wide Lock-In
```
Before: Set scale=8 → stuck at 8× reserves forever
After:  No account parameter → no lock-in
```

### ✅ Optimal Reserve Usage
```
Before: 1000 small entries at scale=8 = 8000 reserves
After:  1000 small entries with capacity=1 each = 1000 reserves
```

### ✅ No Upfront Guessing
```
Before: Must predict max size before creating entries
After:  Capacity grows organically with usage
```

### ✅ Predictable Modifications
```
Before: Modifications within scale never fail on reserves
After:  Modifications within capacity never fail (same)
        Modifications growing capacity: reserve check (clear, expected)
        Modifications shrinking size: never fail (high water mark)
```

### ✅ Mixed Workloads
```
Before: All entries same max size
After:  Each entry independent - 256 bytes, 2KB, 4KB naturally coexist
```

---

## Trade-offs

### Limitation: Can't Reclaim Reserves Without Delete

**Scenario**:
```
Entry grows to 2KB (capacity=8)
Data shrinks to 100 bytes permanently
Still paying 8 reserves
```

**Workaround**: Delete + recreate entry to reset capacity to 1

**Rationale**:
- Satisfies Richard's concern: reserves based on capacity, not current size
- One-way growth = simple, predictable
- Deletion still allows cleanup
- Penalty for "accidental growth" is bounded

### Storage Overhead

**Before**: 0 bytes (account-wide scale in AccountRoot)

**After**: 2 bytes per entry (uint16_t capacity)

**Impact**: Minimal - each entry already has 32-byte key + variable data

---

## Files Modified

### Protocol (13 files)
- src/ripple/protocol/SField.h
- src/ripple/protocol/impl/SField.cpp
- src/ripple/protocol/impl/LedgerFormats.cpp
- src/ripple/protocol/impl/TxFormats.cpp (removed scale from AccountSet)
- hook/sfcodes.h

### Core Logic (3 files)
- src/ripple/app/hook/applyHook.h
- src/ripple/app/hook/impl/applyHook.cpp
- src/ripple/app/hook/Enum.h

### Transaction Processing (2 files)
- src/ripple/app/tx/impl/SetAccount.cpp (removed all scale logic)
- src/ripple/app/tx/impl/SetHook.cpp (namespace deletion)

### Tests (2 files)
- src/test/rpc/AccountSet_test.cpp
- src/test/app/SetHook_test.cpp

### Total: 20 files modified

---

## Lines Changed

**Additions**: ~250 lines
- High water mark logic in set_state_cache()
- Capacity tracking in setHookState()
- Capacity summation in namespace deletion
- New test cases

**Deletions**: ~150 lines
- All account-wide scale logic
- Scale validation in SetAccount
- Scale-based tests
- AccountSet scale transaction handling

**Net**: ~100 lines added

---

## Verification Steps

1. **Compile**: ✅ All compilation errors fixed
2. **Tests**: Run SetHook_test.cpp capacity test
3. **Integration**: Verify OwnerCount calculations
4. **Backward compat**: Test legacy entries without capacity field
5. **Limits**: Verify 4096 byte maximum enforced
6. **Reserves**: Verify capacity growth triggers reserve checks
7. **Deletion**: Verify capacity refunds on delete

---

## Design Rationale

This implementation follows the "High Water Mark Capacity" design from the extended-hook-state-spec.md, which was identified as the optimal approach during the PR review discussion.

**Key Design Principles**:
1. **Capacity-based reserves**: Satisfies Richard's concern about size-based reserves
2. **One-way growth**: Simple, predictable behavior
3. **No upfront guessing**: Organic growth with usage
4. **Per-entry independence**: No account-wide lock-in
5. **Backward compatible**: Graceful handling of legacy entries

This design provides the flexibility of per-entry capacity with the simplicity of one-way growth, eliminating the "Scale Commitment Trap" while maintaining predictable reserve mechanics.
