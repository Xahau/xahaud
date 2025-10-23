# High Water Mark Capacity Tests - Implementation Plan

## Current Situation

**Problem:** Tried to add high water mark capacity tests to existing `test_state_set()` function, but:
- Earlier tests in the suite install hooks on shared accounts (bob, alice, etc.)
- These hooks remain installed and interfere with later tests
- Test execution order and account state are entangled
- Hard to isolate failures

**Solution:** Create a new dedicated test function with clean accounts.

---

## Test Strategy

### New Test Function: `test_high_water_mark_capacity()`

Create a completely isolated test function that:
- Uses fresh accounts not touched by other tests
- Tests high water mark capacity behavior systematically
- Is independent and can run in any order
- Guards execution with `featureExtendedHookState` check

### Location

Add to `SetHook_test.cpp` as a new test case:

```cpp
void
test_high_water_mark_capacity(FeatureBitset features)
{
    testcase("Test high water mark capacity");
    using namespace jtx;

    // Only run if ExtendedHookState feature is enabled
    if (!features[featureExtendedHookState])
        return;

    Env env{*this, features};

    // Use completely fresh accounts
    auto const alice = Account{"alice_hwm"};
    auto const bob = Account{"bob_hwm"};

    env.fund(XRP(10000), alice);
    env.fund(XRP(10000), bob);

    // ... tests here ...
}
```

Register in test runner:

```cpp
void run() override
{
    // ... existing tests ...

    testHookStateScale(env);  // Remove this (old account-wide scale tests)
    test_high_water_mark_capacity(env);  // Add this
}
```

---

## Test Cases

### Test 1: Capacity Grows Automatically (300 → 800 bytes)

**Purpose:** Verify capacity increases when data size exceeds current capacity.

**Setup:**
- Install hook that writes 300 bytes to key "test1"
- Invoke hook → creates entry with capacity=2 (ceil(300/256))

**Action:**
- Update hook to write 800 bytes to same key
- Invoke hook

**Assertions:**
- OwnerCount increases from 3 to 5 (1 hook + 2→4 capacity)
- HookStateCount stays at 1 (modifying, not creating)
- ledger entry `sfHookStateCapacity` = 4
- ledger entry `sfHookStateData.size()` = 800

**Hook WASM:**
```cpp
static uint8_t data[800] = { /* pattern */ };
int64_t hook(uint32_t reserved)
{
    _g(1,1);
    uint8_t key[32] = {0};
    ASSERT(state_set(SBUF(data), SBUF(key)) == 800);
    accept(0,0,0);
}
```

---

### Test 2: Capacity Never Shrinks (800 → 100 bytes)

**Purpose:** Verify high water mark - capacity doesn't decrease when data shrinks.

**Setup:**
- Entry already exists with capacity=4 from Test 1

**Action:**
- Update hook to write 100 bytes to same key
- Invoke hook

**Assertions:**
- OwnerCount stays at 5 (capacity unchanged)
- `sfHookStateCapacity` = 4 (unchanged - high water mark!)
- `sfHookStateData.size()` = 100 (data did shrink)

**Key Insight:** Paying for capacity 4, but only using 100 bytes.

---

### Test 3: Re-growth Within Capacity (100 → 700 bytes)

**Purpose:** Verify no reserve check when growing within existing capacity.

**Setup:**
- Entry has capacity=4, current data=100 bytes

**Action:**
- Update hook to write 700 bytes (needs capacity=3, have capacity=4)
- Invoke hook

**Assertions:**
- OwnerCount stays at 5 (no reserve change)
- `sfHookStateCapacity` = 4 (unchanged)
- `sfHookStateData.size()` = 700

**Key Insight:** Can grow back to 1024 bytes (4*256) without paying more reserves.

---

### Test 4: Growth Beyond High Water Mark (700 → 1100 bytes)

**Purpose:** Verify capacity increases and reserves charged when exceeding high water mark.

**Setup:**
- Entry has capacity=4, current data=700 bytes

**Action:**
- Update hook to write 1100 bytes (needs capacity=5)
- Invoke hook

**Assertions:**
- OwnerCount increases from 5 to 6 (capacity delta +1)
- `sfHookStateCapacity` = 5
- `sfHookStateData.size()` = 1100

---

### Test 5: Multiple Entries with Mixed Capacities

**Purpose:** Verify each entry tracks its own capacity independently.

**Setup:**
- Create entry at key1 with 256 bytes (capacity=1)
- Create entry at key2 with 2000 bytes (capacity=8)
- Create entry at key3 with 500 bytes (capacity=2)

**Assertions:**
- OwnerCount = 12 (1 hook + 1 + 8 + 2 capacities)
- HookStateCount = 3
- Each entry has correct `sfHookStateCapacity`

---

### Test 6: Maximum Size Enforcement (4096 bytes)

**Purpose:** Verify absolute maximum size limit.

**Action:**
- Hook attempts to write 4096 bytes → should succeed
- Hook attempts to write 4097 bytes → should fail with TOO_BIG

**Assertions:**
- 4096 byte entry: capacity=16, succeeds
- 4097 byte entry: fails in hook with TOO_BIG error code

---

### Test 7: Deletion Refunds Capacity

**Purpose:** Verify deleting entry refunds reserves based on capacity, not current data size.

**Setup:**
- Entry has capacity=8, current data=100 bytes
- OwnerCount = 9 (1 hook + 8 capacity)

**Action:**
- Delete entry (set empty data)

**Assertions:**
- OwnerCount = 1 (refunded 8, not 1)
- HookStateCount = 0
- Entry removed from ledger

---

### Test 8: Namespace Deletion with Mixed Capacities

**Purpose:** Verify namespace deletion sums individual entry capacities correctly.

**Setup:**
- Create entries in namespace "test":
  - key1: capacity=2
  - key2: capacity=5
  - key3: capacity=1
- OwnerCount = 9 (1 hook + 2 + 5 + 1)

**Action:**
- Delete namespace via SetHook with hsfNSDELETE flag

**Assertions:**
- OwnerCount = 1 (refunded 2+5+1 = 8)
- HookStateCount = 0
- All entries removed
- Namespace removed from HookNamespaces

---

### Test 9: Legacy Entry Migration (no capacity field)

**Purpose:** Verify backward compatibility - entries without capacity field work correctly.

**Setup:**
- Manually create ledger entry without `sfHookStateCapacity` field
- Entry has 512 bytes of data

**Action:**
- Hook modifies entry to 300 bytes

**Expected Behavior:**
- Old capacity calculated as ceil(512/256) = 2
- New capacity max(ceil(300/256), 2) = max(2, 2) = 2
- No reserve change
- Entry now has `sfHookStateCapacity` = 2 (field added)

**Alternative Action:**
- Hook modifies entry to 800 bytes

**Expected Behavior:**
- Old capacity = 2 (from data size)
- New capacity = max(4, 2) = 4
- Reserves increase by 2
- Entry now has `sfHookStateCapacity` = 4

---

### Test 10: Reserve Exhaustion

**Purpose:** Verify reserve checks prevent capacity growth when insufficient balance.

**Setup:**
- Account with minimal balance (e.g., 2600 XAH)
- Already has 1 hook + some state

**Action:**
- Try to create entry with large capacity that would exceed available reserves

**Assertions:**
- Hook rejects with RESERVE_INSUFFICIENT error code
- No state created
- OwnerCount unchanged

---

## Hook Implementation Patterns

### Pattern 1: Simple Hook (Fixed Size)

Use static/global arrays to avoid stack limits:

```cpp
static uint8_t data[SIZE] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,
    // ... partial initialization, rest zero-filled
};

int64_t hook(uint32_t reserved)
{
    _g(1,1);
    uint8_t key[32] = {0};
    ASSERT(state_set(SBUF(data), SBUF(key)) == SIZE);
    accept(0,0,0);
}
```

### Pattern 2: Multi-Entry Hook

```cpp
static uint8_t data1[256] = { /* ... */ };
static uint8_t data2[2000] = { /* ... */ };
static uint8_t data3[500] = { /* ... */ };

int64_t hook(uint32_t reserved)
{
    _g(1,1);
    uint8_t key1[32] = {1};
    uint8_t key2[32] = {2};
    uint8_t key3[32] = {3};

    ASSERT(state_set(SBUF(data1), SBUF(key1)) == 256);
    ASSERT(state_set(SBUF(data2), SBUF(key2)) == 2000);
    ASSERT(state_set(SBUF(data3), SBUF(key3)) == 500);

    accept(0,0,0);
}
```

### Pattern 3: Delete Hook

```cpp
int64_t hook(uint32_t reserved)
{
    _g(1,1);
    uint8_t key[32] = {0};
    ASSERT(state_set(0, 0, SBUF(key)) == 0);  // Empty data = delete
    accept(0,0,0);
}
```

---

## Test Organization

```cpp
void
test_high_water_mark_capacity(FeatureBitset features)
{
    testcase("Test high water mark capacity");

    if (!features[featureExtendedHookState])
        return;

    using namespace jtx;
    Env env{*this, features};

    // Fresh accounts
    auto const alice = Account{"alice_hwm"};
    auto const bob = Account{"bob_hwm"};
    env.fund(XRP(10000), alice);
    env.fund(XRP(10000), bob);

    // Test 1: Capacity grows automatically
    {
        TestHook hook300 = wasm[R"[test.hook](
            // ... 300 byte hook ...
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook300, overrideFlag)}}, 0),
            M("install 300 byte hook"), HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)), M("create 300 byte entry"), fee(XRP(1)));
        env.close();

        BEAST_EXPECT((*env.le(alice))[sfOwnerCount] == 3);
        // ... more assertions ...
    }

    // Test 2: Capacity never shrinks
    {
        TestHook hook800 = wasm[R"[test.hook](
            // ... 800 byte hook ...
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook800, overrideFlag)}}, 0),
            M("update to 800 byte hook"), HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)), M("grow to 800 bytes"), fee(XRP(1)));
        env.close();

        BEAST_EXPECT((*env.le(alice))[sfOwnerCount] == 5);
        // ... more assertions ...
    }

    // Test 3: Shrink data size
    {
        TestHook hook100 = wasm[R"[test.hook](
            // ... 100 byte hook ...
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook100, overrideFlag)}}, 0),
            M("update to 100 byte hook"), HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)), M("shrink to 100 bytes"), fee(XRP(1)));
        env.close();

        // High water mark - capacity stays at 4
        BEAST_EXPECT((*env.le(alice))[sfOwnerCount] == 5);

        auto const state = env.le(ripple::keylet::hookState(
            alice.id(), beast::zero, beast::zero));
        BEAST_REQUIRE(!!state);
        BEAST_EXPECT(state->getFieldU16(sfHookStateCapacity) == 4);
        BEAST_EXPECT(state->getFieldVL(sfHookStateData).size() == 100);
    }

    // ... more tests ...
}
```

---

## Cleanup Actions

### 1. Revert Current Changes

```bash
git checkout origin/dev -- src/test/app/SetHook_test.cpp
```

### 2. Remove Old Scale Tests

The old `testHookStateScale()` function in `src/test/rpc/AccountSet_test.cpp` should be removed since we're replacing account-wide scale with per-entry capacity.

### 3. Update Test Runner

In the main test runner, replace:
```cpp
testHookStateScale(env);
```

With:
```cpp
test_high_water_mark_capacity(env);
```

---

## Success Criteria

All tests should pass with these characteristics:

- ✅ Tests run in isolation (fresh accounts)
- ✅ Tests are deterministic (no state from previous tests)
- ✅ All 10 test cases pass
- ✅ Capacity field correctly stored/retrieved
- ✅ OwnerCount calculations correct
- ✅ Reserve checks work correctly
- ✅ High water mark behavior verified
- ✅ Legacy compatibility verified

---

## Next Steps

1. **Review this plan** - Make sure approach is sound
2. **Revert test file** - Clean slate
3. **Implement test function** - One test case at a time
4. **Verify each test** - Run and debug incrementally
5. **Add to test suite** - Register in runner
6. **Document** - Update high-water-mark-implementation.md with test results

---

## Notes

### Why New Test Function?

- **Isolation:** No interference from other tests
- **Clarity:** Tests are focused and easy to understand
- **Maintainability:** Easy to add/modify tests
- **Debugging:** Failures are easy to track down

### Why Fresh Accounts?

- Accounts like `alice`, `bob`, `cho` are used throughout the test suite
- They accumulate hooks, state, and other ledger objects
- Trying to predict their state at any point is fragile
- Fresh accounts (`alice_hwm`, `bob_hwm`) are clean

### Why Guard with Feature Check?

- Tests should only run when `featureExtendedHookState` is enabled
- Allows tests to compile but skip when feature is disabled
- Clean way to handle feature-gated functionality

---

## Alternative Approach: Separate File

Instead of adding to `SetHook_test.cpp`, could create `SetHook_ExtendedState_test.cpp`:

**Pros:**
- Complete isolation
- Easier to maintain
- Can have its own test fixture
- Clear separation of concerns

**Cons:**
- Additional build target
- Test discovery configuration
- More files to maintain

**Recommendation:** Start with adding to existing file. If tests grow large, consider splitting later.
