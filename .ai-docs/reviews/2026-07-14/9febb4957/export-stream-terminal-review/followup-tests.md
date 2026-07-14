# Export stream terminal follow-up tests

## Scope

Added regression coverage in `src/test/consensus/ConsensusExtensions_test.cpp`
for the LOW test-gap finding in `synthesis.md`:

- A tracked pending-latch origin that is absent from the next validated ledger
  emits one empty `terminal: true` replacement snapshot. The test verifies the
  retained owner/origin identity, validated-ledger cursor, empty share set,
  internal retirement, and suppression of a duplicate terminal snapshot.
- A validated-ledger callback queued behind `exportStreamMutex_` is fenced by
  `stopExportShareService()`. The test verifies that stop cannot return while
  the stream lock is held and that the queued callback emits no snapshot after
  the service flag is cleared.

No production files were changed. Deterministic accepted-edge/terminal lock
winner coverage was not added because there is no test seam between collector
admission and stream-lock acquisition; adding one would require production
instrumentation, while timing-only assertions would be flaky.

## Verification

- `x-format-changed`: passed.
- `x-quick-check`: passed for `ConsensusExtensions_test.cpp`.
- Narrow object build: passed for
  `CMakeFiles/rippled.dir/src/test/consensus/ConsensusExtensions_test.cpp.o`.
- `/tmp/rippled-export-stream-tests --unittest=ripple.consensus.ConsensusExtensions`:
  passed, 1 suite, 66 cases, 9,628 assertions, 0 failures.

The normal incremental `rippled` target was stopped after Ninja scheduled 321
steps from a stale build tree. A temporary binary was linked from existing
objects plus the newly compiled test object to execute the narrow suite without
a broad rebuild.

## Repository state

Work began at requested HEAD `018719fda`. A concurrent commit advanced the
shared branch to `572365baf` (`fix(export): enforce global live latch cap`)
before this follow-up was committed; that unrelated change was preserved.
