# Extended Hook State Specification

## Rationale

### Problem Statement

Hooks can store persistent state data on the ledger using HookState objects. Prior to this feature, each HookState entry was limited to exactly **256 bytes** of data.

This fixed size limitation creates problems for certain use cases:
- **Metadata storage**: NFT metadata, complex structured data
- **Batched operations**: Accumulating multiple small records before processing
- **Composite state**: Storing related data together rather than fragmenting across multiple entries

### Solution Approach

Rather than increase the fixed size (which would waste space for hooks that don't need it), this feature introduces **configurable capacity** via an account-level scale parameter.

**Key Design Principles:**
1. **Opt-in**: Accounts start at scale=1 (256 bytes, backward compatible)
2. **Account-wide**: All hook state entries for an account use the same size limit
3. **Capacity-based reserves**: Pay for maximum capacity, not actual usage
4. **Simple accounting**: Avoids per-entry size tracking and complex reserve adjustments

### Non-Goals

This feature does NOT:
- Track individual entry sizes (too complex)
- Allow per-entry or per-namespace scale settings (kept simple)
- Provide dynamic resizing of existing entries
- Enable scale decreases without data deletion (anti-spam, commitment)

### ⚠️ Critical Limitation: The Scale Commitment Trap

**You cannot decrease scale if you have any hook state entries.**

**Testing Scenario:**
```
Current state: scale=1, 1000 production hook state entries
Action: Set scale=8 to "test it out"
Result: Immediately pay 8× reserves (8000 vs 1000)
Escape: NONE - cannot decrease scale back to 1 without deleting all 1000 entries
```

**Implications:**
- Increasing scale is a **one-way door** unless you're willing to delete all state
- No "trial mode" or "test on a few entries" option
- Hooks control state lifecycle - you may not be able to delete state easily
- Third-party hooks can create state at your scale, locking you in

**Design Intent:**
This is deliberate anti-spam design - forces commitment. If you need extended state, prove it by locking up significant reserves. Don't experiment with scale changes on production accounts.

**Recommendation:**
Test scale changes on dedicated test accounts with no production state. Only increase scale on production accounts when you're certain you need it and can afford the permanent reserve increase.

---

## Reserve Mechanics

### The Reserve Formula

Hook state reserves are calculated as:

```
Hook State Reserve Contribution = HookStateScale × HookStateCount
```

Where:
- **HookStateScale**: Maximum size multiplier (1-16), stored in AccountRoot
- **HookStateCount**: Number of hook state entries, stored in AccountRoot
- **Total OwnerCount**: Includes hook state + trust lines + offers + escrows + NFT pages + etc.

Each account's total reserve requirement:
```
Required Reserve = Base Reserve + (OwnerCount × Incremental Reserve)
```

### Setting Scale via AccountSet Transaction

**Location**: `src/ripple/app/tx/impl/SetAccount.cpp:660-700`

When an AccountSet transaction changes `HookStateScale`:

```cpp
// Calculate the new total OwnerCount
newOwnerCount = oldOwnerCount - (oldScale × stateCount) + (newScale × stateCount)
              = oldOwnerCount + ((newScale - oldScale) × stateCount)
```

**Process:**
1. Read current scale (default 1 if not set)
2. Read current HookStateCount
3. Calculate new OwnerCount by removing old contribution and adding new contribution
4. Check if account balance meets new reserve requirement
5. If insufficient: `return tecINSUFFICIENT_RESERVE`
6. If sufficient: Call `adjustOwnerCount(view, sle, newOwnerCount - oldOwnerCount, j_)`
7. Store new scale in AccountRoot (or make absent if scale=1)

**Example - Increasing Scale:**
```
Initial state:
  - HookStateScale: 1 (or absent)
  - HookStateCount: 500
  - OwnerCount: 750 (500 from hook state, 250 from other objects)

User sets HookStateScale to 8:
  - newOwnerCount = 750 - (1 × 500) + (8 × 500)
  - newOwnerCount = 750 - 500 + 4000
  - newOwnerCount = 4250
  - Delta: +3500 reserves

Reserve check:
  - If balance < accountReserve(4250): FAIL with tecINSUFFICIENT_RESERVE
  - If balance >= accountReserve(4250): SUCCESS, reserves locked immediately
```

**Example - Decreasing Scale (Blocked):**
```
Attempt to decrease scale from 8 to 4 with HookStateCount > 0:
  - Blocked at preclaim (line 270): return tecHAS_HOOK_STATE
  - Transaction fails before any changes
  - Must delete all hook state first
```

### Creating Hook State Entries

**Location**: `src/ripple/app/hook/impl/applyHook.cpp:1150-1171`

When a hook creates new state via `state_set()`:

**Process:**
1. Increment HookStateCount: `++stateCount`
2. Check if new state count exceeds old count (consumed available allotment)
3. If exceeded: Add hookStateScale reserves
   ```cpp
   ownerCount += hookStateScale
   newReserve = accountReserve(ownerCount)
   if (balance < newReserve) return tecINSUFFICIENT_RESERVE
   adjustOwnerCount(view, sleAccount, hookStateScale, j)
   ```
4. Update HookStateCount in AccountRoot
5. Create the ltHOOK_STATE object

**Example:**
```
Before:
  - HookStateScale: 8
  - HookStateCount: 100
  - OwnerCount: 1050 (800 from hook state, 250 from other)

Hook creates new state entry:
  - HookStateCount: 100 → 101
  - OwnerCount: 1050 → 1058 (add hookStateScale=8)
  - Reserve check: balance must cover accountReserve(1058)
  - If check passes: Create ltHOOK_STATE, lock 8 more reserves
```

### Deleting Hook State Entries

**Location**: `src/ripple/app/hook/impl/applyHook.cpp:1115-1127`

When a hook deletes state via `state_set()` with empty data:

**Process:**
1. Decrement HookStateCount: `--stateCount`
2. Refund hookStateScale reserves:
   ```cpp
   adjustOwnerCount(view, sleAccount, -hookStateScale, j)
   ```
3. Update HookStateCount in AccountRoot (make absent if zero)
4. Delete the ltHOOK_STATE object

**Example:**
```
Before:
  - HookStateScale: 8
  - HookStateCount: 101
  - OwnerCount: 1058

Hook deletes state entry:
  - HookStateCount: 101 → 100
  - OwnerCount: 1058 → 1050 (subtract hookStateScale=8)
  - 8 reserves immediately available for other uses
  - ltHOOK_STATE removed from ledger
```

### Reserve Check Locations

**IMPORTANT:** Reserve checks only happen at specific points, not on every operation.

#### ✓ Reserve Checks Happen Here:

**1. Setting Scale (SetAccount transaction)**
- Location: `SetAccount.cpp:691`
- Check: `if (balance < reserve) return tecINSUFFICIENT_RESERVE`
- Checks NEW total reserve requirement after scale change

**2. Creating Hook State Entry**
- Location: `applyHook.cpp:1163`
- Check: `if (balance < newReserve) return tecINSUFFICIENT_RESERVE`
- Checks if account can afford `hookStateScale` more reserves

#### ✗ Reserve Checks DO NOT Happen Here:

**3. Modifying Hook State Entry**
- Location: `applyHook.cpp:1177`
- Code: `hookState->setFieldVL(sfHookStateData, data)`
- **NO RESERVE CHECK** - only size limit check:
  ```cpp
  if (data.size() > maxHookStateDataSize(hookStateScale))
      return temHOOK_DATA_TOO_LARGE
  ```
- Rationale: Reserves already paid at creation. Can modify within capacity freely.

**4. Deleting Hook State Entry**
- Location: `applyHook.cpp:1122`
- Code: `adjustOwnerCount(view, sleAccount, -hookStateScale, j)`
- **NO RESERVE CHECK** - unconditional refund
- Rationale: Deletion always reduces reserves (improves account health)

#### Why This Matters

**Predictable Modifications:**
Once a hook state entry exists, modifications never fail due to reserves (only size limits). This allows hooks to update state reliably without checking account balance on every write.

**Creation is the Gate:**
The reserve check at creation is the anti-spam mechanism. If you can afford to create the entry, you own that capacity until deletion.

**Design Philosophy:**
This follows Richard's principle: "I really dislike the idea of a variable length field claiming different reserves depending on its current size." Reserves are based on **capacity** (scale), not **current content size**.

### Key Observations

**Immediate Effect:**
- Scale changes affect reserves **instantly** for all existing entries
- No gradual migration or per-entry adjustment

**Refundable:**
- All hook state reserves are refundable upon deletion
- Not a fee, just locked capital (anti-spam via capital requirements)

**Shared Counter:**
- OwnerCount is a composite across all object types
- Hook state contribution is calculated as `scale × count`
- Relies on HookStateCount accuracy (cached in AccountRoot)

**No Partial Escapes:**
- Can't selectively reduce scale for some entries
- Can't migrate entries between scales
- All-or-nothing: delete everything or stay at current scale

---

## Hook State Creation Flow

### Overview

Hook state entries are created through Hook API functions callable from WebAssembly. The system uses a **two-phase commit** approach: virtual reserve checking during execution, then actual ledger updates after the hook finishes.

### Complete Flow Diagram

```
┌─────────────────────────────────────┐
│ Hook WASM Code                      │
│   state_set(data, key)              │  Hook calls API
│   state_foreign_set(...)            │
└──────────────┬──────────────────────┘
               │
               ↓
┌─────────────────────────────────────┐
│ Hook API Layer (WASM → C++)         │
│   DEFINE_HOOK_FUNCTION              │  Exposed via macro
│   - state_set()           (line 1622)│
│   - state_foreign_set()   (line 1651)│  Validates params, checks grants
└──────────────┬──────────────────────┘
               │
               ↓
┌─────────────────────────────────────┐
│ Cache Layer (Virtual Accounting)    │
│   set_state_cache()       (line 1470)│  ✓ RESERVE CHECK (virtual)
│   → stateMap[acc][ns][key] = data   │  In-memory only, no ledger changes
│   → availableReserves -= scale      │  Tracks available capacity
└──────────────┬──────────────────────┘
               │
               │ (Hook finishes execution)
               │
               ↓
┌─────────────────────────────────────┐
│ Commit Layer (Actual Ledger)        │
│   finalizeHookState()     (line 1838)│  Iterate all cached changes
│   → setHookState()        (line 1062)│  ✓ RESERVE CHECK (actual)
│     → adjustOwnerCount()             │  Modify AccountRoot.OwnerCount
│     → create/update ltHOOK_STATE     │  Create ledger objects
└─────────────────────────────────────┘
```

### Phase 1: During Hook Execution (Virtual)

**Entry Point:** Hook calls `state_set()` or `state_foreign_set()` from WASM

**What Happens:**

1. **API Layer** (`state_foreign_set` at applyHook.cpp:1651)
   - Validates parameters (bounds checking, size limits)
   - For foreign state: checks HookGrants for authorization
   - Checks size against `maxHookStateDataSize(hookStateScale)`
   - Calls cache layer

2. **Cache Layer** (`set_state_cache` at applyHook.cpp:1470)
   - **First time seeing this account:**
     ```cpp
     availableForReserves = (balance - currentReserve) / incrementalReserve
     if (availableForReserves < hookStateScale && modified)
         return RESERVE_INSUFFICIENT;
     ```
   - **Subsequent entries:**
     ```cpp
     canReserveNew = availableForReserves >= hookStateScale
     if (!canReserveNew && modified)
         return RESERVE_INSUFFICIENT;
     availableForReserves -= hookStateScale;  // Decrement virtual counter
     ```
   - Stores change in memory: `stateMap[acc][ns][key] = {modified, data}`
   - **No ledger changes yet** - purely in-memory accounting

3. **Result:**
   - Hook continues executing if reserve check passed
   - Hook aborts with error code if insufficient reserves
   - All state changes stay in cache (stateMap)

### Phase 2: After Hook Finishes (Actual)

**Entry Point:** Transaction applies hook result to ledger

**What Happens:**

1. **Finalization** (`finalizeHookState` at applyHook.cpp:1838)
   ```cpp
   for (const auto& accEntry : stateMap) {
       for (const auto& nsEntry : ...) {
           for (const auto& cacheEntry : ...) {
               if (is_modified) {
                   setHookState(applyCtx, acc, ns, key, data);
               }
           }
       }
   }
   ```

2. **Actual Ledger Update** (`setHookState` at applyHook.cpp:1062)
   - For **creates**:
     ```cpp
     ++stateCount;
     ownerCount += hookStateScale;
     newReserve = accountReserve(ownerCount);

     // Safety check (should never fail if Phase 1 worked correctly)
     if (balance < newReserve)
         return tecINSUFFICIENT_RESERVE;

     adjustOwnerCount(view, sleAccount, hookStateScale, j);
     // Actually create ltHOOK_STATE object
     view.insert(hookState);
     ```

   - For **modifications**:
     ```cpp
     hookState->setFieldVL(sfHookStateData, data);
     // NO RESERVE CHECK - already paid at creation
     ```

   - For **deletes**:
     ```cpp
     --stateCount;
     adjustOwnerCount(view, sleAccount, -hookStateScale, j);
     view.erase(hookState);
     // NO RESERVE CHECK - unconditional refund
     ```

3. **Result:**
   - AccountRoot.OwnerCount updated
   - AccountRoot.HookStateCount updated
   - ltHOOK_STATE objects created/modified/deleted
   - Changes committed to ledger

### Why Two Phases?

**Fail Fast:**
- Virtual check in Phase 1 aborts hook execution immediately if reserves insufficient
- Hook doesn't waste computation if it can't afford the state changes

**Safety Net:**
- Actual check in Phase 2 catches any accounting bugs
- Comment at line 1882: `"should not fail... checks were done before map insert"`

**Efficiency:**
- Multiple state changes checked once (virtual accounting) during execution
- Single ledger update pass after hook finishes
- No repeated ledger reads during hook execution

### Key Functions Summary

| Function | Location | Purpose | Reserve Check |
|----------|----------|---------|---------------|
| `state_set()` | applyHook.cpp:1622 | Hook API for local state | Via cache |
| `state_foreign_set()` | applyHook.cpp:1651 | Hook API for foreign state | Via cache |
| `set_state_cache()` | applyHook.cpp:1470 | Virtual accounting layer | ✓ Virtual check |
| `finalizeHookState()` | applyHook.cpp:1838 | Iterate cached changes | N/A (coordinator) |
| `setHookState()` | applyHook.cpp:1062 | Actual ledger updates | ✓ Actual check (creates only) |

---

## Alternative Design: Per-Entry Capacity

### Overview

Instead of an account-wide scale parameter, each HookState entry could store its own capacity determined at creation time.

### How It Would Work

**Creation:**
```cpp
state_set(key, 300 byte data):
  capacity = ceil(300 / 256) = 2  // Round up to nearest 256-byte increment
  reserves_needed = capacity

  Check: if (balance < accountReserve(ownerCount + capacity))
           return tecINSUFFICIENT_RESERVE

  Store in ltHOOK_STATE:
    - sfHookStateData: <300 bytes>
    - sfHookStateCapacity: 2  // NEW FIELD

  Lock 2 reserves
  max_size = 512 bytes forever
```

**Modification:**
```cpp
state_set(key, 400 byte data):
  max_allowed = entry.capacity × 256 = 512 bytes

  if (data.size() > max_allowed)
    return temHOOK_DATA_TOO_LARGE

  // NO RESERVE CHECK - already paid at creation
  hookState->setFieldVL(sfHookStateData, data)
```

**Deletion:**
```cpp
state_set(key, empty):
  adjustOwnerCount(-entry.capacity)  // Refund 2 reserves
  delete ltHOOK_STATE
```

### Advantages Over Current Design

**1. No Account-Wide Footgun**
```
Current: Set scale=8 → hooks create state → stuck at 8× reserves forever
Per-entry: Each entry independent → no cross-contamination
```

**2. Fine-Grained Pricing**
```
Current: All entries cost scale × 1 reserve (regardless of actual size)
Per-entry: 300 bytes costs 2 reserves, 1000 bytes costs 4 reserves
```

**3. Mixed Use Cases**
```
Current: All entries limited by single scale parameter
Per-entry: Some entries 256 bytes, some 2KB, some 4KB - naturally
```

**4. No Scale Change Restrictions**
```
Current: Cannot change scale without deleting all state
Per-entry: No "scale" to change - each entry has its own capacity
```

### Still Satisfies Richard's Concerns

From PR discussion: "I really dislike the idea of a variable length field claiming different reserves depending on its current size."

**Per-entry capacity satisfies this:**
- ✓ Reserves based on **capacity** (set at creation), not **current content size**
- ✓ Modifications never change reserves (only check size limits)
- ✓ No modification-time reserve checks
- ✓ Predictable: pay once at creation, modify freely within capacity

### Implementation Cost

**Additional field in ltHOOK_STATE:**
```cpp
{sfHookStateCapacity, soeREQUIRED},  // uint8 or uint16
```

**OwnerCount accounting:**
```
Current: OwnerCount += scale × count (simple multiplication)
Per-entry: OwnerCount += sum(entry.capacity for each entry)
```

Requires tracking individual capacities, but HookStateCount still works for counting entries.

### Why Current Design Was Chosen

Likely reasons:
1. **Simplicity** - account-wide parameter easier than per-entry field
2. **Storage** - one field in AccountRoot vs field in every ltHOOK_STATE
3. **Accounting** - simple `scale × count` calculation
4. **Conservative** - didn't want to add fields to ledger objects

Trade-off: Chose simplicity over flexibility, accepting the footgun as "user must be careful."

### Implementation with Two-Phase Commit

The discovered two-phase commit architecture makes per-entry capacity **easier to implement** than initially thought.

**Phase 1 Changes (Virtual - in set_state_cache):**

Current:
```cpp
// Account-wide scale
hookStateScale = sleAccount->getFieldU16(sfHookStateScale) ?: 1;
if (availableForReserves < hookStateScale && modified)
    return RESERVE_INSUFFICIENT;
availableForReserves -= hookStateScale;

stateMap[acc] = {availableReserves, namespaceCount, hookStateScale, {{ns, {{key, {modified, data}}}}}};
```

Per-Entry Capacity:
```cpp
// Calculate capacity from actual data size at creation
capacity = ceil(data.size() / 256);  // e.g., 300 bytes → capacity=2
if (availableForReserves < capacity && modified)
    return RESERVE_INSUFFICIENT;
availableForReserves -= capacity;

// Store capacity with the cached entry
stateMap[acc] = {availableReserves, namespaceCount, {{ns, {{key, {modified, data, capacity}}}}}};
```

**Phase 2 Changes (Actual - in setHookState):**

Current:
```cpp
ownerCount += hookStateScale;  // Use account-wide scale
adjustOwnerCount(view, sleAccount, hookStateScale, j);
```

Per-Entry Capacity:
```cpp
ownerCount += entry.capacity;  // Use per-entry capacity from cache
adjustOwnerCount(view, sleAccount, entry.capacity, j);
hookState->setFieldU8(sfHookStateCapacity, entry.capacity);  // Store in ledger
```

**Key Insights:**

1. **Capacity determined once:** At Phase 1 creation based on actual data size
2. **Cached with entry:** Flows naturally through stateMap cache to Phase 2
3. **No account-wide parameter:** Each entry independent
4. **Virtual accounting unchanged:** Still just decrementing available reserves
5. **OwnerCount naturally sums:** Each adjustOwnerCount call adds entry.capacity

**Why This Is Actually Simpler:**

Current design:
- Must read `hookStateScale` from AccountRoot in Phase 1
- Must use same scale for all entries (account-wide constraint)
- Phase 2 uses cached scale for all entries from same account

Per-entry design:
- Calculate capacity directly from `data.size()` in Phase 1
- No account-wide constraint to check/enforce
- Phase 2 uses cached capacity from each specific entry

The two-phase architecture was **designed** for this kind of per-entry logic - cache computed values in Phase 1, use them in Phase 2!

### Migration Path

If desired, could add per-entry capacity as a new feature:
1. Add `sfHookStateCapacity` field to ltHOOK_STATE
2. Make `sfHookStateScale` optional/deprecated
3. Modify cache structure: `stateMap[acc][ns][key] = {modified, data, capacity}`
4. Phase 1: calculate `capacity = ceil(data.size() / 256)` at creation
5. Phase 2: use `entry.capacity` instead of `hookStateScale`
6. Old entries: assume capacity = 1 if field absent (backward compatible)
7. OwnerCount accounting: automatically sums via individual `adjustOwnerCount` calls

**Estimated Implementation Complexity:** Moderate. Most changes localized to:
- `set_state_cache()` - add capacity calculation and cache field
- `setHookState()` - use cached capacity instead of account scale
- `ltHOOK_STATE` ledger format - add capacity field

No changes needed to Hook API surface, transaction validation, or reserve checking logic.

---

## Alternative Design: Scale Reduction via Directory Walk

### Overview

Instead of blanket blocking scale reductions when HookStateCount > 0, allow reductions if all existing entries actually fit within the new size limit. Validate by walking owner directories during transaction preclaim/doApply.

### Current Problem

**Location:** `SetAccount.cpp:270-275`

```cpp
if (stateCount > 0 && newScale < currentScale)
{
    JLOG(ctx.j.trace())
        << "Cannot decrease HookStateScale if state count is not zero.";
    return tecHAS_HOOK_STATE;
}
```

**Issue:** Blocks ALL scale reductions, even if actual data is small.

**Example:**
```
State: scale=8 (2048 bytes), 1000 entries
Actual sizes: All entries < 300 bytes
Want: scale=2 (512 bytes) - would save 6000 reserves!
Result: BLOCKED - must delete all 1000 entries first
```

### Proposed Enhancement

**Walk directories to validate actual sizes:**

```cpp
if (stateCount > 0 && newScale < currentScale)
{
    uint32_t const maxAllowedSize = 256 * newScale;
    uint32_t tooBigCount = 0;
    std::vector<uint256> tooBigKeys;  // For error reporting

    // Walk ALL HookState entries via owner directories
    // Iterate through HookNamespaces
    if (sleAccount->isFieldPresent(sfHookNamespaces))
    {
        auto const& namespaces = sleAccount->getFieldV256(sfHookNamespaces);
        for (auto const& ns : namespaces)
        {
            auto const dirKeylet = keylet::hookStateDir(account, ns);
            auto const dir = view.read(dirKeylet);
            if (!dir)
                continue;

            // Walk directory entries
            for (auto const& itemKey : dir->getFieldV256(sfIndexes))
            {
                auto const hookState = view.read({ltHOOK_STATE, itemKey});
                if (!hookState)
                    continue;

                auto const& data = hookState->getFieldVL(sfHookStateData);
                if (data.size() > maxAllowedSize)
                {
                    tooBigCount++;
                    if (tooBigKeys.size() < 10)  // Limit error details
                        tooBigKeys.push_back(hookState->getFieldH256(sfHookStateKey));
                }
            }
        }
    }

    if (tooBigCount > 0)
    {
        JLOG(ctx.j.trace())
            << "Cannot decrease HookStateScale: " << tooBigCount
            << " entries exceed new size limit of " << maxAllowedSize << " bytes";
        return tecHOOK_STATE_TOO_LARGE;  // New error code
    }

    // All entries fit! Proceed with scale reduction
}
```

### Fee Implications

**Cost of directory walk:**
- Must read every ltHOOK_STATE entry to check size
- Proportional to HookStateCount
- Expensive for accounts with many entries

**Fee Structure Options:**

**Option 1: Fixed premium**
```cpp
if (stateCount > 0 && newScale < currentScale)
{
    // Add flat fee for validation work
    fee += XRPAmount{stateCount * 10};  // 10 drops per entry
    // ... then validate
}
```

**Option 2: Dynamic based on work**
```cpp
// Charge per directory page read + per entry validated
fee += (dirPagesRead * 100) + (entriesValidated * 10);
```

**Option 3: Require explicit opt-in**
```cpp
// New optional field in AccountSet
if (tx.isFieldPresent(sfValidateHookStateReduction) &&
    tx.getFieldU8(sfValidateHookStateReduction) == 1)
{
    // Willing to pay for validation
    // ... walk and validate
}
else if (stateCount > 0 && newScale < currentScale)
{
    // Default: block as before
    return tecHAS_HOOK_STATE;
}
```

### Advantages

**1. Eliminates Primary Footgun**
```
Before: scale=8 with 1000 small entries → stuck forever
After:  scale=8 with 1000 small entries → can reduce to scale=2, free 6000 reserves
```

**2. Predictable Failure**
```
Error: "Cannot decrease HookStateScale: 50 entries exceed 512 byte limit"
User knows: Must delete those 50 entries (not all 1000)
```

**3. Incentivizes Cleanup**
```
User creates entries, most shrink over time
Can gradually reduce scale as data compacts
Rewards good data hygiene with reserve refunds
```

**4. Pay for What You Use**
```
Want to reduce scale? Pay fee proportional to validation work
Don't want to pay? Keep current scale (no harm)
```

### Disadvantages

**1. Expensive for Large State**
```
1000 entries × directory walk = expensive transaction
May be cheaper to just keep high scale
```

**2. Potential Griefing**
```
Attacker: Install hook that creates 10,000 small entries at scale=8
Victim:   Wants to reduce scale → must pay huge fee to validate
Alternative: Attacker must pay for 10,000×8 reserves (self-limiting)
```

**3. Transaction Complexity**
```
Simple check (stateCount > 0) → instant
Directory walk → reads 1000+ ledger objects
Longer transaction time, more validation complexity
```

**4. Still Doesn't Solve Per-Entry Mismatch**
```
1000 small entries, 1 large entry at 1500 bytes
Want scale=2 (512 bytes)? BLOCKED by 1 entry
Must delete that 1 entry (better than 1000, but still manual)
```

### Implementation Phases

**Phase 1: Simple validation (proposed above)**
- Walk all entries in preclaim/doApply
- Check each against new size limit
- Fail with specific error if any too large

**Phase 2: Optimization**
- Cache directory reads
- Early exit on first too-large entry (if only need boolean)
- Batch reads for efficiency

**Phase 3: Enhanced reporting**
- Return list of keys that exceed limit
- RPC endpoint to preview scale reduction impact
- Pre-check without transaction: "Would reducing to scale=2 work?"

### Comparison with Per-Entry Capacity

| Feature | Scale Reduction Walk | Per-Entry Capacity |
|---------|---------------------|-------------------|
| Account-wide lock-in | ✓ Eliminated | ✓ Eliminated |
| Per-entry lock-in | ✗ Still exists (just better) | ✗ Still exists |
| Implementation cost | Moderate (validation logic) | Moderate (cache + ledger field) |
| Transaction cost | High (walk directories) | None (no validation needed) |
| Mixed sizes | Must delete largest | Naturally supported |
| Storage overhead | None | +1 field per ltHOOK_STATE |

### Recommendation

**Combining both approaches:**

1. **Short term:** Add scale reduction validation via directory walk
   - Fixes immediate footgun
   - Works with current design
   - Opt-in via flag to avoid surprise fees

2. **Long term:** Consider per-entry capacity
   - Better long-term solution
   - Requires amendment
   - Can migrate gradually

This makes the current implementation more user-friendly while keeping the door open for a better design later.

---

## Alternative Design: High Water Mark Capacity (One-Way Growth)

### Overview

Instead of fixed capacity at creation, track the **maximum size ever seen** for each HookState entry. Capacity can only grow (never shrink), reserves adjust automatically as data grows, no reserve checks on shrinking modifications.

### Core Concept

```
HookState.Capacity = max(all historical data sizes)
Reserves = Capacity (in 256-byte increments)
Capacity only increases, never decreases
```

**Example lifecycle:**
```
Creation: state_set(key, 300 bytes)
  → capacity = 2 (ceil(300/256))
  → reserves = 2

Growth: state_set(key, 800 bytes)
  → oldCapacity = 2, newCapacity = 4
  → reserves += 2 (delta check only)
  → capacity = 4 (stored)

Shrink: state_set(key, 200 bytes)
  → capacity = 4 (unchanged - high water mark)
  → reserves = 4 (no change)
  → NO reserve check

Re-grow: state_set(key, 700 bytes)
  → capacity = 4 (unchanged - still within high water mark)
  → reserves = 4 (no change)
  → NO reserve check

Exceed: state_set(key, 1100 bytes)
  → oldCapacity = 4, newCapacity = 5
  → reserves += 1
  → capacity = 5
```

### Implementation

**Phase 1: During Hook Execution (Virtual)**

```cpp
// set_state_cache() modifications
int64_t set_state_cache(
    hook::HookContext& hookCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key,
    ripple::Blob& data,
    bool modified)
{
    uint32_t newCapacity = (data.size() + 255) / 256;  // ceil(size / 256)

    // Check if entry exists in cache or ledger
    auto existingEntry = lookup_state_cache(hookCtx, acc, ns, key);
    uint32_t oldCapacity = 0;

    if (!existingEntry) {
        // Check ledger
        auto hsSLE = view.peek(keylet::hookState(acc, key, ns));
        if (hsSLE) {
            oldCapacity = hsSLE->isFieldPresent(sfHookStateCapacity)
                ? hsSLE->getFieldU8(sfHookStateCapacity)
                : 1;  // Legacy entries default to 1
        }
    } else {
        oldCapacity = existingEntry->capacity;
    }

    // Only check reserves if capacity INCREASES
    if (newCapacity > oldCapacity && modified) {
        uint32_t delta = newCapacity - oldCapacity;
        if (availableForReserves < delta)
            return RESERVE_INSUFFICIENT;
        availableForReserves -= delta;
    }

    // Store max capacity seen
    uint32_t finalCapacity = std::max(newCapacity, oldCapacity);

    // Cache entry with capacity
    stateMap[acc][ns][key] = {modified, data, finalCapacity};

    return 1;
}
```

**Phase 2: After Hook Finishes (Actual)**

```cpp
// setHookState() modifications
TER hook::setHookState(
    ripple::ApplyContext& applyCtx,
    ripple::AccountID const& acc,
    ripple::uint256 const& ns,
    ripple::uint256 const& key,
    ripple::Slice const& data,
    uint32_t capacity)  // NEW: passed from cache
{
    auto hookState = view.peek(hookStateKeylet);
    bool createNew = !hookState;

    if (createNew) {
        // Creating new entry
        ownerCount += capacity;
        if (balance < accountReserve(ownerCount))
            return tecINSUFFICIENT_RESERVE;

        adjustOwnerCount(view, sleAccount, capacity, j);
        hookState = std::make_shared<SLE>(hookStateKeylet);
        hookState->setFieldU8(sfHookStateCapacity, capacity);
    }
    else {
        // Modifying existing entry
        uint32_t oldCapacity = hookState->getFieldU8(sfHookStateCapacity);

        if (capacity > oldCapacity) {
            // Capacity grew - adjust reserves
            uint32_t delta = capacity - oldCapacity;
            ownerCount += delta;

            if (balance < accountReserve(ownerCount))
                return tecINSUFFICIENT_RESERVE;

            adjustOwnerCount(view, sleAccount, delta, j);
            hookState->setFieldU8(sfHookStateCapacity, capacity);
        }
        // If capacity <= oldCapacity: no reserve change
    }

    hookState->setFieldVL(sfHookStateData, data);
    // ... rest of creation logic
}
```

### Advantages

**1. No Upfront Capacity Guessing**
```
Hook doesn't need to know max size at creation
Data grows organically as needed
Reserves adjust automatically
```

**2. One-Way = Predictable**
```
Modifications that shrink: NEVER fail on reserves
Modifications that stay same size: NEVER fail on reserves
Modifications that exceed high water mark: Reserve check (clear, expected)
```

**3. Satisfies Richard's Concern**
```
"I really dislike the idea of a variable length field claiming different
reserves depending on its current size"

With high water mark:
  Current size: 200 bytes
  Capacity: 4 (high water mark from previous 800 bytes)
  Reserves: Based on CAPACITY (4), not current size (1)
  ✓ Reserves don't change when current size changes
```

**4. No Account-Wide Lock-In**
```
Each entry has independent capacity
No scale parameter to get stuck with
Mixed sizes naturally supported
```

**5. Deletion Still Refunds**
```
Delete entry with capacity=8 → refund 8 reserves
Recreate with 300 bytes → starts at capacity=2
Clean slate for new data
```

**6. AccountSet Becomes Optional**
```
Current: Must set scale before creating entries
High water mark: Scale is per-entry maximum (optional ceiling)

AccountSet scale=8: "No entry can exceed capacity=8"
No AccountSet: Each entry can grow to 16 (4096 bytes max)
```

### Comparison with Other Approaches

| Feature | Current (Account Scale) | Fixed Per-Entry | High Water Mark |
|---------|------------------------|-----------------|-----------------|
| **Upfront guessing** | ✗ Must set scale | ✗ Must know size | ✓ Grows as needed |
| **Account lock-in** | ✗ Stuck at scale | ✓ Independent | ✓ Independent |
| **Per-entry lock-in** | ✗ All same scale | ✗ Fixed at creation | ✓ Grows with use |
| **Reserve on shrink** | ✓ No check | ✓ No check | ✓ No check |
| **Reserve on grow** | ✓ No check | ✗ FAIL (fixed) | ✓ Check delta only |
| **Predictable** | ⚠️ If you guess right | ⚠️ If you guess right | ✓ Always |
| **Storage overhead** | None (in AccountRoot) | +1 byte per entry | +1 byte per entry |
| **Hook API changes** | None | None | None |

### Disadvantages

**1. Can't Reclaim Reserve Without Delete**
```
Entry grew to 2KB (capacity=8)
Data shrinks to 100 bytes permanently
Still paying 8 reserves
Must delete+recreate to get refund
```

**2. Accidental Growth = Permanent**
```
Bug causes entry to temporarily grow to 4KB
Bug fixed, data back to 256 bytes
Capacity stuck at 16 (high water mark)
Paying 16 reserves forever (unless delete+recreate)
```

**3. Storage Per Entry**
```
Every ltHOOK_STATE needs sfHookStateCapacity field
Slight ledger bloat vs account-wide scale
```

### Edge Case: AccountSet as Ceiling

**Optional enhancement:**
```cpp
// AccountSet with HookStateMaxCapacity
AccountSet {
    HookStateMaxCapacity: 8  // No entry can exceed capacity=8
}

// Hook tries to grow beyond ceiling
state_set(key, 2500 bytes)  // Would need capacity=10
→ FAIL: Exceeds account maximum capacity of 8
→ Must AccountSet HookStateMaxCapacity=10 first
```

This allows accounts to:
- Start permissive (no ceiling)
- Lock down after deployment (prevent runaway growth)
- Explicitly raise ceiling when needed

### Migration from Current Design

**Backward compatibility:**
```cpp
// Read old-style entries
if (!hookState->isFieldPresent(sfHookStateCapacity)) {
    // Legacy entry - assume capacity based on current size
    capacity = (data.size() + 255) / 256;
    // Or: use account scale if present
    capacity = sleAccount->getFieldU16(sfHookStateScale) ?: 1;
}
```

**Gradual migration:**
1. Amendment enables sfHookStateCapacity field
2. New entries: use high water mark
3. Old entries: migrate on first modification
4. Both systems coexist during transition

### Recommendation

**High water mark is the optimal design:**

✓ Fixes account-wide lock-in (per-entry capacity)
✓ Fixes per-entry lock-in (grows as needed)
✓ Satisfies Richard's concern (reserves = capacity, not current size)
✓ No upfront guessing required
✓ Modifications predictable (only grow checks reserves)
✓ One-way = simple mental model
✓ Deletion still allows cleanup
✓ Optional ceiling via AccountSet

**Only real downside:** Can't reclaim reserves from temporarily-large data without delete+recreate. But this is true of all approaches except pure usage-based (which Richard dislikes).

This should be seriously considered as a replacement for the current account-wide scale approach before the feature ships.

---

## Comparing the Three Approaches

### Summary Table

| Feature | Account-Wide Scale (Current) | Fixed Per-Entry Capacity | High Water Mark Capacity |
|---------|------------------------------|-------------------------|--------------------------|
| **Implementation Status** | ✅ Implemented in PR #406 | 💡 Proposed alternative | 💡 Proposed alternative |
| **Complexity** | Low (one field in AccountRoot) | Moderate (+field per entry) | Moderate (+field per entry) |
| **Storage Overhead** | Minimal (one uint16 per account) | +1 byte per ltHOOK_STATE | +1 byte per ltHOOK_STATE |
| **Upfront Guessing** | ⚠️ Must set scale first | ⚠️ Fixed at creation | ✅ Grows automatically |
| **Account Lock-In** | ❌ Stuck unless state=0 | ✅ Each entry independent | ✅ Each entry independent |
| **Entry Resize** | ✅ Within scale limit | ❌ Fixed forever | ✅ Grows, never shrinks |
| **Multi-Hook Accounts** | ❌ All entries same scale | ✅ Mixed sizes | ✅ Mixed sizes, optimal |
| **Reserve Predictability** | ✅ No change on modifications | ✅ No change ever | ✅ Only on capacity growth |
| **Overpayment Risk** | ⚠️ High (all entries × scale) | ⚠️ Medium (if guess too high) | ✅ Minimal (actual usage) |
| **Satisfies Richard's Concern** | ✅ Capacity-based | ✅ Capacity-based | ✅ Capacity-based |
| **Hook API Changes** | ✅ None | ✅ None | ✅ None |
| **Testing Complexity** | Low | Medium | Medium |
| **Migration Path** | N/A (current) | Can coexist with current | Can coexist with current |

### Detailed Comparison

#### 1. Account-Wide Scale (Current Design)

**How it works:**
```
AccountSet scale=8
All entries limited to 256×8 = 2048 bytes
All entries cost 8 reserves each
```

**Best for:**
- ✅ Accounts where all hooks need similar data sizes
- ✅ Simple mental model: one parameter controls everything
- ✅ Already implemented and tested

**Problems:**
- ❌ **The Footgun:** Can't reduce scale without deleting all state
- ❌ **Overpayment:** 1000 small entries at scale=8 = 8000 reserves
- ❌ **Lock-in:** One hook needs large state → account stuck at high scale forever

**Example scenario:**
```
Account has 3 hooks:
- Counter hook: 50 entries × 100 bytes
- Flag hook: 200 entries × 80 bytes
- Metadata hook: 10 entries × 1800 bytes

Must set scale=8 for metadata hook
Pay: 260 entries × 8 = 2080 reserves
Reality: Need only ~60 reserves for actual usage
Overpayment: 35× more than needed
```

#### 2. Fixed Per-Entry Capacity

**How it works:**
```
state_set(key, 300 bytes)
Capacity = ceil(300/256) = 2
Entry can hold up to 512 bytes forever
Pay 2 reserves
```

**Best for:**
- ✅ Predictable data sizes per entry
- ✅ Multi-hook accounts with known requirements
- ✅ Mixed sizes without account-wide parameter

**Problems:**
- ❌ **Growth blocked:** Entry created at 300 bytes can never exceed 512 bytes
- ❌ **Guessing required:** Must predict max size at creation
- ❌ **Resize = delete:** Must delete and recreate to change capacity

**Example scenario:**
```
Hook stores user preferences:
- Created with 200 bytes (capacity=1, max 256 bytes)
- User adds more preferences → 350 bytes needed
- ❌ BLOCKED: Exceeds capacity
- Must: delete entry, lose data, recreate with new capacity
```

#### 3. High Water Mark Capacity (Recommended)

**How it works:**
```
state_set(key, 300 bytes) → capacity=2, reserves=2
state_set(key, 800 bytes) → capacity=4, reserves=4 (+2 check)
state_set(key, 200 bytes) → capacity=4, reserves=4 (no check)
state_set(key, 1100 bytes) → capacity=5, reserves=5 (+1 check)
```

**Best for:**
- ✅ **Everything** - most flexible and fair approach
- ✅ Organic growth without guessing
- ✅ Multi-hook accounts with varying needs
- ✅ Predictable reserve checks (only on capacity growth)

**Problems:**
- ⚠️ **Accidental growth:** Bug causes temporary spike → capacity stuck high
- ⚠️ **No reclaim:** Data shrinks permanently → still paying for capacity
- 💡 **Solution:** Delete and recreate entry to reset capacity

**Example scenario:**
```
Same 3 hooks:
- Counter: 50 entries × 100 bytes = 50 reserves (capacity=1 each)
- Flag: 200 entries × 80 bytes = 200 reserves (capacity=1 each)
- Metadata: 10 entries × 1800 bytes = 70 reserves (capacity=7 each)

Total: 320 reserves
vs Account scale=8: 2080 reserves
Savings: 85% less reserves locked
```

### Use Case Recommendations

**Choose Account-Wide Scale (Current) if:**
- Single-purpose account (one hook type)
- All entries have similar size requirements
- Willing to accept lock-in trade-off for simplicity
- Already deployed and working

**Choose Fixed Per-Entry Capacity if:**
- Data sizes are very predictable per entry type
- Entries rarely need to grow
- Want per-entry independence without growth
- Prefer explicit capacity declaration

**Choose High Water Mark Capacity if:**
- Multi-hook accounts with diverse needs ⭐
- Data sizes may grow over time
- Want optimal reserve usage
- Deploying new system (not migration constraint)

### Migration Strategy

**If starting fresh:**
→ Implement **High Water Mark** from the beginning

**If PR #406 already merged:**
1. **Short term:** Add scale reduction via directory walk (fixes footgun)
2. **Medium term:** Add sfHookStateCapacity field via amendment
3. **Long term:** Deprecate sfHookStateScale, migrate to high water mark

**Backward compatibility:**
```cpp
// Support both during transition
if (hookState->isFieldPresent(sfHookStateCapacity)) {
    // New system: per-entry capacity
    capacity = hookState->getFieldU8(sfHookStateCapacity);
} else {
    // Legacy: use account scale
    capacity = sleAccount->getFieldU16(sfHookStateScale) ?: 1;
}
```

### Final Recommendation

**High Water Mark Capacity is the optimal long-term design:**

✅ Eliminates both account-wide and per-entry lock-in
✅ No upfront guessing required
✅ Automatic, organic growth
✅ Optimal reserve usage (pay for what you use)
✅ Supports diverse multi-hook accounts
✅ Satisfies Richard's concerns
✅ One-way growth = predictable behavior

**The only downside** (can't reclaim reserves from temporary spikes without delete/recreate) is acceptable given the massive advantages.

**Recommendation:** Seriously consider implementing High Water Mark instead of current design before PR #406 merges, or plan it as the next amendment if already merged.

---

## Implementation Details (Current Design)

### Key Files and Locations

**AccountSet Transaction:**
- `src/ripple/app/tx/impl/SetAccount.cpp`
  - Line 187-197: Preflight validation (scale 1-16)
  - Line 264-276: Preclaim checks (block decrease if stateCount > 0)
  - Line 660-700: DoApply scale change and reserve adjustment

**Hook State Management:**
- `src/ripple/app/hook/impl/applyHook.cpp`
  - Line 1062: `setHookState()` - Actual ledger updates (Phase 2)
  - Line 1470: `set_state_cache()` - Virtual accounting (Phase 1)
  - Line 1622: `state_set()` - Hook API (local state)
  - Line 1651: `state_foreign_set()` - Hook API (foreign state)
  - Line 1838: `finalizeHookState()` - Commit cached changes

**Ledger Formats:**
- `src/ripple/protocol/impl/LedgerFormats.cpp`
  - Line 71: `{sfHookStateScale, soeOPTIONAL}` in AccountRoot
  - Line 59: `{sfHookStateCount, soeOPTIONAL}` in AccountRoot
  - Line 244-251: HookState ledger entry definition

**Field Definitions:**
- `src/ripple/protocol/impl/SField.cpp` - Field declarations
- `src/ripple/protocol/SField.h` - Field headers

**Size Limits:**
- `src/ripple/app/hook/Enum.h:49-57` - `maxHookStateDataSize(hookStateScale)`

**Tests:**
- `src/test/app/SetHook_test.cpp` - Hook state scale tests
- `src/test/rpc/AccountSet_test.cpp` - AccountSet validation tests

### Code Review Notes

**Issue: Missing field presence check (line 268)**
```cpp
// Current (preclaim):
uint16_t const currentScale = sle->getFieldU16(sfHookStateScale);
// Returns 0 if field absent (via STI_NOTPRESENT → V() → 0)

// Should be (like line 662):
uint16_t const currentScale = sle->isFieldPresent(sfHookStateScale)
    ? sle->getFieldU16(sfHookStateScale)
    : 1;
```

**Status:** Semantically wrong but functionally harmless. The check `newScale < currentScale` uses 0 instead of 1, but since `newScale >= 1` (validation blocks 0), the comparison `newScale < 0` is always false. Works accidentally but should be fixed for consistency.

**Issue: Potential overflow (line 679-680)**
```cpp
uint32_t const newOwnerCount = oldOwnerCount -
    (oldScale * stateCount) + (newScale * stateCount);
```

**Analysis:**
- Overflow at: `16 × stateCount > 2^32`
- Critical threshold: `stateCount > 268,435,456`
- Economic constraint: 268M entries × 16 scale = 4.3B reserves ≈ 43B XRP at 10 XRP/reserve
- **Verdict:** Theoretically possible, economically impossible. No explicit limit on stateCount, but reserves self-limit.

**Issue: Sanity check (line 683)**
```cpp
if (newOwnerCount < oldOwnerCount)
    return tecINTERNAL;
```

**Status:** Actually correct. Detects arithmetic underflow/bugs. Since scale decreases are blocked at line 270, this should never trigger in normal operation. If it does, indicates internal error.

### Field Default Behavior

**STObject optional field handling:**
```cpp
template <typename T, typename V>
V STObject::getFieldByValue(SField const& field) const
{
    const STBase* rf = peekAtPField(field);
    if (!rf)
        throwFieldNotFound(field);  // Field not registered

    SerializedTypeID id = rf->getSType();
    if (id == STI_NOTPRESENT)
        return V();  // Optional field not present → returns default

    const T* cf = dynamic_cast<const T*>(rf);
    if (!cf)
        Throw<std::runtime_error>("Wrong field type");

    return cf->value();
}
```

For `sfHookStateScale` (soeOPTIONAL):
- Not present → returns `0` (uint16_t default)
- Present → returns actual value

**This is why line 662-664 uses explicit `?: 1` pattern - to make semantic default explicit.**

### Test Coverage

**Covered:**
- ✓ Amendment gating
- ✓ Validation (scale 0 and 17 blocked)
- ✓ Field optimization (scale=1 → absent)
- ✓ Decrease blocking (stateCount > 0)
- ✓ Basic OwnerCount arithmetic

**Not covered:**
- Reserve balance checks (tecINSUFFICIENT_RESERVE scenarios)
- Actual hook state creation lifecycle
- Scale changes with real hook state entries
- Multi-step scale changes (1→8→4→16)
- Integration testing with multiple hooks

### PR Context

**Source:** GitHub PR #406 on Xahau/xahaud
**Status:** Under review, addressing comments
**Discussion highlights:**
- Richard: "I really dislike the idea of a variable length field claiming different reserves depending on its current size"
- Tequ addressed overflow checks, type consistency issues
- Agreement on account-wide scale approach
- Tests demonstrate basic functionality

---

## Feature Components

[To be continued with specific modifications...]
