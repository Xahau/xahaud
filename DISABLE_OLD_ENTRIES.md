# Auto-Disable Strategy for Hash Migration

## Core Concept
Instead of trying to fix entries with hardcoded old keys, **automatically disable them** during migration. If it contains old keys, it's broken anyway - make that explicit.

## The Algorithm

### Phase 1: Build Complete Old Key Set
```cpp
std::unordered_set<uint256> all_old_keys;

// Collect ALL SHA-512 keys from current state
stateMap_.visitLeaves([&](SHAMapItem const& item) {
    all_old_keys.insert(item.key());
    
    // Also collect keys from reference fields
    SerialIter sit(item.slice());
    auto sle = std::make_shared<SLE>(sit, item.key());
    
    // Collect from vector fields
    if (sle->isFieldPresent(sfIndexes)) {
        for (auto& key : sle->getFieldV256(sfIndexes)) {
            all_old_keys.insert(key);
        }
    }
    // ... check all other reference fields
});
```

### Phase 2: Scan and Disable

#### Hook Definitions (WASM Code)
```cpp
bool scanWASMForKeys(Blob const& wasm_code, std::unordered_set<uint256> const& keys) {
    // Scan for 32-byte sequences matching known keys
    for (size_t i = 0; i <= wasm_code.size() - 32; i++) {
        uint256 potential_key = extract32Bytes(wasm_code, i);
        if (keys.count(potential_key)) {
            return true;  // Found hardcoded key!
        }
    }
    return false;
}

// During migration
for (auto& hookDef : allHookDefinitions) {
    if (scanWASMForKeys(hookDef->getFieldBlob(sfCreateCode), all_old_keys)) {
        hookDef->setFieldU32(sfFlags, hookDef->getFlags() | HOOK_DISABLED_OLD_KEYS);
        disabled_hooks.push_back(hookDef->key());
    }
}
```

#### Hook State (Arbitrary Data)
```cpp
for (auto& hookState : allHookStates) {
    auto data = hookState->getFieldBlob(sfHookStateData);
    if (containsAnyKey(data, all_old_keys)) {
        hookState->setFieldU32(sfFlags, STATE_INVALID_OLD_KEYS);
        disabled_states.push_back(hookState->key());
    }
}
```

#### Other Vulnerable Entry Types
```cpp
void disableEntriesWithOldKeys(SLE& sle) {
    switch(sle.getType()) {
        case ltHOOK:
            if (hasOldKeys(sle)) {
                sle.setFlag(HOOK_DISABLED_MIGRATION);
            }
            break;
            
        case ltESCROW:
            // Check if destination/condition contains old keys
            if (containsOldKeyReferences(sle)) {
                sle.setFlag(ESCROW_FROZEN_MIGRATION);
            }
            break;
            
        case ltPAYCHAN:
            // Payment channels with old key references
            if (hasOldKeyInFields(sle)) {
                sle.setFlag(PAYCHAN_SUSPENDED_MIGRATION);
            }
            break;
            
        case ltHOOK_STATE:
            // Already handled above
            break;
    }
}
```

## Flag Definitions

```cpp
// New flags for migration-disabled entries
constexpr uint32_t HOOK_DISABLED_OLD_KEYS     = 0x00100000;
constexpr uint32_t STATE_INVALID_OLD_KEYS     = 0x00200000;
constexpr uint32_t ESCROW_FROZEN_MIGRATION    = 0x00400000;
constexpr uint32_t PAYCHAN_SUSPENDED_MIGRATION = 0x00800000;
constexpr uint32_t ENTRY_BROKEN_MIGRATION     = 0x01000000;
```

## Execution Prevention

```cpp
// In transaction processing
TER HookExecutor::executeHook(Hook const& hook) {
    if (hook.isFieldPresent(sfFlags)) {
        if (hook.getFlags() & HOOK_DISABLED_OLD_KEYS) {
            return tecHOOK_DISABLED_MIGRATION;
        }
    }
    // Normal execution
}

TER processEscrow(Escrow const& escrow) {
    if (escrow.getFlags() & ESCROW_FROZEN_MIGRATION) {
        return tecESCROW_FROZEN_MIGRATION;
    }
    // Normal processing
}
```

## Re-enabling Process

### For Hooks
Developer must submit a new SetHook transaction with updated WASM:
```cpp
TER SetHook::doApply() {
    // If hook was disabled for migration
    if (oldHook->getFlags() & HOOK_DISABLED_OLD_KEYS) {
        // Verify new WASM doesn't contain old keys
        if (scanWASMForKeys(newWasm, all_old_keys)) {
            return tecSTILL_CONTAINS_OLD_KEYS;
        }
        // Clear the disabled flag
        newHook->clearFlag(HOOK_DISABLED_OLD_KEYS);
    }
}
```

### For Hook State
Must be cleared and rebuilt:
```cpp
TER HookStateModify::doApply() {
    if (state->getFlags() & STATE_INVALID_OLD_KEYS) {
        // Can only delete, not modify
        if (operation != DELETE) {
            return tecSTATE_REQUIRES_REBUILD;
        }
    }
}
```

## Migration Report

```json
{
    "migration_ledger": 20000000,
    "entries_scanned": 620000,
    "entries_disabled": {
        "hooks": 12,
        "hook_definitions": 3,
        "hook_states": 1847,
        "escrows": 5,
        "payment_channels": 2
    },
    "disabled_by_reason": {
        "wasm_contains_keys": 3,
        "state_contains_keys": 1847,
        "reference_old_keys": 19
    },
    "action_required": [
        {
            "type": "HOOK_DEFINITION",
            "key": "0xABCD...",
            "owner": "rXXX...",
            "reason": "WASM contains 3 hardcoded SHA-512 keys",
            "action": "Recompile hook with new keys or remove hardcoding"
        }
    ]
}
```

## Benefits

1. **Safety**: Broken things explicitly disabled, not silently failing
2. **Transparency**: Clear record of what was disabled and why
3. **Natural Cleanup**: Abandoned entries stay disabled forever
4. **Developer Responsibility**: Owners must actively fix and re-enable
5. **No Silent Corruption**: Better to disable than corrupt
6. **Audit Trail**: Complete record of migration casualties

## Implementation Complexity

- **Scanning**: O(n×m) where n=entries, m=data size
- **Memory**: Need all old keys in memory (~40MB)
- **False Positives**: Extremely unlikely (2^-256 probability)
- **Recovery**: Clear path to re-enable fixed entries

## The Nuclear Option

If too many critical entries would be disabled:
```cpp
if (disabled_count > ACCEPTABLE_THRESHOLD) {
    // Abort migration
    return temMIGRATION_TOO_DESTRUCTIVE;
}
```

## Summary

Instead of attempting impossible fixes for hardcoded keys, acknowledge reality: **if it contains old keys, it's broken**. Make that brokenness explicit through disabling, forcing conscious action to repair and re-enable. This turns an impossible problem (fixing hardcoded keys in WASM) into a manageable one (identifying and disabling broken entries).