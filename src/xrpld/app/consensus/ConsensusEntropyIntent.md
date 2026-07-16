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
node injects the byte-identical `ttCONSENSUS_ENTROPY` (digest, tier, count,
denominator, contributors). That
object is ledger state. Therefore **non-fallback entropy must not read mutable
local collector state or timing-derived state.** The selector derives non-fallback
`(digest, tier, count, denominator, contributors)` only from the accepted
`entropySetMap_` (matched to the hash the gate accepted) plus the parent-ledger
active view. The contributor bitmap is ordered by the canonical
parent-ledger active-validator view; it must not resolve signing keys through
live manifests or any mutable local cache at injection time. The transaction
ordering salt intentionally uses the digest/tier/count/denominator tuple, not
the contributor bitmap. This is semantic separation, not a downgrade in
consensus risk: once the bitmap is written into the pseudo-transaction, any
disagreement on it is already a ledger-byte disagreement. The salt uses the
entropy value and quality labels; the bitmap remains the accountability label.
Local timeout or diagnostic state such as `entropyFailed_` must not override an
accepted root at injection time; a node that never accepts a root falls back
through the normal missing-accepted-root path.
Apply rejects a `ttCONSENSUS_ENTROPY` whose `sfLedgerSequence` does not equal
the ledger being built; persisted metadata therefore cannot claim a different
source ledger than the transaction that wrote it.
*Enforced:* `selectEntropy`, the `acceptedEntropySetHash_` gate, and the
accepted-root authority test. *Anti-pattern:* reading live
`pendingReveals_`/collector state at injection time, re-evaluating contributor
identity through live manifests, or letting local timeout flags override an
accepted root.

**INV-1A — Accept-vs-fallback is also ledger-defining.**
The choice between a non-fallback sidecar and `consensus_fallback` is part of the
same injected object. It must be tied to the accepted sidecar-hash discipline and
the proofed/quorum sidecar material, not to one node's local observation counts.
Local signals such as previous proposers, currently visible peer positions, or
"quorum seems impossible from here" may influence logging, diagnostics, and
bounded waits, but they must not short-circuit the gate while enough proofed
sidecar material exists to continue toward non-fallback entropy.
A node that cannot accept the entropy root by the bounded deadline may inject
`consensus_fallback` while the aligned quorum injects validator entropy. This is
an accepted, validation-resolved lagging-node close result, not a selector
determinism defect: an accepted root must win, and absence of an accepted root
falls back. This residual can occur even when the node otherwise agreed on the
pre-injection transaction set: CE is appended after base transaction-set
consensus, so missing CE proposal material is its own close-time boundary.
This is a theoretical/reproduced-in-lab boundary, not a behavior observed on
healthy testnets. CE reveal material rides the same proposal messages as the
base transaction-set positions, so a node healthy enough to align on the tx set
is expected to receive the CE material within the bounded CE window. Reaching
this state requires a persistent, specific CE-material miss, or a stall at the
CE sub-state boundary, after base tx-set agreement.
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
`featureNegativeUNLActiveViewCap` should be active before or with CE on networks
that use NegativeUNL: it makes producer-side nUNL disable voting use the same
parent-ledger UNLReport denominator that this invariant relies on. The
consumer-side active-view builder still caps raw ledger NegativeUNL subtraction
defensively against `originalViewSize`.
*Enforced:* `quorumThreshold` / `tier2Threshold` over `activeValidatorView`; the
alignment-counting universe is filtered to the active view; amended
`NegativeUNLVote` uses the same UNLReport active count for its disable cap when
available. *Anti-pattern:*
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
*already-agreed* inputs: `H(entropyFallback, parentLedgerHash, buildTxSetHash,
seq)` — and must **never** depend on the post-injection tx set (no circular
dependency on the set that carries the pseudo-tx). `buildTxSetHash` is the raw
agreed set after removing only supplied ConsensusEntropy and Export synthetic
transactions; legacy protocol pseudos remain included.
*Enforced:* `makeLiveBuildTxSet` before ordering and `selectEntropy` fallback;
the original consensus-set hash remains separate bookkeeping.

Live transaction-set membership grants no authority to write extension state.
Live-set preparation removes every supplied `ttCONSENSUS_ENTROPY` and
`ttEXPORT_SIGNATURES` before computing the build-set hash or ordering salt, and
`onPreBuild` removes them again before local derivation. Only accepted extension
evidence may synthesize the live extension stream. Historical replay follows
the opposite rule: it consumes persisted ordered bytes and never sanitizes or
re-derives them.

**INV-6 — Bounded, opt-in entropy quality.**
Hooks state `min_tier` explicitly on every draw (no hidden network default).
Entropy is served iff it is **fresh** (current or previous ledger) **and** meets
that class floor; otherwise the call **fails closed**
(`TOO_LITTLE_ENTROPY`). `entropy_status()` separately exposes the stored tier,
contributor count, and denominator so hooks can impose proportional or absolute
policies without freezing those policies into the host ABI.
Fallback is tier 1 with count/denominator `0/0`, so callers must classify tier
before arithmetic. The `validator_full` label is structurally valid only when
`EntropyCount == EntropyDenominator`; the contributor bitmap independently must
have exactly that population over the denominator-sized view. Draws are also
domain-separated by the hook execution role that can share a transaction and
hook hash: strong vs weak, callback vs direct dispatch, and hook chain position.
*Enforced:* `fairRng` tier/freshness gate and metadata-only `entropy_status`.

**INV-7 — Inert when un-amended.**
With `featureConsensusEntropy` off, no RNG sidecar state is consensus-visible and
CE itself adds no proposal bytes. Export may independently use the same extended
proposal envelope when `featureExport` is active.
*Enforced:* the CE per-round enable latch is snapshotted from the *parent
ledger's* rules; `ExtendedPosition` serializes to exactly the legacy 32-byte
tx-set hash only when neither feature has populated a sidecar field.

**Rollout note:** enabling `featureConsensusEntropy` or `featureExport` switches
the network to extension-aware proposal semantics. An individual proposal with
no populated sidecar fields still serializes to the legacy 32-byte tx-set hash,
but live proposals may instead carry a serialized `ExtendedPosition` in the
legacy `currenttxhash` protobuf field. This is a proposal wire-format dependency,
not a sidecar-fetch dependency. Older binaries that only accept a 32-byte
`currenttxhash` are not compatible proposal participants after activation;
operators must upgrade the proposal-processing network first, or add explicit
version/capability negotiation before attempting a heterogeneous rollout.

**INV-8 — No unbounded liveness dependency.**
Every sub-state has a bounded timeout with a deterministic downgrade. CE must
never be the reason a round stalls once base consensus is itself making progress.
*Enforced:* bounded reveal/entropy deadlines → fallback.

**INV-9 — Live construction owns cardinality and first application.**
When the parent-rule latch enables CE, live construction contains exactly one
locally derived `ttCONSENSUS_ENTROPY`; when disabled it contains none. The
selector always yields a digest, using `consensus_fallback` when necessary, so
an enabled live build never skips injection. Re-derivation is reconstructive,
not additive: supplied or previously derived extension pseudos are removed
before the one canonical transaction is inserted with zero Account, Sequence,
and Fee and `sfLedgerSequence` equal to the ledger being built. `BuildLedger`
and `applyConsensusEntropy` are not duplicate detectors; the exactly-one
guarantee belongs to live construction.

In a live build, that sole pseudo is attempted once through the evolving view
before every ordinary transaction. On success it writes the singleton with the
current ledger sequence, and later Hook execution in that build observes the
new value. If the first application fails, the builder records the failure and
continues ordinary execution; the Hook freshness policy may then expose the
previous-ledger snapshot. This degradation is explicit and must not silently
become either fail-closed ledger construction or an unordered ordinary apply.

Replay does not select entropy, recompute its salt, sanitize the persisted set,
inject a replacement, or impose live first-application ordering. It applies the
recorded transaction-index order so the closed ledger's bytes and state are
reproduced exactly.

## Known residuals (by design — not bugs)

These are deliberate properties, documented so a future reader doesn't "fix" them
into an INV violation:

- **Commit/reveal withholding bias** of up to one bit per withholder applies to
  accepted non-full reveal sets: a withholder can choose whether its contribution
  is included, and colluding withholders near a threshold can force a downgrade
  or fallback. `validator_full` removes that in-vs-out slack within a successful
  tier-4 result: every active validator contributed, although a withholder can
  still force downgrade or make a tier-4-requiring hook fail. The behavior is
  **bounded and labeled, not eliminated.** True unbiasability would require a VRF
  / threshold-BLS construction (out of scope). The accountability lever for
  *persistent* withholding is validator scoring / NegativeUNL, **not** weakening
  any gate above (that would violate INV-2..INV-4).
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
