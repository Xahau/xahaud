# Consensus Extension Design Principles

This note captures the principles behind the Xahau consensus extensions:
ConsensusEntropy/RNG, proposal sidecars, and export signature convergence.
Read this before changing `ConsensusExtensions`, `ConsensusExtensionsTick`,
`ExtendedPosition`, sidecar SHAMap handling, or the related CSF tests.

The short version: extension data may coordinate extra same-ledger features,
but it must not redefine ordinary transaction-set consensus. When extension
state cannot be made safe in time, the extension degrades deterministically
and the ledger still closes.

The priority order for consensus extensions is: safe, fast, works. Safety means
extension timing must not create divergent closed-ledger effects when a bounded
coordination step can avoid it. Fast means those coordination steps stay short
and conditional, never becoming an open-ended wait for an extension feature to
succeed. Works means missed or late extension material follows that feature's
deterministic fallback, such as Tier 1 consensus_fallback entropy for RNG or normal
Export retry/expiry, rather than blocking core consensus.

## Fallback Semantics

RNG and Export use similar positive-path sidecar gates, but they do not have
the same safe fallback. RNG closes with a deterministic consensus-bound fallback
digest (Tier 1) in either of two cases: (1) when peers cannot establish an
accepted participant_aligned or validator_quorum entropy set in time, or (2)
whenever the round's active validator view is not UNLReport-backed — no on-ledger
`UNLReport`, e.g. early ledgers or config-trusted dev/test networks — regardless
of how well peers aligned, because a config-derived view can differ between nodes
and yield divergent tier labels for the same entropy set (see Entropy Alignment
Rules). Either way every input to the fallback
(`HashPrefix::entropyFallback`, parent ledger hash, base tx set hash,
sequence) is already consensus-agreed at injection time, so no second
agreement is needed. The result is explicitly labeled
(`EntropyTier = consensus_fallback`, `EntropyCount = 0`) — it is
user-influenceable via transaction submission and must never be presented
under validator-entropy semantics. Export has no equivalent fallback value:
without quorum-aligned verified export signatures, the export must not be
treated as complete and must retry or expire under transaction rules.

## Core Invariants

1. Core consensus remains keyed by the transaction set.

   `ExtendedPosition` has no whole-position equality or implicit `uint256`
   conversion. Callers that need the ordinary consensus identity compare
   `txSetHash` explicitly, or use the generic `positionTxSetID(position)` helper
   in templated consensus code. RNG, export sig, commit-set, and entropy-set
   hashes are proposal sidecars. They are coordinated during establish, but they
   do not define whether peers agree on the ordinary transaction set.

2. Extension waits are bounded.

   RNG and export sidecar convergence may wait briefly inside establish, but
   they must not block ledger close indefinitely. If RNG cannot establish
   an accepted entropy set, it injects the deterministic Tier 1
   consensus_fallback digest (labeled `consensus_fallback`, count 0). If export
   signatures cannot converge, export retries or expires according to
   transaction rules.

3. Safety is in validation; extension logic is deliberation.

   ConsensusEntropy is materialized during accept/buildLCL as a deterministic
   pseudo-transaction. Nodes agree on the base transaction set first, then
   derive the entropy transaction from agreed sidecar inputs. Any local fault
   still has to survive normal validation/LCL agreement.

4. Converge signed inputs, not just derived outputs.

   RNG commits, RNG reveals, and export signatures are the verifiable inputs.
   The design converges on those input sets using sidecar SHAMaps. The final
   entropy digest and export quorum result are derived from the converged
   inputs.

5. Sidecars are not transactions.

   Commit, reveal, and export signature entries are `STObject(sfGeneric)`
   leaves in ephemeral `SHAMapType::SIDECAR` maps. They use `sfSidecarType`
   to distinguish payloads and `HashPrefix::sidecar` for item hashes. They
   are fetched through sidecar sync, not parsed or submitted as transactions.

6. Proposal-visible or validation-visible extension data must be signed.

   Do not attach behavior-changing sidecar payloads as unsigned out-of-band
   proposal or validation wrapper data. If stripping or changing a field would
   alter RNG or export behavior, that field must be covered by the relevant
   signed payload and by the identity used for duplicate suppression/replay
   checks on that path.

   Today ConsensusExtensions uses signed proposal sidecars, not validation
   sidecars. If a future design carries extension material through
   validations, the same rule applies: the behavior-changing data, or a digest
   of it, must be inside the signed validation payload and bound to the
   validating key and ledger. A protobuf field outside the signed validation is
   only transport metadata; it must not affect consensus-extension behavior.

## Validator Set And Quorum

The active validator view is the shared denominator for RNG and export:

- Prefer `UNLReport.sfActiveValidators` from the consensus parent ledger.
- If no report is available, fall back to configured trusted validators so
  early ledgers and dev/test networks can make progress. This configured
  fallback view is `!fromUNLReport`: a non-standalone node mints only
  `consensus_fallback` entropy under it and must not finalize Export, because a
  config-derived view can diverge between nodes.
- If `featureNegativeUNL` is enabled, subtract the parent ledger's Negative
  UNL from whichever source produced the view.
- Use the same snapshot throughout the round.

`quorumThreshold()` is 80% of that active validator view. Recent or expected
proposers are liveness hints only; they do not shrink the quorum denominator.

Be careful with `prevProposers`: in the generic consensus code it is peer-only.
When checking whether the previous round had enough active participants, count
our own proposer slot if this node is proposing.

## Participant Diagnostics

Proposals may carry a signed `observedParticipantsHash` for debugging active-UNL
visibility during RNG/Export rounds. The hash represents the active validators
this node has observed participating in the current establish round, including
self when proposing. It also commits to the active validator view used by the
node, so mismatched fallback/configured views do not collapse to the same
diagnostic hash merely because they have the same size.

Local diagnostics also log the same observed set as a canonical binary bitmap
over the sorted active validator view. The bitmap is not sent on the wire; the
signed proposal field remains the hash.

This field is diagnostic only:

- It is covered by the proposal signature and duplicate-suppression identity.
- It does not participate in core tx-set identity.
- It does not lower the active validator quorum denominator.
- It is intended to explain timing/degraded-network cases where commits,
  reveals, or sidecar hashes arrive late or asymmetrically.

## RNG Commit/Reveal Principles

RNG proceeds through establish sub-states:

1. `ConvergingTx`: ordinary transaction-set convergence while harvesting
   commitments.
2. `ConvergingCommit`: after proofed commit quorum, publish the commit sidecar
   hash and reveal the same secret that produced the original commitment.
3. `ConvergingReveal`: collect reveals, publish the entropy sidecar hash, and
   wait for sidecar agreement or deterministic fallback.

These sub-states are sequential gates, not sequential collection phases.
Commitments ride on the initial proposal and are usually already harvested
during `ConvergingTx`; in a healthy round `ConvergingCommit` is often just a
one-tick commit-set publication/checkpoint before reveals begin. The bounded
waits there are for late quorum or conflicting commit-set hashes, not the normal
commit transport path.

Commit quorum counts only proofed commits from active validators. A commit that
cannot be emitted as a verifiable sidecar leaf does not count.

Reveal collection targets all known committers, because the commit sidecar set
defines who is expected to reveal. The reveal wait is still bounded. A node
that crashes, withholds, or partitions after committing must not stop the
ledger forever.

Final entropy is computed from the agreed entropy sidecar SHAMap, not from a
node's opportunistic local `pendingReveals_` map. This prevents different
local reveal subsets at timeout boundaries from producing different entropy.
The accepted-hash latch is the ledger-material boundary: a node that misses the
bounded observation window falls back instead of injecting from a local candidate
map. That does not make accept-vs-timeout decisions global; it makes any
non-fallback injection depend only on the sidecar root that this node accepted,
with ordinary validation quorum resolving boundary timing.

## Entropy Alignment Rules

Non-fallback entropy tiers require a UNLReport-anchored active view. A
non-standalone node mints only `consensus_fallback` (Tier 1) when the round's
active view is not built from an on-ledger `UNLReport` (`!fromUNLReport`),
regardless of how well peers aligned, because a config-derived view can differ
between nodes and yield divergent tier labels for the same set. (Standalone/dev
nodes are a separate exception that synthesize validator_quorum entropy.)
Everything below assumes a UNLReport-anchored view.

Two distinct counts are involved; keep them separate.

The **alignment (gate) count** decides whether the round proceeds with the agreed
entropy set or falls back. It is taken over the active validator view:

```
(our published entropySetHash, counted only if this node is itself an active validator)
  + active-view, tx-converged peers advertising the same entropySetHash
```

Trusted-but-non-active proposers are NOT counted even when tx-converged and
aligned (peers are filtered through the active view via `isUNLReportMember`), and
a non-active local node does not add its own +1 (gated on
`localIsActiveValidator()`). This mirrors the `buildEntropySet`/`hasQuorumOfCommits`
membership filter and keeps the counting universe from inflating above the
active-view size, preserving the Tier-2 intersection margin (`2t - n`) and
equivocation uniqueness. The round proceeds when this count reaches
`entropyGateThreshold() = min(quorumThreshold(), tier2Threshold())` and the node
has full observation of the tx-converged active set; otherwise it falls back at
the bounded deadline.

The **tier label** is then derived from the agreed entropy set itself — the
number of validator reveals (leaves) in the agreed `entropySetMap_`, not the
peer-alignment count above. If that leaf count reaches `quorumThreshold()`, the
set is labeled `validator_quorum`; if it is below `quorumThreshold()` but reaches
`tier2Threshold()`, it is labeled `participant_aligned`; otherwise the round falls
back. `quorumThreshold()` is 80% of the effective active view; `tier2Threshold()`
is the intersection-safe floor over the original pre-nUNL view.

Under nUNL, exact integer thresholds can cross either way. For example, a
20-validator original view with five disabled validators has effective quorum
12 but participant threshold 13. That only changes which threshold dominates
the proceed gate; the final tier label is still derived from the agreed entropy
set count by the ladder above.

In both label cases, a below-threshold minority can advertise a conflicting or
unacquirable entropy hash without vetoing the aligned cohort.

If no entropy hash reaches the entropy gate threshold before the bounded
deadline, the round must fall back to the Tier 1 consensus-bound digest. This
is the safe degradation path, not a consensus failure.

Examples with six active validators on a UNLReport-anchored view (validator_quorum
threshold five, participant_aligned threshold four; six is the smallest view with
a non-empty Tier 2 band and non-zero tolerated Byzantine count; at five validators
quorum and participant_aligned coincide at four, leaving no band). On a
non-UNLReport (config-fallback) view, every case below instead mints
`consensus_fallback`:

- Five honest validators align on one entropy hash and one validator advertises
  a bogus hash: proceed with validator_quorum entropy for the honest quorum.
- Four validators align on the honest hash and two advertise different bogus
  hashes: proceed with participant_aligned (Tier 2) entropy — the aligned
  cohort is below the 80% quorum but at or above the intersection-safe floor,
  and its overlap (2*4 - 6 = 2 > floor(6/5) = 1) still shares an honest
  validator between any two such cohorts.
- Three validators align on the honest hash and three fail to align: fall back
  to the Tier 1 digest.
- No peer entropy hash is observed in time: fall back to the Tier 1 digest.

The fallback pseudo-transaction is deterministic — every node derives the same
digest from `(HashPrefix::entropyFallback, parentLedgerHash, baseTxSetHash,
seq)` — and labeled with `EntropyTier = consensus_fallback` and
`EntropyCount = 0`. Hooks state their own requirements via the required
`min_tier`/`min_count` arguments to `dice()`/`random()`: a hook that demands
validator-tier entropy fails closed with `TOO_LITTLE_ENTROPY` on fallback
ledgers, while a hook that opts into fallback-grade randomness must do so
explicitly at the call site. Valid `min_tier` values are the stored entropy
tiers 1..3; `min_count` must fit the on-ledger `EntropyCount` UINT16 field.
Invalid requirements return `INVALID_ARGUMENT`, while valid-but-unmet
requirements return `TOO_LITTLE_ENTROPY`.

Open-ledger hook execution is provisional. During speculative open-ledger
execution, `dice()`/`random()` can only use the previous ledger's finalized
entropy; final buildLCL execution sees the current ledger's entropy pseudo-tx
after it updates the SLE. Hooks that need final entropy must treat open-ledger
RNG results as previews.

The fallback digest derives from the BASE (pre-injection) tx set hash to avoid
circularity, and entropy pseudo-tx deduplication is value-based: if the agreed
set already contains the exact pseudo-tx, injection skips it; a
present-but-different pseudo-tx is logged as a determinism violation and left in
the agreed set.

## Sidecar Convergence Rules

Sidecar SHAMaps use union convergence:

- Every valid active-validator contribution belongs in the set.
- Sets only grow during fetch/merge.
- Fetch/merge is a safety net for missed proposals, not the normal transport.
- Rebuild and republish the sidecar hash after merging missing leaves.

Do not use avalanche-style transaction inclusion logic for sidecar inputs.
For RNG and export sidecars, the disagreement to resolve is usually timing or
delivery, not whether a valid contribution should be included.

The entropy sidecar gate always gives peers at least one observation tick after
publishing `entropySetHash`. Publishing and accepting in the same tick can hide
conflicts and produce asymmetric zero/non-zero outcomes.

## Export Principles

`featureExport` and `featureConsensusEntropy` are independently amendment
gated.

Export can run without ConsensusEntropy and still uses the active validator
view's 80% quorum threshold. Verified export signature sidecars converge
through `ExtendedPosition`, and the `exportSigSetHash` is signed by proposals
whether or not RNG is enabled. Do not make Export liveness depend on unanimity:
one active validator with a missing, delayed, or conflicting sidecar must not
veto an otherwise quorum-aligned export round.

Non-standalone Export completion requires a UNLReport-backed active validator
view. If the parent ledger has no `UNLReport`, Export has no safe deterministic
fallback result, so it retries or expires rather than finalizing against local
trusted-configuration thresholds.

The extended proposal machinery is enabled when either feature needs signed
sidecar fields. Do not make Export depend on RNG availability just because RNG
was the first consumer of `ExtendedPosition`.

When `featureExport` is disabled, the export sidecar gate is disabled too. Stale
collector entries must not keep a stopped amendment active.

Only verified export signatures count toward quorum or enter export sidecar
SHAMaps. Proposal-ingress signatures are sender-bound to the trusted proposal
validator and may be stored as unverified until the matching export transaction
is available for cryptographic verification.

The consensus candidate transaction set is the authority for export signature
verification. The open ledger may be used for early proposal ingestion, but
once a candidate tx set exists, only signatures verified against the `ttEXPORT`
in that candidate set may become quorum material or enter `exportSigSetHash`.

Export sidecar publication is local-material only. A node may publish only the
verified export signatures it actually has locally, and only for `ttEXPORT`
transactions in the consensus candidate set. A fetched export sidecar is not a
separate apply input: on merge, each leaf must be active-view checked, verified
against the candidate transaction, and promoted into `ExportSigCollector`.
Closed-ledger apply must assemble the signer set from the agreed
`exportSigSetHash` sidecar map, not from the still-growing live collector, so
late local arrivals cannot change the ledger-defining export result.

If the consensus candidate contains a `ttEXPORT` but the node has no eligible
local export signatures yet, the export sidecar gate opens only a bounded
safety window for tx-converged peers to advertise `exportSigSetHash`. This is
not a wait-for-Export-success mechanism; it is a short opportunity to avoid
closing a minority ledger while sidecar convergence is already reachable. If no
advertised sidecar appears by the deadline, the gate stops waiting and the
export retries or expires through normal transaction rules.

Export success requires quorum alignment on `exportSigSetHash`, not merely a
local collector quorum. Since `featureExport` enables signed extended proposal
fields, a quorum-aligned `exportSigSetHash` is enough to proceed even if a
tx-converged minority peer has not advertised an export sidecar hash. Do not let
one active validator with a missing sidecar force an otherwise quorum-aligned
export round to retry or expire. Full observation remains useful diagnostics; it
is not an Export success precondition. If no export signature hash reaches quorum
alignment by the bounded deadline, do not choose the largest non-quorum set; the
export retries or expires according to normal transaction rules.
Closed-ledger apply consumes only the accepted `exportSigSetHash` root. A node
that times out before accepting a root retries; a node that proceeds assembles
from the accepted sidecar map, never from its live collector. This avoids
successful-but-different export blobs while preserving the bounded wait model.

Closed-ledger apply must not promote unverified proposal-carried signatures into
current-round quorum material. It may verify and retain them for a future retry,
where they can be published in a sidecar set and converged before use.

Export sig convergence runs in parallel with RNG. An export-side convergence
failure must not change RNG semantics; an RNG fallback must not make export
unsafe. Each feature has its own gate and fallback.

Accept-time cleanup must preserve Export state through `buildLCL` whenever
`featureExport` is enabled. RNG-disabled does not mean extensions-disabled:
`ttEXPORT` still needs the round's export sidecar convergence state when it
applies.

CSF consensus tests model the export sidecar gate directly. Testnet scenarios
under `.testnet/scenarios/export/` cover live-node Export+CE behavior and
Export-only quorum behavior.

## Review Checklist

When changing consensus extension code, check these questions:

- Does this preserve transaction-set identity as the core consensus identity?
- Does every extension wait have a bounded fallback?
- Does validator_quorum entropy require active-validator quorum alignment?
- Can one bad validator deny entropy to an honest quorum? It must not.
- Can a sub-quorum set produce participant_aligned entropy only after reaching
  the intersection-safe tier2Threshold()?
- Are quorum calculations using the active validator view, not recent
  proposers as the denominator?
- Do non-fallback tier labels (validator_quorum / participant_aligned) require a
  UNLReport-anchored active view, falling back to `consensus_fallback` when the
  view is config-derived (`!fromUNLReport`)?
- Is the alignment-count *universe* itself — not just the quorum denominator —
  the active validator view (non-active proposers and a non-active local node
  excluded from the count)?
- Are sidecar entries typed as sidecars, not pseudo-transactions?
- Are proposal-visible or validation-visible sidecar fields covered by the
  relevant signature and duplicate/replay identity?
- Are export signatures verified before they count?
- Does export success require `exportSigSetHash` alignment, not just local
  collector quorum?
- Can one bad validator deny Export to an honest quorum? It must not.
- Can timeout select a largest-but-below-quorum export sidecar set? It must not.
- Are CE and Export still independently gated and independently stoppable?
