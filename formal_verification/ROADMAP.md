# Xahau Lean Roadmap

This package should stay focused on invariants that are compact enough to be
reviewable and stable enough to mirror from C++.

Good targets:

1. Threshold arithmetic
   - Tier-2 participant threshold formula
   - quorum threshold relation
   - nUNL original-view anchoring
   - small-network boundary examples
2. Sidecar alignment
   - active-view-only counting
   - quorum-aligned predicate
   - full-observation as diagnostic vs success precondition where applicable
3. Entropy selector
   - non-UNLReport fallback
   - tier ladder from agreed participant count
   - no local pending-state dependency in the tier decision
4. Export gate
   - quorum-aligned success without full observation
   - no deterministic fallback value
   - retry/expire as liveness behavior, not ledger-content substitution

Poor targets for this package:

- direct verification of C++ implementation details
- wall-clock timing and network scheduling liveness
- full ledger execution semantics

Those belong in C++ tests, CSF/testnet scenarios, or a dedicated temporal model.
