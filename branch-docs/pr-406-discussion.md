# PR #406 Discussion Context - Extended Hook State

This document captures all discussion from GitHub PR #406 (ExtendedHookState feature).

## Timeline

- **Initial PR**: Feature to extend hook state beyond 256 bytes using account-wide scale parameter
- **Early Reviews**: Feb 2025 - tequdev's TODO notes
- **Richard's Feedback**: June 2025 - Design philosophy discussion
- **Recent Reviews**: Oct 2025 - Code review comments from RichardAH, tequdev, sublimator

---

## Design Philosophy Discussion (June 2025)

### Richard's Initial Position (2025-06-18)

> I think we should not add the complexity of incremental reserve increase, rather just increase the allowable size for the same existing reserve cost. I think 1 reserve for 2048 bytes is probably ok.
>
> An alternative simpler fee model would be to nominate your account for "large hook data" whereupon all your hook states cost more to store but you have access to the larger size. **I really dislike the idea of a variable length field claiming different reserves depending on its current size.**

**Key insight**: Richard's core objection is to reserves varying based on current data size.

### Evolution to Scale-Based Design (2025-06-20)

Richard's "large hook data" proposal:

> Sure, so you do an account set flag which is like the equivalent of specifying large pages in your OS. All your hook states cost more but they're allowed to be bigger. It makes the computation easier. So let's say in large mode you get 2048 bytes per hook state but in computing reserve requirements for your hook state we take your HookStateCount and multiply it by 8. Once the flag is enabled it can only be disabled if HookStateCount is 0.

### Tequdev's Refinement (2025-06-22)

> That's interesting. Instead of using the flag, how about using a numeric field to represent the maximum size the account allows?
>
> If we use the flag, whenever we need to increase the protocol's maximum allowed size, we end up having to add a new flag.

### Richard Agrees (2025-06-25)

> I think that's reasonable. We could say HookStateScale or something 0,1 ...

**Result**: Current design with `sfHookStateScale` numeric field (1-16) was born from this discussion.

---

## Code Review Comments (October 2025)

### Critical Issues

#### 1. Scale Change Restriction (RichardAH, 2025-10-22)

**Location**: `src/ripple/app/tx/impl/SetAccount.cpp:273`

> shouldn't be able to set/change it at all unless hook state count is zero

**Current code**:
```cpp
if (stateCount > 0 && newScale < currentScale)
    return tecHAS_HOOK_STATE;
```

**Richard's concern**: Should block ALL changes when `stateCount > 0`, not just decreases.

**sublimator's response** (2025-10-22):
> This does make the feature hard to test out without trapping yourself though, right? Buyer beware?

**Analysis**:
- Current: Can increase scale freely, can only decrease if `stateCount == 0`
- Richard wants: Can only change (up or down) if `stateCount == 0`
- Trade-off: More restrictive = safer but less flexible

**sublimator's test suggestion** (2025-10-23):
```cpp
// Test the escape hatch - decrease WITHOUT state
applyCount(5, 0, 50);  // ← stateCount=0 (all state deleted)
jt[sfHookStateScale.fieldName] = 2;
env(jt);  // ← Should succeed! This is how you escape high scale
BEAST_EXPECT(env.le(alice)->getFieldU16(sfHookStateScale) == 2);
```

#### 2. Overflow Sanity Check (RichardAH, 2025-10-22)

**Location**: `src/ripple/app/tx/impl/SetAccount.cpp:680`

> sanity check newOwnerCount > oldOwnerCount

**Current code**:
```cpp
uint32_t const newOwnerCount = oldOwnerCount -
    (oldScale * stateCount) + (newScale * stateCount);

if (newOwnerCount < oldOwnerCount)
    return tecINTERNAL;  // ← This IS the sanity check
```

**Status**: Actually already implemented correctly! The check detects underflow.

**Mathematical impossibility**: Would need 268M entries × 16 scale ≈ 43B XRP in reserves - economically impossible.

#### 3. TER Code Comment Clarity (RichardAH, 2025-10-22)

**Location**: `src/ripple/protocol/impl/TER.cpp:97`

**First suggestion** (2025-06-30):
> maybe expand the explanation to "The account has hook state. Delete all existing state before changing the scale."

**Updated suggestion** (2025-10-22):
> Change comment to something like: Delete all hook state before reducing scale.

**tequdev's response** (2025-10-22):
> Since the number of TER codes is limited, we used a generic error message. However, we will change it as you pointed out, as it can be modified later when this code is used for other purposes.

**Status**: Will be updated to be more specific about scale reduction requirement.

---

### Code Quality Issues

#### 4. Magic Number Constants (sublimator, 2025-10-22)

**Location 1**: `src/ripple/app/tx/impl/SetAccount.cpp:195`
```cpp
if (scale == 0 || scale > 16)  // ← extract constant
```

> extract a constant for this ?

**Location 2**: `src/ripple/app/hook/Enum.h:56`
```cpp
return 256U * hookStateScale;  // ← add defensive check
```

> we could add an extra defensive check here with a constant, I guess

**Suggested improvement**:
```cpp
constexpr uint16_t MAX_HOOK_STATE_SCALE = 16;
constexpr uint32_t HOOK_STATE_BASE_SIZE = 256;

// In validation:
if (scale == 0 || scale > MAX_HOOK_STATE_SCALE)
    return temMALFORMED;

// In size calculation:
inline uint32_t maxHookStateDataSize(uint16_t hookStateScale)
{
    if (hookStateScale == 0 || hookStateScale > MAX_HOOK_STATE_SCALE)
        return HOOK_STATE_BASE_SIZE;
    return HOOK_STATE_BASE_SIZE * hookStateScale;
}
```

#### 5. Type Consistency (RichardAH, 2025-06-29)

**Location**: `src/ripple/app/hook/applyHook.h:35`

> I don't mind if you use int16_t or uint16_t or uint32_t but keep it consistent with the function calls? like `maxHookStateDataSize(uint32_t hookStateScale)` same for the sf type

**Issue**: `sfHookStateScale` is `uint16_t` but function parameter types vary.

**Status**: Needs consistency pass to ensure all uses of scale parameter use `uint16_t`.

#### 6. Pointless Change? (RichardAH, 2025-06-29)

**Location**: `src/ripple/app/tx/impl/SetHook.cpp:930`

> was there a reason for changing this? I assume it doesn't change the outcome

**tequdev's response** (2025-10-22):
> https://github.com/Xahau/xahaud/commit/a75daaea0249c1fabd773c4ef1ee08929cd9d3a3

**Status**: Addressed in commit a75daae.

---

### Defensive Programming Issues

#### 7. Overflow Checks in Hook API (RichardAH, 2025-06-29)

**Locations**:
- `src/ripple/app/hook/impl/applyHook.cpp:1540`
- `src/ripple/app/hook/impl/applyHook.cpp:1577`
- `src/ripple/app/hook/impl/applyHook.cpp:1599`
- `src/ripple/app/tx/impl/SetHook.cpp:946`

> we need defensive sanity checks on math like this
> overflow sanity check needed
> here too
> we should do a sanity check on this value before passing it to make sure it is reasonable and hasn't overflowed

**tequdev's response** (2025-10-18):
All addressed in commit `91f683a9994e02c29f85cd3aede807bb6ebee93f`

**Status**: ✅ Fixed with sanity checks added.

---

## Early Development TODOs (Feb-March 2025)

### 1. Reserve Check Optimization (tequdev, 2025-02-12)

**Location**: `src/ripple/app/hook/impl/applyHook.cpp:1170`

> TODO: Don't return `tecINSUFFICIENT_RESERVE` if the OwnerCount decreases.

**Follow-up** (2025-02-14):
> This check is not made because set_state_cache, which performs the reserve check, does not access the StateData before the change.

**Analysis**: When modifying existing state to be smaller, current code may incorrectly require reserves. However, the virtual reserve check in `set_state_cache()` can't see the old data size to know if it's decreasing.

### 2. Owner Count Decrease Testing (tequdev, 2025-02-14)

**Location**: `src/ripple/app/tx/impl/SetHook.cpp:929`

> TODO: needs tests that OwnerCount decreases depending on HookState size when NS deleted.

**Status**: Still needed. When namespace (NS) is deleted, OwnerCount should decrease by `scale × stateCount`.

### 3. Insufficient Reserves Testing (tequdev, 2025-03-29)

**Review submission**:
> TODO: Tests for Insufficient Reserves

**sublimator's test case** (2025-10-23):
```cpp
// Insufficient reserves on increase
applyCount(1, 100, 200);  // 100 entries at scale=1
// Manually set balance to just cover current reserves
jt[sfHookStateScale.fieldName] = 16;  // Needs 1600 reserves (100×16)
env(jt, ter(tecINSUFFICIENT_RESERVE));  // Should fail - can't afford it
```

---

## Summary of Outstanding Issues

### Must Fix Before Merge

1. **TER comment clarity** - Update error message per Richard's suggestion
2. **Extract magic constants** - Define `MAX_HOOK_STATE_SCALE` and `HOOK_STATE_BASE_SIZE`
3. **Type consistency** - Ensure all scale parameters use `uint16_t`

### Should Fix Before Merge

4. **Scale change restriction** - Decide: block all changes when `stateCount > 0`, or just decreases?
5. **Test coverage gaps**:
   - Escape hatch test (decrease scale after deleting all state)
   - Insufficient reserves on scale increase
   - OwnerCount decrease when namespace deleted
   - Reserve check optimization when modifying to smaller size

### Already Fixed ✅

- Overflow sanity checks (commit 91f683a)
- Pointless change in SetHook.cpp (commit a75daae)

---

## Design Trade-offs Discussed

### Current Design: Account-Wide Scale

**Pros**:
- Simple reserve calculation: `stateCount × scale`
- Deterministic reserve requirements
- No per-entry metadata overhead
- Satisfies Richard's objection to size-based reserves

**Cons**:
- "Scale Commitment Trap" - can't decrease without deleting all state
- All entries pay for max capacity even if using less
- Hard to test without trapping yourself (sublimator's concern)

### Alternative: High Water Mark Capacity (from spec)

**Concept**: Each entry stores `HookStateCapacity` = max size ever seen

**Pros**:
- Granular reserves per entry
- One-way growth (can't shrink capacity)
- Still deterministic (capacity-based, not size-based)
- Satisfies Richard's objection (reserves based on capacity, not current size)
- More economical for mixed workloads

**Cons**:
- Per-entry metadata overhead (2 bytes per entry)
- More complex reserve calculations
- Ledger format changes required

### Richard's Original Objection

> I really dislike the idea of a variable length field claiming different reserves depending on its current size.

Both current design (scale-based) and high water mark satisfy this because reserves are based on:
- Current: declared scale capacity
- Alternative: declared entry capacity

Neither varies with current data size.

---

## Key Insights from Discussion

1. **Scale as "Large Pages"**: Richard's OS analogy - scale is like requesting large page size for all your hook states

2. **Numeric Field > Flags**: Tequdev's insight that numeric field is more extensible than adding flags for each size tier

3. **Testing Trap**: sublimator identified that you can't easily test increasing scale without potentially trapping yourself

4. **Escape Hatch**: The ability to decrease scale when `stateCount == 0` is critical - it's the only way out of high scale

5. **Economic Impossibility**: Overflow concerns are theoretical - would require ~43B XRP in reserves

---

## Chronological Review Activity

- **Feb 12, 2025**: tequdev - Reserve check optimization TODO
- **Feb 14, 2025**: tequdev - Clarification on reserve check limitation
- **Feb 14, 2025**: tequdev - OwnerCount decrease testing TODO
- **Mar 29, 2025**: tequdev - Insufficient reserves testing TODO
- **Jun 18, 2025**: RichardAH - Design philosophy: dislike size-based reserves
- **Jun 19, 2025**: tequdev - Request clarification on "large hook data"
- **Jun 20, 2025**: RichardAH - Explain flag-based approach
- **Jun 22, 2025**: tequdev - Suggest numeric field instead of flags
- **Jun 25, 2025**: RichardAH - Agree to HookStateScale field
- **Jun 29-30, 2025**: RichardAH - Code review: consistency, sanity checks, comments
- **Oct 18, 2025**: tequdev - Address overflow checks (commit 91f683a)
- **Oct 22, 2025**: tequdev - Address SetHook.cpp changes (commit a75daae)
- **Oct 22, 2025**: RichardAH - Scale change restriction, overflow sanity, TER comment
- **Oct 22, 2025**: tequdev - TER comment rationale
- **Oct 22, 2025**: sublimator - Magic constants, testing trap concern
- **Oct 23, 2025**: sublimator - Test case suggestions (escape hatch, insufficient reserves)
