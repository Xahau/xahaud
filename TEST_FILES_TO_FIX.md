# Test Files That Need hash_options Fixes

## How to Check Compilation Errors

Use the `compile_single_v2.py` script to check individual files:
```bash
# Check compilation errors for a specific file
./compile_single_v2.py src/test/app/SomeFile_test.cpp -e 3 --errors-only

# Get just the last few lines to see if it compiled successfully
./compile_single_v2.py src/test/app/SomeFile_test.cpp 2>&1 | tail -5
```

## Originally Fixed Files (11 files)

1. **src/test/app/Import_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

2. **src/test/app/LedgerReplay_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

3. **src/test/app/Offer_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

4. **src/test/app/SetHook_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

5. **src/test/app/SetHookTSH_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

6. **src/test/app/ValidatorList_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

7. **src/test/app/XahauGenesis_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options, keylet::fees() needs hash_options

8. **src/test/consensus/NegativeUNL_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

9. **src/test/consensus/UNLReport_test.cpp**
   - Status: Needs fixing
   - Errors: keylet functions missing hash_options

10. **src/test/jtx/impl/balance.cpp**
    - Status: Needs fixing
    - Errors: keylet functions missing hash_options

11. **src/test/jtx/impl/Env.cpp**
    - Status: Needs fixing
    - Errors: keylet functions missing hash_options

## Fix Strategy

Each file needs:
1. keylet function calls updated to include hash_options{ledger_seq, classifier}
2. The ledger_seq typically comes from env.current()->seq() or view.seq()
3. The classifier matches the keylet type (e.g., KEYLET_ACCOUNT, KEYLET_FEES, etc.)

## Progress Tracking

- [x] Import_test.cpp - FIXED
- [x] LedgerReplay_test.cpp - FIXED
- [x] Offer_test.cpp - FIXED
- [x] SetHook_test.cpp - FIXED
- [x] SetHookTSH_test.cpp - FIXED
- [x] ValidatorList_test.cpp - FIXED (sha512Half calls updated with VALIDATOR_LIST_HASH classifier)
- [x] XahauGenesis_test.cpp - FIXED (removed duplicate hash_options parameters)
- [x] NegativeUNL_test.cpp - FIXED
- [x] UNLReport_test.cpp - FIXED
- [x] balance.cpp - FIXED
- [x] Env.cpp - FIXED

## All original 11 files have been successfully fixed!

## Remaining Files Still Needing Fixes (9 files)

### Status: NOT STARTED
These files still have compilation errors and need hash_options fixes:

1. **src/test/jtx/impl/uritoken.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/jtx/impl/uritoken.cpp -e 3 --errors-only`

2. **src/test/jtx/impl/utility.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/jtx/impl/utility.cpp -e 3 --errors-only`

3. **src/test/ledger/Directory_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/ledger/Directory_test.cpp -e 3 --errors-only`

4. **src/test/ledger/Invariants_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/ledger/Invariants_test.cpp -e 3 --errors-only`

5. **src/test/overlay/compression_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/overlay/compression_test.cpp -e 3 --errors-only`

6. **src/test/rpc/AccountSet_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/rpc/AccountSet_test.cpp -e 3 --errors-only`

7. **src/test/rpc/AccountTx_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/rpc/AccountTx_test.cpp -e 3 --errors-only`

8. **src/test/rpc/Book_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/rpc/Book_test.cpp -e 3 --errors-only`

9. **src/test/rpc/Catalogue_test.cpp**
   - Status: Needs fixing
   - Check errors: `./compile_single_v2.py src/test/rpc/Catalogue_test.cpp -e 3 --errors-only`

## CRITICAL INSTRUCTIONS FOR FIXING

### 1. Read the HashContext enum from digest.h
**ALWAYS** check @src/ripple/protocol/digest.h lines 37-107 for the complete HashContext enum.
This enum defines ALL the valid classifiers you can use in hash_options.

### 2. Understanding hash_options constructor
The hash_options struct (lines 110-126 in digest.h) has TWO constructors:
- `hash_options(HashContext ctx)` - classifier only, no ledger index
- `hash_options(std::uint32_t li, HashContext ctx)` - ledger index AND classifier

### 3. How to classify each hash operation

#### For keylet functions:
- Match the keylet function name to the KEYLET_* enum value
- Examples:
  - `keylet::account()` → use `KEYLET_ACCOUNT`
  - `keylet::fees()` → use `KEYLET_FEES`
  - `keylet::trustline()` → use `KEYLET_TRUSTLINE`
  - `keylet::negativeUNL()` → use `KEYLET_NEGATIVE_UNL`
  - `keylet::UNLReport()` → use `KEYLET_UNL_REPORT`
  - `keylet::hook()` → use `KEYLET_HOOK`
  - `keylet::uriToken()` → use `KEYLET_URI_TOKEN`

#### For sha512Half calls:
- Validator manifests/lists → use `VALIDATOR_LIST_HASH`
- Hook code hashing → use `HOOK_DEFINITION` or `LEDGER_INDEX_UNNEEDED`
- Network protocol → use appropriate context from enum

#### For test environments:
- Use `env.current()->seq()` to get ledger sequence (it's already uint32_t, NO CAST NEEDED)
- Use `ledger->seq()` for Ledger pointers
- Use `view.seq()` for ReadView/ApplyView references

### 4. IMPORTANT: Read the entire file first!
When fixing a file, ALWAYS:
1. Read the ENTIRE file first (or at least 500+ lines) to understand the context
2. Look for patterns of how the test is structured
3. Check what types of ledger objects are being tested
4. Then fix ALL occurrences systematically

### 5. Common patterns to fix:

```cpp
// OLD - missing hash_options
env.le(keylet::account(alice));

// NEW - with proper classification  
env.le(keylet::account(hash_options{env.current()->seq(), KEYLET_ACCOUNT}, alice));

// OLD - sha512Half without context
auto hash = sha512Half(data);

// NEW - with proper classification
auto hash = sha512Half(hash_options{VALIDATOR_LIST_HASH}, data);
```