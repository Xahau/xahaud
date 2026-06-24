# xahau_consensus

Lean proofs for small Xahau consensus invariants.

This package is intentionally narrow. It does **not** try to verify the C++
implementation directly. It mirrors small formulas and decision ladders from
the consensus-extension code so the safety arguments can be checked as theorems
instead of repeatedly re-derived in review notes.

Current modules:

- `XahauConsensus.Threshold`
  - mirrors `calculateParticipantThreshold`
  - proves the Tier-2 intersection inequality:
    `count + floor(count / 5) < 2 * participantThreshold count`
  - proves the threshold is minimal for that strict inequality
  - proves the original-view threshold remains safe when nUNL shrinks the
    effective view
  - includes the `original=10`, `effective=8` regression example showing why
    using the effective view for the Tier-2 floor is forkable
  - proves `participantThreshold count <= quorumThreshold count` for
    non-empty views
  - distinguishes raw formula helpers from the live safety-wrapped gate
    thresholds used by `ConsensusExtensions`
- `XahauConsensus.ThresholdFacts`
  - records small-network values and band-empty/band-present examples
  - proves exact multiple-of-five behavior
  - proves threshold monotonicity facts
- `XahauConsensus.SixtyPercent`
  - defines a naive `ceil(60%)` threshold
  - proves naive 60% is unsafe at exact multiples of five
  - proves the live derived floor is one higher there and restores strict
    intersection safety
- `XahauConsensus.Intersection`
  - proves the abstract cardinality argument behind quorum intersection
  - shows two threshold-sized cohorts must overlap above the fault bound
    whenever `n + f < 2t`
  - specializes that argument to the live participant threshold, including
    nUNL-shrunk effective views
- `XahauConsensus.HonestOverlap`
  - bridges overlap arithmetic to the consensus claim that two cohorts share at
    least one honest validator
  - specializes that bridge to the participant threshold and `floor(n/5)` fault
    bound
- `XahauConsensus.ViewUniverse`
  - proves original-view anchoring remains safe under nUNL shrink
  - separates strict safety from threshold reachability
  - defines cross-view participant-band presence/absence
  - shows effective-view thresholds can be unsafe against the original fault
    bound
  - shows trusted-superset counting universes erode the intersection margin
- `XahauConsensus.NunlCap`
  - models the protocol's ceil-25% nUNL disablement cap
  - proves 8/6 and 10/8 band collapse examples
  - records that 10 at max cap has effective view 7, below the original
    participant floor
  - records the important counterexample: original `20`, effective `15` does
    **not** make validator quorum meet the original participant floor
- `XahauConsensus.SidecarAlignment`
  - models aligned participant counting for sidecar hashes
  - proves non-active peers and non-active local publication cannot pad the
    alignment count
  - proves changing nonmember reports cannot change quorum alignment
- `XahauConsensus.EntropySelector`
  - models the tier-label ladder from `ConsensusExtensions::selectEntropy`
  - proves non-UNLReport views select fallback
  - proves the quorum / participant / fallback bands select the expected tier
- `XahauConsensus.SelectorDeterminism`
  - models labeled digest output
  - proves digest payload bytes do not affect the label when consensus metadata
    is fixed
  - records examples where changing view provenance or view sizes changes labels
- `XahauConsensus.ExportGate`
  - models export's quorum-aligned success rule
  - models export's outcome as `finalize` or `retryOrExpire`, with no
    deterministic fallback signature set
  - proves missing minority observation does not block a quorum-aligned export
  - proves `fullObservation` alone cannot change the export decision
- `XahauConsensus.ExportQuorum`
  - proves two 80% export quorums overlap above the standard Byzantine bound
    in nonempty active universes
  - proves export quorum overlap remains above the original-view Byzantine
    bound when nUNL shrinkage is within the protocol cap
  - proves Byzantine validators at the standard bound cannot veto quorum
  - records concrete overlap margins for 5/10/20-validator universes
- `XahauConsensus.FinsetIntersection`
  - uses Mathlib finite sets to prove the cardinality premise behind the
    arithmetic intersection theorems
  - specializes that bridge for Tier-2 cohorts, nUNL-shrunk cohorts, and export
    80% quorums
- `XahauConsensus.Invariants`
  - restates cross-module design contracts in one place
  - pins the live safety-wrapped threshold relationship
  - proves the cross-view entropy gate is exactly the selector's non-fallback
    boundary
  - pins non-UNLReport fallback and export full-observation independence

Run:

```sh
~/.elan/bin/lake build
```

## Optional C++ cross-checks

The xahaud CMake build can also compile a Lean-backed unit-test path, but it is
off by default and is not part of normal release builds:

```sh
conan install . --output-folder=build-formal --build=missing \
  -s build_type=Release \
  -o '&:tests=True' \
  -o '&:xrpld=True' \
  -o '&:formal_verification=True'

cmake -S . -B build-formal-cmake \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/build-formal/build/generators/conan_toolchain.cmake \
  -Dtests=ON \
  -Dxrpld=ON \
  -Dformal_verification=ON

cmake --build build-formal-cmake --target rippled
./build-formal-cmake/rippled --unittest=LeanConsensus
```

This path builds `XahauConsensus:static`, links the resulting Lean archive and
runtime into the test binary, and runs a C++ drift test that compares selected
Lean formulas against the production C++ implementations. Today the exported
surface is intentionally small: the Byzantine bound, participant threshold, and
validator quorum threshold.

This is still a model-to-code cross-check, not a proof that the C++ implements
the Lean model. Its value is narrower and practical: if a production threshold
formula changes without the formal model changing too, the gated unit test fails.
