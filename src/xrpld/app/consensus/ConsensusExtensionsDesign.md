# Consensus Extension Design Principles

This note captures the principles behind the Xahau consensus extension
framework (signed proposal sidecars, local sidecar snapshots, bounded gates,
and a transaction-stream replay witness, which for RNG is the
`ttCONSENSUS_ENTROPY` pseudo-transaction) and its one extension,
ConsensusEntropy/RNG. Read this before changing `ConsensusExtensions`,
`ConsensusExtensionsTick`, `ExtendedPosition`, sidecar SHAMap handling, or the
related CSF tests.

The short version: extension data may coordinate extra ledger-visible features,
but it must not redefine ordinary transaction-set consensus. When extension
state cannot be made safe in time, the extension degrades deterministically
and the ledger still closes.

The priority order for consensus extensions is: safe, fast, works. Safety means
extension timing must not create divergent closed-ledger effects when a bounded
coordination step can avoid it. Fast means those coordination steps stay short
and conditional, never becoming an open-ended wait for an extension feature to
succeed. Works means missed or late extension material follows that feature's
deterministic fallback, such as Tier 1 consensus_fallback entropy for RNG,
rather than blocking core consensus.

## Fallback Semantics

RNG closes with a deterministic consensus-bound fallback digest (Tier 1) in
either of two cases: (1) when the round does not accept an entropy set that
earns a non-fallback tier (participant_aligned or better) in time, or (2)
whenever the round's active validator view is not UNLReport-backed — no on-ledger
`UNLReport`, e.g. early ledgers or config-trusted non-standalone test networks —
regardless of how well peers aligned, because a config-derived view can differ
between nodes and yield divergent tier labels for the same entropy set (see
Entropy Alignment Rules). Either way every input to the fallback
(`HashPrefix::entropyFallback`, parent ledger hash, base tx set hash,
sequence) is already consensus-agreed at injection time, so no second
agreement is needed. The result is explicitly labeled
(`EntropyTier = consensus_fallback`, `EntropyCount = 0`) — it is
user-influenceable via transaction submission and must never be presented
under validator-entropy semantics.

The fallback/non-fallback decision is itself ledger-defining. A local node may
diagnose that progress looks unlikely from its current peer view, but it must
not close with fallback solely because local previous-proposer or peer-position
counts are low while proofed sidecar material already meets the entropy gate.
Local observations can bound waits; they cannot bypass available quorum
material.

There is still a bounded consensus-edge residual: one node may observe a
quorum-aligned sidecar before its deadline while another times out and builds
the fallback pseudo. Validation resolves that as ordinary ledger disagreement.
Eliminating it entirely would require making the accept/fallback decision itself
part of the agreed consensus object. The rule here is therefore narrower and
enforceable: no additional node-local shortcut may create fallback before the
sidecar gate has had its bounded chance to use proofed/quorum material.

## Core Invariants

1. Core consensus remains keyed by the transaction set.

   `ExtendedPosition` has no whole-position equality or implicit `uint256`
   conversion. Callers that need the ordinary consensus identity compare
   `txSetHash` explicitly, or use the generic `positionTxSetID(position)` helper
   in templated consensus code. The commit-set and entropy-set hashes and the
   per-validator commitment and reveal leaves are proposal sidecars. They are
   coordinated during establish, but they do not define whether peers agree on
   the ordinary transaction set.

2. Extension waits are bounded.

   Sidecar convergence may wait briefly inside establish, but it must not block
   ledger close indefinitely; every extension needs a deterministic outcome for
   a missed gate. If RNG cannot establish an accepted entropy set, it injects
   the deterministic Tier 1 consensus_fallback digest (labeled
   `consensus_fallback`, count 0).

   The bounded fallback rule is not permission for local shortcuts to decide
   ledger output. "Cannot establish" means the accepted-hash gate did not
   produce usable material before its bounded deadline; it does not mean one
   node locally under-observed proposers or peers while proofed/quorum material
   was already available.

3. Safety is in validation; extension logic is deliberation.

   ConsensusEntropy is materialized during accept/buildLCL as a deterministic
   pseudo-transaction. Nodes agree on the base transaction set first, then
   derive the entropy transaction from agreed sidecar inputs. Any local fault
   still has to survive normal validation/LCL agreement.

4. Align signed inputs, not just derived outputs.

   RNG commits and RNG reveals are the verifiable inputs. The design aligns on
   signed roots over those input sets using local sidecar SHAMap snapshots. The
   final entropy digest is derived from the accepted local snapshot, not from
   live collector state.

5. Sidecars are not transactions.

   Commit and reveal entries are `STObject(sfGeneric)` leaves in ephemeral
   `SHAMapType::SIDECAR` maps. They use `sfSidecarType` to distinguish
   payloads and `HashPrefix::sidecar` for item hashes. The maps are local
   immutable snapshots; they are never advertised, served, or acquired from
   peers. A node uses them to materialize the root it signed into its proposal
   and, if that root is accepted, to build the ledger-visible pseudo/witness.

6. Proposal-visible or validation-visible extension data must be signed.

   Do not attach behavior-changing sidecar payloads as unsigned out-of-band
   proposal or validation wrapper data. If stripping or changing a field would
   alter extension behavior, that field must be covered by the relevant
   signed payload and by the identity used for duplicate suppression/replay
   checks on that path.

   ConsensusExtensions uses signed proposal sidecars, not validation
   sidecars. If a future design carries extension material through
   validations, the same rule applies: the behavior-changing data, or a digest
   of it, must be inside the signed validation payload and bound to the
   validating key and ledger. A protobuf field outside the signed validation is
   only transport metadata; it must not affect consensus-extension behavior.

7. Proposal-carried material is untrusted until semantic validation passes.

   A sidecar root proves only byte identity for the local snapshot that produced
   it. It does not prove that a contribution is well-formed, authorized, or
   round-correct. Proposal-carried commits and reveals must pass cheap
   structural checks, safe key-type checks, active-view membership, and the
   relevant cryptographic proof before entering pending RNG state. Cluster
   trust may affect relay and resource policy, but extension sidecars become
   ledger inputs and must be harvested only after the signed proposal verifies
   against the claimed validator key.

8. Ledger-defining sidecar material crosses apply as transaction-stream input.

   Sidecars are an establish-phase convergence mechanism, not a ledger replay
   input. If sidecar material changes a closed-ledger effect that cannot be
   recomputed from the parent ledger and ordered transaction set, the accepted
   material must first be represented by a deterministic pseudo transaction (or
   an equivalent transaction-stream witness). Apply/replay then consumes that
   witness and re-validates it; it must not depend on ephemeral sidecar memory.

## Validator Set And Quorum

The active validator view is the fixed denominator for RNG sidecar alignment
and entropy tiers:

- Prefer `UNLReport.sfActiveValidators` from the consensus parent ledger.
- If no report is available, fall back to configured trusted validators so
  early ledgers and dev/test networks can make progress. This configured
  fallback view is `!fromUNLReport`: a non-standalone node mints only
  `consensus_fallback` entropy under it, because a config-derived view can
  diverge between nodes.
- If `featureNegativeUNL` is enabled, subtract the parent ledger's Negative
  UNL from whichever source produced the view.
- Use the same snapshot throughout the round.

When `featureConsensusEntropy` is enabled in the parent ledger's rules,
NegativeUNL vote production also uses the parent-ledger
`UNLReport.sfActiveValidators` count as the 25-percent disable-cap denominator.
The count includes unique valid public keys from that report. This aligns the
producer-side nUNL vote policy with the active-view universe that RNG uses as
soon as ConsensusEntropy activates, without a separate supporting amendment or
rollout prerequisite. With ConsensusEntropy disabled, or without a usable
nonempty parent report, voting retains the locally configured trusted UNL
denominator. The consumer-side active-view builder remains defensive and caps
any raw ledger NegativeUNL overage against `originalViewSize`.

`quorumThreshold()` is 80% of that active validator view. Recent proposers,
expected proposers, and currently visible peer positions are liveness hints and
diagnostic context only; they do not shrink the quorum denominator and do not
decide fallback-vs-non-fallback output.

## Participant Diagnostics

Proposals may carry a signed `observedParticipantsHash` for debugging active-UNL
visibility during RNG rounds. The hash represents the active validators
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
- It is gated by ConsensusEntropy. Proposals carry it only in rounds whose
  parent ledger enables `featureConsensusEntropy`, and ingress validation
  rejects it when the proposal's parent ledger is locally available and does
  not enable that feature.
- It is intended to explain timing/degraded-network cases where commits,
  reveals, or sidecar hashes arrive late or asymmetrically.

## Proposal Relay And Local Sidecar Snapshots

In the common case, extension material arrives the same way proposals do:
through relay and observation over time. There is no "fetch missing proposal X"
mechanism in base consensus. Transaction sets are fetchable by hash; proposals
are not. A node that joins or falls behind mid-round normally observes for a few
ticks/rounds until the relayed proposal stream is coherent enough to participate.

RNG adds a stricter requirement on top of that proposal stream: its sidecar
roots are accepted by absolute thresholds over a fixed parent-ledger validator
denominator, not by percentages over whichever proposers this node happens to
observe. That fixed denominator is intentional. It gives the
sidecar gates deterministic, intersection-safe semantics: two quorum-aligned
cohorts cannot both make conflicting sidecar roots ledger material under the
same active-view assumptions. The cost is that missed proposal-borne material
does not shrink the target the way observed-proposer percentages do; it leaves
the node short of the fixed quorum.

Sidecar roots are signed into proposals, but the backing `SHAMapType::SIDECAR`
maps are local snapshots: generic transaction-set advertisement, serving, and
acquisition skip or refuse them. A node that missed proposal-carried material
may therefore be unable to materialize the quorum root this round. For RNG it
falls back to the explicit Tier 1 consensus digest or accepts a lower locally
materialized tier. If a quorum of validators did materialize and validate a
richer synthetic ledger, a missing-material validator follows that ledger
through the normal validation/LCL path after the round, just as it would after
failing to build any other majority ledger.

An extension takes its validator material through one ingress path, the signed
proposal stream: the design counts what the proposal round actually delivered
and makes degradation explicit.

## RNG Commit/Reveal Principles

RNG proceeds through establish sub-states:

1. `ConvergingTx`: ordinary transaction-set convergence while harvesting
   commitments. On proofed commit quorum (or, once `rngPIPELINE_TIMEOUT` has
   passed, a proofed cohort at the entropy gate) publish the commit sidecar
   hash and advance.
2. `ConvergingCommit`: check tx-converged positions for conflicting commit
   sidecar hashes, then freeze commit admission and reveal the same secret that
   produced the original commitment.
3. `ConvergingReveal`: collect reveals, publish the entropy sidecar hash, and
   wait for sidecar agreement or deterministic fallback.

These sub-states are sequential gates, not sequential collection phases.
Commitments ride on the initial proposal and are usually already harvested
during `ConvergingTx`; in a healthy round `ConvergingCommit` is often just a
one-tick checkpoint between commit-set publication and reveal. The
`ConvergingTx` wait covers late commit quorum, bounded by
`rngPIPELINE_TIMEOUT`. The `ConvergingCommit` wait covers only conflicting
commit-set hashes, bounded by `rngREVEAL_TIMEOUT` from the first observed
conflict. Neither wait is the normal commit transport path.

Commit-root conflict detection is not filtered by the active view: it compares
the commit-set hash of every trusted, tx-converged peer position. It feeds no
count; a conflict only opens the bounded `ConvergingCommit` wait.

Commit quorum counts only proofed commits from active validators. A commit that
cannot be emitted as a verifiable sidecar leaf does not count.

Every proposal signature is verified before its position enters consensus
peer-position state; cluster transport trust does not bypass that boundary.
Sidecar-root alignment then counts only stored positions whose captured master
belongs to the active view, while commitment/reveal harvest additionally verifies
the live signing-key-to-master attribution. Here, *proofed* means a sequence-zero
commitment accompanied by a self-contained serialized signed `ExtendedPosition`
whose signature and attribution both verify; a bare digest is not a proofed
commitment.

Reveal collection targets every active proofed committer admitted before the
commit freeze at the reveal transition: `hasMinimumReveals()` compares proofed
reveals with `proofedCommitCount()`, and commits harvested after the commit
sidecar hash was published but before the freeze are included. The published
commit-set root is used only for conflict detection; it does not define the
expected revealers. The reveal wait is bounded. A node that crashes, withholds,
or partitions after committing must not stop the ledger forever.

Final entropy is computed from the agreed entropy sidecar SHAMap, not from a
node's opportunistic local `pendingReveals_` map. This prevents different
local reveal subsets at timeout boundaries from producing different entropy.
The gate sets the accepted-hash latch only on the tick that returns
`readyForAccept`, and consensus enters the accepted phase on that same tick, so
an accepted root is final for the round. The bounded gate deadlines run before
acceptance: they clear the published root, the round proceeds to accept with
no accepted root, and selection falls back instead of reading a local
candidate map. That does not make accept-vs-timeout decisions global; it makes
any non-fallback injection depend only on the accepted root and its matching
local map, with ordinary validation quorum resolving boundary timing.

## Entropy Alignment Rules

Non-fallback entropy tiers require a UNLReport-anchored active view. A
non-standalone node mints only `consensus_fallback` (Tier 1) when the round's
active view is not built from an on-ledger `UNLReport` (`!fromUNLReport`),
regardless of how well peers aligned, because a config-derived view can differ
between nodes and yield divergent tier labels for the same set. (Standalone
nodes are a test-harness exception with configurable synthetic metadata,
defaulting to `validator_full` with count/denominator `20/20`.)
Everything below assumes a UNLReport-anchored view.

Two distinct counts are involved; keep them separate.

The **alignment (gate) count** decides whether the round proceeds with the agreed
entropy set or falls back. It is taken over the active validator view:

```
(our entropySetHash, counted only if this node is an active validator in
 proposing mode, i.e. it published the root in a signed proposal)
  + active-view, tx-converged peers advertising the same entropySetHash
```

`detail::inspectTxConvergedSidecarPeers` (`ConsensusExtensionsTick.h`) takes
the peer term from every active-view NodeID in the peer-position map and adds
the local node separately. It therefore relies on `NetworkOPsImp::processTrustedProposal`
dropping trusted proposals signed with this validator's own signing or master
key. Without that drop, a validator key duplicated on another server, or a
relayed copy of this node's own proposal re-signed in a non-canonical form
(which gets a fresh suppression id), would count this node twice.

Trusted-but-non-active proposers are NOT counted even when tx-converged and
aligned (peers are filtered through the active view via `isUNLReportMember`), and
the local node adds its own +1 only when it is an active validator
(`localIsActiveValidator()`) in proposing mode. An observing node still writes
the root into its local position, but it sends no proposal and does not count
itself. This mirrors the `buildEntropySet`/`hasQuorumOfCommits`
membership filter and keeps the counting universe from inflating above the
active-view size, preserving the Tier-2 intersection margin (`2t - n`) and
equivocation uniqueness. On the clean path, the round proceeds when this count
reaches `entropyGateThreshold() = min(quorumThreshold(), tier2Threshold())`;
silence from a tx-converged active validator is not itself a conflicting value
and must not become a one-validator RNG veto. If a conflicting entropy hash is
observed, the same fixed-denominator threshold remains the deciding boundary:
only one entropy hash can be quorum-aligned under the intersection margin, so a
below-threshold conflicting minority or silent peer must not force fallback once
our hash reaches the gate. Conflicting states below the threshold still wait
only until the bounded deadline, then fall back.

The **tier label** is then derived from the agreed entropy set itself — the
number of validator reveals (leaves) in the agreed `entropySetMap_`, not the
peer-alignment count above. If that leaf count equals the effective active view
size, the set is labeled `validator_full`; if it is below full participation but
reaches `quorumThreshold()`, it is labeled `validator_quorum`; if it is below
`quorumThreshold()` but reaches `tier2Threshold()`, it is labeled
`participant_aligned`; otherwise the round falls back. `quorumThreshold()` is 80%
of the effective active view; `tier2Threshold()` is the intersection-safe floor
over the original pre-nUNL view.

Under nUNL, exact integer thresholds can cross either way. For example, a
20-validator original view with five disabled validators has effective quorum
12 but participant threshold 13. That only changes which threshold dominates
the proceed gate; the final tier label is still derived from the agreed entropy
set count by the ladder above.

In both label cases, a silent or below-threshold minority cannot veto the
aligned cohort. A below-threshold conflicting or locally unmaterializable
entropy hash is handled by the conflict path and falls back if the bounded
window does not produce a quorum-aligned hash.

If no entropy hash reaches the entropy gate threshold before the bounded
deadline, the round must fall back to the Tier 1 consensus-bound digest. This
is the safe degradation path, not a consensus failure.

That fallback condition is a gate result, not a local reachability guess. A
node-local count such as "previous proposers seen" or "current peer positions
visible here" is not a stable consensus input and must not skip the pipeline
when the proofed sidecar material already meets the entropy gate. At the
deadline boundary, nodes can still disagree about accept-vs-fallback until
validation chooses the ledger; that residual is bounded and tracked, not a
license for extra local gates.

Examples with six active validators on a UNLReport-anchored view (validator_quorum
threshold five, participant_aligned threshold four, so the proceed gate is four;
six is the smallest view with a non-empty Tier 2 band and non-zero tolerated
Byzantine count; at five validators quorum and participant_aligned coincide at
four, leaving no band). Each example gives both counts: alignment decides
proceed-vs-fallback, and the accepted set's leaf count decides the label. On a
non-UNLReport (config-fallback) view, every case below instead mints
`consensus_fallback`:

- All six validators reveal and align on the root of that six-leaf set: proceed
  with validator_full entropy (`EntropyCount == EntropyDenominator`) so hooks
  that demand full active-validator participation can fail closed on any
  withholding.
- Five validators reveal and align on the root of that five-leaf set; the sixth
  withholds its reveal and advertises a bogus hash: proceed (five aligned) with
  validator_quorum entropy (five leaves). If the sixth validator's reveal had
  reached the honest nodes before they built the set, the set would hold six
  leaves and earn validator_full despite the bogus advertisement: the label
  counts reveals, not aligned advertisers.
- Four validators reveal and align on the root of that four-leaf set; the other
  two withhold their reveals and advertise different bogus hashes: proceed
  (four aligned) with participant_aligned (Tier 2) entropy (four leaves). Four
  is below the 80% quorum but at the intersection-safe floor, and the overlap
  of any two aligned cohorts (2*4 - 6 = 2 > floor(6/5) = 1) still contains an
  honest validator.
- Three validators align on the honest hash and three fail to align: the
  alignment count stays below four, so the round falls back to the Tier 1
  digest at the bounded deadline, whatever the set's leaf count.
- No peer entropy hash is observed in time: fall back to the Tier 1 digest.

The fallback pseudo-transaction is deterministic — every node derives the same
digest from `(HashPrefix::entropyFallback, parentLedgerHash, buildTxSetHash,
seq)`, where `buildTxSetHash` is the agreed set after removing only supplied
ConsensusEntropy transactions — and labels it with
`EntropyTier = consensus_fallback`,
`EntropyCount = 0`, `EntropyDenominator = 0`, and an empty
`EntropyContributors` bitmap. Non-fallback entropy records both the contributor
count and the active-validator denominator used for the validator-quorum
threshold, plus a canonical `EntropyContributors` bitmap ordered by the
parent-ledger active-validator view. The participant-aligned floor still uses
the original pre-NegativeUNL view internally. The contributor bitmap is written
to the pseudo-transaction for observability, but is not part of the
transaction-ordering salt; the salt already binds the selected
digest/tier/count/denominator tuple. This does not make the bitmap
non-critical: any disagreement in a ledger-written field is a ledger
disagreement. It only keeps transaction ordering semantically tied to the
entropy value and quality labels rather than to the accountability label. Hooks
state their class requirement through the mandatory `min_tier` argument to
`entropy_cr_dice()`/`entropy_cr_random()`: a hook that demands validator-tier entropy fails closed
with `TOO_LITTLE_ENTROPY` on fallback ledgers, while a hook that opts into
fallback-grade randomness must do so explicitly at the call site. Valid
`min_tier` values are the stored entropy tiers 1..4; invalid requirements return
`INVALID_ARGUMENT`, while valid-but-unmet requirements return
`TOO_LITTLE_ENTROPY`.

The draw signatures are `entropy_cr_dice(sides, min_tier, flags)`
and `entropy_cr_random(write_ptr, write_len, min_tier, flags)`. `flags = 0`
requires terminal strong execution. A later eligible strong Hook, even in the
same account's chain, causes `LATER_STRONG_HOOK` (-49) at the API call before
output or draw-counter mutation. The application can handle the error without
rejecting its transaction. `ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO = 1U << 1`
permits later strong Hooks only on the drawing Hook's installed account; a
later foreign-account strong Hook still refuses the draw.
`ENTROPY_ALLOW_ANY_STRONG_VETO = 1U << 0` permits subsequent strong Hooks on
any account, including the same account. Setting both bits means ANY; other
bits are invalid. Allowed Hooks retain ordinary veto and skip behavior.
All modes enforce the same entropy quality and freshness admission.
Same-account permission trusts the downstream code installed on that account
and whoever can replace it. Hooks trusted independently of their host account
should keep zero flags. Untrusted transaction parameters must not choose the
policy. A broader call cannot clear a prior successful draw's stricter guard.
Terminal Hook/account eligibility is planned before strong execution using the dispatch
filters and stakeholder order; dynamic skip requests cannot grant permission.
The guarantee excludes the drawing Hook's own aborts, shared-resource failures,
and failures in base application. Value-bearing settlement still requires an
earlier commitment and a fixed outcome across retries.

`entropy_cr_status()` returns a packed non-negative scalar with tier in bits
32..39, contributor count in bits 16..31, and denominator in bits 0..15.
Negative values remain Hook API errors. This lets Hook code implement policies
such as one-absent tolerance,
proportional participation, or an absolute floor without embedding those
participation policies in each draw call. Status exposes no digest. Callers
must classify tier before count or denominator arithmetic because fallback
deliberately reports tier 1 and `0/0`.
Status remains metadata-only, independent of the per-call composition flags.

Open-ledger hook execution is provisional. During speculative open-ledger
execution, `entropy_cr_dice()`/`entropy_cr_random()` and `entropy_cr_status()` can only use the previous
ledger's finalized entropy; final buildLCL execution sees the current ledger's
entropy pseudo-tx after it publishes the host-only ledger execution context. Hooks that need final entropy must
treat open-ledger RNG results as previews.

The fallback digest derives from the sanitized pre-injection live-build set
hash to avoid circularity and synthetic-input authority. Sanitization removes
only supplied `ttCONSENSUS_ENTROPY` transactions; legacy fee, amendment,
NegativeUNL, and other protocol pseudos remain ordinary members of the agreed
set. Supplied entropy pseudos are discarded and logged as errors before they
can influence fallback entropy, transaction ordering, or ledger state, then the
one canonical entropy pseudo is derived locally by the deterministic selector.
The original agreed-set hash remains the consensus-bookkeeping value carried by
validations. Historical replay is selected before live materialization and
consumes the persisted transaction order, including its recorded synthetic
transactions, without regeneration.

## Local Snapshot Alignment Rules

Sidecar SHAMaps are local immutable snapshots:

- RNG entries must have been harvested from trusted signed proposals.
- Snapshot roots may be signed into `ExtendedPosition` for peer observation.
- Peer-advertised roots are alignment evidence, not payload availability.
- Sidecar maps stay local: they are never advertised or served to peers, and
  transaction-set acquisition refuses them.
- If a quorum root cannot be materialized locally before the bounded deadline,
  RNG degrades or falls back.

Do not use avalanche-style transaction inclusion logic for sidecar inputs.
For RNG sidecars, the disagreement to resolve is usually timing or delivery,
not whether a valid contribution should be included.

The entropy sidecar gate waits one observation tick after the round's first
publication of `entropySetHash`, because publishing and accepting in the same
tick can hide conflicts and produce asymmetric synthetic outcomes. The wait
applies once per round: a root refreshed later in the round, by the conflict
rebuild or by re-publication on a later reveal tick, is evaluated in the tick it
is published, against the peer roots already observed. A quorum-aligned root can
proceed without full observation, because one silent active validator must not
get a free RNG off-switch; however, a validator that cannot locally materialize
that quorum root will not build the richer synthetic ledger in that round.

## Proposal Wire Format And Rollout

`featureConsensusEntropy` is amendment gated, and its per-round enable latch is
read from the consensus parent ledger's rules.

Rollout invariant: enabling `featureConsensusEntropy` switches the network to
extension-aware proposal semantics. An individual proposal with no populated
sidecar fields still serializes to the legacy 32-byte tx-set hash, but live
proposal messages may instead use the `TMProposeSet` protobuf field
`currentTxHash` (C++ accessor `currenttxhash()`) to carry a serialized
`ExtendedPosition`. This is a proposal wire-format change: older binaries that
require `currentTxHash` to be exactly 32 bytes cannot process these proposals.
A network that activates ConsensusEntropy therefore needs every binary expected
to process live proposals to understand the extended position format. Once the
amendment is active, the peer-protocol gate in
[Amendment-gated peer protocol features](../../overlay/ProtocolFeatureRequirements.md)
disconnects sessions that did not negotiate the matching capability; it does
not make older binaries compatible.

## Review Checklist

When changing consensus extension code, check these questions:

- Does this preserve transaction-set identity as the core consensus identity?
- Does every extension wait have a bounded fallback?
- Does the proceed gate require the alignment count to reach
  `entropyGateThreshold()` (= min(quorumThreshold(), tier2Threshold())), and is
  the tier label (validator_full / validator_quorum / participant_aligned)
  derived only from the accepted set's leaf count, never from the alignment
  count?
- Can one bad validator deny entropy to an honest quorum? It must not.
- Can a sub-quorum set produce participant_aligned entropy only after reaching
  the intersection-safe tier2Threshold()?
- Are quorum calculations using the active validator view, not recent
  proposers as the denominator?
- Do non-fallback tier labels (validator_full / validator_quorum /
  participant_aligned) require a UNLReport-anchored active view, falling back to
  `consensus_fallback` when the view is config-derived (`!fromUNLReport`)?
- Is the alignment-count *universe* itself — not just the quorum denominator —
  the active validator view (non-active proposers, and a local node that is not
  an active validator in proposing mode, excluded from the count)?
- Are sidecar entries typed as sidecars, not pseudo-transactions?
- Are proposal-visible or validation-visible sidecar fields covered by the
  relevant signature and duplicate/replay identity?
- Does ledger-defining sidecar material reach apply and replay only through a
  deterministic transaction-stream witness, never through ephemeral sidecar
  memory?
- Does validator material enter through the signed proposal stream only, with
  no second ingress path?
