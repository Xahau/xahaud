# Consensus Entropy — Design Intent (canonical spine)

This is the **normative** intent for `featureConsensusEntropy`: the invariants that
must hold regardless of how the implementation is refactored. It is deliberately
short. The verbose mechanics live in `ConsensusExtensionsDesign.md`; the
reviewer-facing walkthrough lives in the PR description. **Both defer to this
file.**

How to use it: if code contradicts an invariant below, the *code* is wrong — or
the invariant is being changed and **this file must be consciously edited in the
same change, with the rationale**. A plausible-sounding comment added next to
drifted code is not a design decision. (This document exists because the intent
once lived only in a maintainer's head; an agent tightened past it and wrote a
rationale that made the drift look deliberate. Git, not the docs, preserved the
truth. Don't rely on that twice.)

## Purpose (one line)

Same-ledger consensus randomness for hooks — entropy finalized *after* user intent
is locked but *before* normal execution — with **bounded, labeled** manipulation
and **graceful degradation**, and **without ever weakening base-consensus
determinism or liveness.**

## Invariants

**INV-1 — Determinism of the injected object.**
Given the same parent ledger and the same *agreed* entropy sidecar, every honest
node injects the byte-identical `ttCONSENSUS_ENTROPY` (digest, tier, count). That
object is ledger state. Therefore **no injection input may be node-local mutable
or timing-derived state.** The selector derives `(digest, tier, count)` only from
the agreed `entropySetMap_` (matched to the hash the gate accepted) plus the
parent-ledger active view.
*Enforced:* `selectEntropy` + the `acceptedEntropySetHash_` gate. *Anti-pattern:*
reading a local `entropyFailed_`/timeout flag at injection time (this was the H2
bug).

**INV-1A — Accept-vs-fallback is also ledger-defining.**
The choice between a non-fallback sidecar and `consensus_fallback` is part of the
same injected object. It must be tied to the accepted sidecar-hash discipline and
the proofed/quorum sidecar material, not to one node's local observation counts.
Local signals such as previous proposers, currently visible peer positions, or
"quorum seems impossible from here" may influence logging, diagnostics, and
bounded waits, but they must not short-circuit the gate while enough proofed
sidecar material exists to continue toward non-fallback entropy.
*Enforced:* the same accepted-hash boundary as INV-1, plus tests that compare
nodes with asymmetric local observation. *Anti-pattern:* a bootstrap or
"impossible quorum" shortcut that falls through to close with fallback from
`prevProposers` or visible `peerPositions` while the proofed commit set already
meets the entropy gate.

**INV-2 — No single validator can veto.**
Entropy mints on **quorum, not unanimity**. A minority withholding reveals or
sidecar-hash advertisements must not, by silence alone, force fallback or stall
while the remaining fixed-denominator cohort still reaches the entropy gate. On
timeout the round uses the revealed/quorum-aligned set it has when that set still
meets the gate; it downgrades only when the remaining set is below threshold or
conflicted/unresolved.
*Enforced:* the clean (no-conflict) gate path accepts on `quorumAligned()` alone.
*Anti-pattern:* requiring `peersSeen == txConverged` (full observation) on the
clean path (this was the M6 over-correction).

**INV-3 — Anchored denominator.**
All thresholds are computed over the **fixed parent-ledger UNLReport active-view
size** (tier-2 over the *original* pre-NegativeUNL size). No node-local
observation may grow or shrink that denominator `N`. This is load-bearing for
tier-2 equivocation-uniqueness (`2t − N > f`).
*Enforced:* `quorumThreshold` / `tier2Threshold` over `activeValidatorView`; the
alignment-counting universe is filtered to the active view. *Anti-pattern:*
counting "valid/observed proposals" as the denominator — that lets a withholder
shrink `N` and is also node-local (split).

**INV-4 — Quorum alignment is the conflict boundary.**
Equivocation (a peer showing different hashes to different peers) must not let
two sidecar hashes both become ledger material. The fixed-denominator entropy
threshold is sized so any two quorum-aligned cohorts intersect above the
Byzantine floor, so at most one entropy hash can be quorum-aligned. Once our
hash reaches that gate, a below-threshold conflicting minority or silent peer
must not force fallback by withholding full observation; ordinary validation
resolves the bounded deadline edge.
*Enforced:* both clean and conflicting entropy-hash gates proceed on
`quorumAligned()`; conflicting states below that threshold wait only for the
bounded deadline. *Anti-pattern:* requiring `fullObservation()` before ignoring a
below-quorum conflict, which lets a minority equivocation recreate a veto.

**INV-5 — Graceful, labeled, deterministic degradation.**
Under no-UNLReport / lost reveals / failed alignment / timeout / impossible
quorum, the round mints an **explicitly labeled lower tier**, never an unlabeled
or non-deterministic value. The tier-1 fallback is a pure function of
*already-agreed* inputs: `H(entropyFallback, parentLedgerHash, baseTxSetHash,
seq)` — and must **never** depend on the post-injection tx set (no circular
dependency on the set that carries the pseudo-tx).
*Enforced:* `selectEntropy` fallback path; `baseTxSetHash` is the pre-injection
set hash.

**INV-6 — Bounded, opt-in entropy quality.**
Hooks state `min_tier` / `min_count` explicitly (no hidden network default).
Entropy is served iff it is **fresh** (current or previous ledger) **and** meets
the requirement; otherwise the call **fails closed** (`TOO_LITTLE_ENTROPY`). A
hook never silently receives weaker-than-requested entropy. Draws are also
domain-separated by the hook execution role that can share a transaction and
hook hash: strong vs weak, callback vs direct dispatch, and hook chain
position.
*Enforced:* `fairRng` gate.

**INV-7 — Inert when un-amended.**
With `featureConsensusEntropy` off, no RNG sidecar state is consensus-visible and
proposal bytes remain byte-identical to base XRPL.
*Enforced:* per-round enable latch snapshotted from the *parent ledger's* rules;
`ExtendedPosition` serializes to exactly the legacy 32-byte tx-set hash when no
sidecar fields are set.

**INV-8 — No unbounded liveness dependency.**
Every sub-state has a bounded timeout with a deterministic downgrade. CE must
never be the reason a round stalls once base consensus is itself making progress.
*Enforced:* bounded reveal/entropy deadlines → fallback.

## Known residuals (by design — not bugs)

These are deliberate properties, documented so a future reader doesn't "fix" them
into an INV violation:

- **Commit/reveal withholding bias** of up to one bit per withholder exists on
  **all** tiers, not just fallback; colluding withholders near a threshold can
  force a downgrade. It is **bounded and labeled, not eliminated.** True
  unbiasability would require a VRF / threshold-BLS construction (out of scope).
  The accountability lever for *persistent* withholding is validator scoring /
  NegativeUNL, **not** weakening any gate above (that would violate INV-2..INV-4).
- **Fallback (tier 1) is user-influenceable** (a quiet-ledger submitter can grind
  the tx set). That is why it is a distinct labeled tier hooks must opt into, and
  never suitable for value-bearing outcomes.
- **Provisional open-ledger entropy** differs from the closed-ledger value
  (speculative execution sees the previous ledger's entropy in the open ledger,
  the current ledger's at close). Open-ledger `dice()`/`random()` are previews,
  not the authority.
- **Bounded accept-vs-fallback timing asymmetry** remains possible at the edge of
  observation deadlines: one node may see a quorum-aligned entropy sidecar before
  its deadline while another times out to `consensus_fallback`. That is a
  validation-backstopped liveness/resync residual of doing sidecar agreement
  outside the base transaction-set hash, not permission for additional local
  shortcut gates. Removing it entirely would require making the fallback/accept
  decision itself an agreed consensus object.
