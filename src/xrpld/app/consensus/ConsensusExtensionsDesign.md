# Consensus Extension Design Principles

This note captures the principles behind the Xahau consensus extensions:
ConsensusEntropy/RNG, proposal sidecars, and export signature convergence.
Read this before changing `ConsensusExtensions`, `ConsensusExtensionsTick`,
`ExtendedPosition`, sidecar SHAMap handling, or the related CSF tests.

The short version: extension data may coordinate extra ledger-visible features,
but it must not redefine ordinary transaction-set consensus. When extension
state cannot be made safe in time, the extension degrades deterministically
and the ledger still closes.

The priority order for consensus extensions is: safe, fast, works. Safety means
extension timing must not create divergent closed-ledger effects when a bounded
coordination step can avoid it. Fast means those coordination steps stay short
and conditional, never becoming an open-ended wait for an extension feature to
succeed. Works means missed or late extension material follows that feature's
deterministic fallback, such as Tier 1 consensus_fallback entropy for RNG or normal
Export pending/expiry behavior, rather than blocking core consensus.

## Fallback Semantics

RNG and Export use similar positive-path sidecar gates, but they do not have
the same safe fallback. RNG closes with a deterministic consensus-bound fallback
digest (Tier 1) in either of two cases: (1) when peers cannot establish an
accepted participant_aligned or validator_quorum entropy set in time, or (2)
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
under validator-entropy semantics. Export has no equivalent fallback value:
without quorum-aligned verified export signatures, the admitted latch remains
pending until a later witness or publication expiry. The source transaction
does not retry.

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
   in templated consensus code. RNG, export sig, commit-set, and entropy-set
   hashes are proposal sidecars. They are coordinated during establish, but they
   do not define whether peers agree on the ordinary transaction set.

2. Extension waits are bounded.

   RNG and export sidecar convergence may wait briefly inside establish, but
   they must not block ledger close indefinitely. If RNG cannot establish
   an accepted entropy set, it injects the deterministic Tier 1
   consensus_fallback digest (labeled `consensus_fallback`, count 0). If export
   signatures cannot converge, the Export remains pending or its publication
   window expires without blocking ledger close.

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

   RNG commits, RNG reveals, and export signatures are the verifiable inputs.
   The design aligns on signed roots over those input sets using local sidecar
   SHAMap snapshots. The final entropy digest and export quorum result are
   derived from the accepted local snapshot, not from live collector state.

5. Sidecars are not transactions.

   Commit, reveal, and export signature entries are `STObject(sfGeneric)`
   leaves in ephemeral `SHAMapType::SIDECAR` maps. They use `sfSidecarType`
   to distinguish payloads and `HashPrefix::sidecar` for item hashes. Current
   same-round consensus does not advertise, fetch, serve, or merge these maps
   from peers. The maps are local immutable snapshots used to materialize the
   root a node signed into its proposal and, if accepted, to build the
   ledger-visible pseudo/witness.

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

7. Proposal-carried material is untrusted until semantic validation passes.

   A sidecar root proves only byte identity for the local snapshot that produced
   it. It does not prove that a contribution is well-formed, authorized, or
   round-correct. Proposal-carried commits, reveals, and export signatures must
   pass cheap structural checks, safe key-type checks, active-view membership,
   and the relevant cryptographic proof before entering pending RNG/export
   state. Cluster trust may affect relay and resource policy, but extension
   sidecars become ledger inputs and must be harvested only after the signed
   proposal verifies against the claimed validator key.

8. Ledger-defining sidecar material crosses apply as transaction-stream input.

   Sidecars are an establish-phase convergence mechanism, not a ledger replay
   input. If sidecar material changes a closed-ledger effect that cannot be
   recomputed from the parent ledger and ordered transaction set, the accepted
   material must first be represented by a deterministic pseudo transaction (or
   an equivalent transaction-stream witness). Apply/replay then consumes that
   witness and re-validates it; it must not depend on ephemeral sidecar memory.

## Validator Set And Quorum

The active validator view is the shared source-alignment denominator for RNG
and Export qV. Export qC has a separate account-owned committee denominator:

- Prefer `UNLReport.sfActiveValidators` from the consensus parent ledger.
- If no report is available, fall back to configured trusted validators so
  early ledgers and dev/test networks can make progress. This configured
  fallback view is `!fromUNLReport`: a non-standalone node mints only
  `consensus_fallback` entropy under it and must not finalize Export, because a
  config-derived view can diverge between nodes.
- If `featureNegativeUNL` is enabled, subtract the parent ledger's Negative
  UNL from whichever source produced the view.
- Use the same snapshot throughout the round.

When `featureNegativeUNLActiveViewCap` is enabled, NegativeUNL vote production
also uses the parent-ledger `UNLReport.sfActiveValidators` count as the
25-percent disable-cap denominator. This aligns the producer-side nUNL vote
policy with the active-view universe that RNG and Export proofs use. Without
that amendment, legacy NegativeUNL voting can still cap against the locally
configured trusted UNL size; the consumer-side active-view builder remains
defensive and caps any raw ledger NegativeUNL overage against `originalViewSize`.

`quorumThreshold()` is 80% of that active validator view. Recent proposers,
expected proposers, and currently visible peer positions are liveness hints and
diagnostic context only; they do not shrink the quorum denominator and do not
decide fallback-vs-non-fallback output.

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
- It is shared extension diagnostics, not RNG-only material. Proposals may
  carry it when either `featureConsensusEntropy` or `featureExport` is active,
  and ingress validation must reject it only when both extensions are disabled.
- It is intended to explain timing/degraded-network cases where commits,
  reveals, or sidecar hashes arrive late or asymmetrically.

## Proposal Relay And Local Sidecar Snapshots

In the common case, extension material arrives the same way proposals do:
through relay and observation over time. There is no "fetch missing proposal X"
mechanism in base consensus. Transaction sets are fetchable by hash; proposals
are not. A node that joins or falls behind mid-round normally observes for a few
ticks/rounds until the relayed proposal stream is coherent enough to participate.

RNG and Export add a stricter requirement on top of that proposal stream: their
sidecar roots are accepted by absolute quorum over a fixed parent-ledger
validator denominator, not by percentages over whichever proposers this node
happens to observe. That fixed denominator is intentional. It gives the
sidecar gates deterministic, intersection-safe semantics: two quorum-aligned
cohorts cannot both make conflicting sidecar roots ledger material under the
same active-view assumptions. The cost is that missed proposal-borne material
does not shrink the target the way observed-proposer percentages do; it leaves
the node short of the fixed quorum.

This branch deliberately does not add same-round sidecar reconciliation for that
gap. Sidecar roots are still signed into proposals, but the backing
`SHAMapType::SIDECAR` maps are local snapshots only. They are not advertised,
served, fetched, or merged from peers, and generic transaction-set acquisition
must reject them. A node that missed proposal-carried material may therefore be
unable to materialize the quorum root this round. For RNG it falls back to the
explicit Tier 1 consensus digest or accepts a lower locally materialized tier; for
Export the latch remains pending or its publication window expires. If a quorum
of validators did
materialize and validate a richer synthetic ledger, a missing-material validator
follows that ledger through the normal validation/LCL path after the round, just
as it would after failing to build any other majority ledger.

That is the tradeoff being ratified here. Reconciliation was useful only in a
narrow topology: a validator was up and proposed, its proposal-carried material
failed to push-relay to some cohort before the deadline, another reachable peer
advertised a root covering it, and a content-addressed pull completed quickly
enough to cross a tier/signature boundary. In the realistic cases examined, that
is an edge of an already unhealthy overlay; if the cut is severe enough to matter
for sidecar fetch, validations and ordinary proposal relay are also under stress.
Keeping the fetch path required aggressive same-round acquisition to help in
time, which made the protocol chatty and added a second semantic ingress path
for the same validator material. The review history showed that dual-ingress
surface repeatedly produced drift bugs. The current design therefore counts what
the proposal round actually delivered and makes degradation explicit instead of
keeping an availability mechanism whose value has not justified its surface.

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

Every proposal signature is verified before its position enters consensus
peer-position state; cluster transport trust does not bypass that boundary.
Sidecar-root alignment then counts only stored positions whose captured master
belongs to the active view, while commitment/reveal harvest additionally verifies
the live signing-key-to-master attribution. Here, *proofed* means a sequence-zero
commitment accompanied by a self-contained serialized signed `ExtendedPosition`
whose signature and attribution both verify; a bare digest is not a proofed
commitment.

Reveal collection targets all known committers, because the commit sidecar set
defines who is expected to reveal. The reveal wait is still bounded. A node
that crashes, withholds, or partitions after committing must not stop the
ledger forever.

Final entropy is computed from the agreed entropy sidecar SHAMap, not from a
node's opportunistic local `pendingReveals_` map. This prevents different
local reveal subsets at timeout boundaries from producing different entropy.
The accepted-hash latch is provisional until live injection: a later bounded
gate deadline may clear it, after which selection falls back instead of reading
a local candidate map. That does not make accept-vs-timeout decisions global;
it makes any non-fallback injection depend only on a matching root that remains
accepted at injection, with ordinary validation quorum resolving boundary
timing.

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
(our published entropySetHash, counted only if this node is itself an active validator)
  + active-view, tx-converged peers advertising the same entropySetHash
```

Trusted-but-non-active proposers are NOT counted even when tx-converged and
aligned (peers are filtered through the active view via `isUNLReportMember`), and
a non-active local node does not add its own +1 (gated on
`localIsActiveValidator()`). This mirrors the `buildEntropySet`/`hasQuorumOfCommits`
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
threshold five, participant_aligned threshold four; six is the smallest view with
a non-empty Tier 2 band and non-zero tolerated Byzantine count; at five validators
quorum and participant_aligned coincide at four, leaving no band). On a
non-UNLReport (config-fallback) view, every case below instead mints
`consensus_fallback`:

- Six honest validators align on one entropy hash: proceed with validator_full
  entropy (`EntropyCount == EntropyDenominator`) so hooks that demand full
  active-validator participation can fail closed on any withholding.
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
digest from `(HashPrefix::entropyFallback, parentLedgerHash, buildTxSetHash,
seq)`, where `buildTxSetHash` is the agreed set after removing only supplied
ConsensusEntropy and Export synthetic transactions — and labels it with
`EntropyTier = consensus_fallback`,
`EntropyCount = 0`, `EntropyDenominator = 0`, and an empty
`EntropyContributors` bitmap. Non-fallback entropy records both the contributor
count and the active-validator denominator used for the validator-quorum
threshold, plus a canonical `EntropyContributors` bitmap ordered by the
parent-ledger active-validator view. The participant-aligned floor still uses
the original pre-NegativeUNL view internally. The contributor bitmap is written
to the pseudo-transaction and SLE for observability, but is not part of the
transaction-ordering salt; the salt already binds the selected
digest/tier/count/denominator tuple. This does not make the bitmap
non-critical: any disagreement in a ledger-written field is a ledger
disagreement. It only keeps transaction ordering semantically tied to the
entropy value and quality labels rather than to the accountability label. Hooks
state their class requirement through the mandatory `min_tier` argument to
`dice()`/`random()`: a hook that demands validator-tier entropy fails closed
with `TOO_LITTLE_ENTROPY` on fallback ledgers, while a hook that opts into
fallback-grade randomness must do so explicitly at the call site. Valid
`min_tier` values are the stored entropy tiers 1..4; invalid requirements return
`INVALID_ARGUMENT`, while valid-but-unmet requirements return
`TOO_LITTLE_ENTROPY`.

`entropy_status()` returns a packed non-negative scalar with tier in bits
32..39, contributor count in bits 16..31, and denominator in bits 0..15.
Negative values remain Hook API errors. This lets Hook code implement policies
such as one-absent tolerance,
proportional participation, or an absolute floor without widening the frozen
draw API. It exposes no digest. Callers must classify tier before count or
denominator arithmetic because fallback deliberately reports tier 1 and
`0/0`.

Open-ledger hook execution is provisional. During speculative open-ledger
execution, `dice()`/`random()` and `entropy_status()` can only use the previous
ledger's finalized entropy; final buildLCL execution sees the current ledger's
entropy pseudo-tx after it updates the SLE. Hooks that need final entropy must
treat open-ledger RNG results as previews.

The fallback digest derives from the sanitized pre-injection live-build set
hash to avoid circularity and synthetic-input authority. Sanitization removes
only supplied `ttCONSENSUS_ENTROPY` and `ttEXPORT_SIGNATURES` transactions;
legacy fee, amendment, NegativeUNL, and other protocol pseudos remain ordinary
members of the agreed set. Supplied extension pseudos are discarded and logged
as errors before they can influence fallback entropy,
transaction ordering, or ledger state, then the canonical synthetic stream is
derived from accepted extension evidence. The original agreed-set hash remains
the consensus-bookkeeping value carried by validations. Historical replay is
selected before live materialization and consumes the persisted transaction
order, including its recorded synthetic transactions, without regeneration.

## Local Snapshot Alignment Rules

Sidecar SHAMaps are local immutable snapshots:

- RNG entries and proposal-carried Export entries must have been harvested from
  trusted signed proposals. Direct Export relay entries instead pass the same
  validated-origin, manifest-attribution, and target-signature admission checks
  before entering the collector.
- Snapshot roots may be signed into `ExtendedPosition` for peer observation.
- Peer-advertised roots are alignment evidence, not payload availability.
- Nodes never fetch, advertise, serve, or merge sidecar maps from peers.
- If a quorum root cannot be materialized locally before the bounded deadline,
  RNG degrades/falls back and Export injects no witness in that ledger.

Do not use avalanche-style transaction inclusion logic for sidecar inputs.
For RNG and export sidecars, the disagreement to resolve is usually timing or
delivery, not whether a valid contribution should be included.

The entropy sidecar gate always gives peers at least one observation tick after
publishing `entropySetHash`. Publishing and accepting in the same tick can hide
conflicts and produce asymmetric synthetic outcomes. A quorum-aligned root can
proceed without full observation, because one silent active validator must not
get a free RNG off-switch; however, a validator that cannot locally materialize
that quorum root will not build the richer synthetic ledger in that round.

## Export Principles

`ExportIntent.md` is the normative spine for Export invariants, especially the
replay-witness rule. This section explains the current mechanics and should not
be read as permission to make closed-ledger Export output depend on ephemeral
sidecar memory.

`featureExport` and `featureConsensusEntropy` are independently amendment
gated.

Export can run without ConsensusEntropy. Two thresholds remain deliberately
separate:

- qC is `ceil(0.8 * C)` over one explicit account-owned committee of at most 32
  validator master keys. It decides whether one Export has enough target
  signatures.
- qV is the source active-validator threshold used to align one exact
  `exportSigSetHash`. It decides whether the sidecar snapshot is coordinated
  enough to materialize witnesses.

One missing sidecar advertiser must not veto a qV-aligned root, and one missing
committee signer must not lower qC. Neither threshold is unanimity or a count of
locally visible peers.

The committee is an immutable `ltEXPORT_COMMITTEE` owned by the exporting
account. It stores one canonical sorted master-key roster and is named by a
network-neutral content digest. Latches store only the digest. There is no
default committee and no implicit capped subset of the full UNL. Bare setup may
pre-stage a roster, but every actual intent checks every roster master against
the exact admitting parent ledger's pre-NegativeUNL `UNLReport`. Inline roster
creation and intent admission perform that check atomically. Committee deletion
is blocked while the owner has any live Export latch. That guard is coupled to
witness apply's parent-state committee lookup: weakening it to pending-only
would make a retained latch's accepted witness unreplayable.

The destination account's SignerList remains the actual remote authority. Its
keys, weights, optional operator signer, and rotation policy are client/operator
configuration that source consensus cannot verify. The committee cap matches
`STTx::maxMultiSigners()`; reserving a separate operator entry reduces the
maximum validator roster usable by that destination account. A lower remote
threshold weakens the claimed custody policy, while a higher threshold may
strand an otherwise sufficient source witness. Target ledger validation and the
returning XPOP are separate from this authorization decision.

The extended proposal machinery is enabled when either feature needs signed
sidecar fields. Do not make Export depend on RNG availability just because RNG
was the first consumer of `ExtendedPosition`.

Rollout invariant: enabling `featureConsensusEntropy` or `featureExport`
switches the network to extension-aware proposal semantics. An individual
proposal with no populated sidecar fields still serializes to the legacy
32-byte tx-set hash, but live proposal messages may instead use the legacy
`currenttxhash` protobuf field to carry a serialized `ExtendedPosition`. This is
a proposal wire-format change, not a sidecar-reconciliation detail. Disabling
sidecar fetch/reconciliation does not restore compatibility with older binaries
that require `currenttxhash` to be exactly 32 bytes. A network that activates
either extension therefore needs every binary expected to process live
proposals to understand the extended position format, or it needs explicit
version/capability negotiation before activation.

When `featureExport` is disabled, the export sidecar gate is disabled too. Stale
collector entries must not keep a stopped amendment active.

Hook-originated Export uses the same `ttEXPORT` admission path through a
separately bounded lane. `xport_reserve()` consumes both the Hook Export budget
and ordinary emission slots; `xport()` may reference only an already existing
committee and generic `emit()` cannot emit `ttEXPORT`. Deferred emitted Export
wrappers are capped in the emitted directory before the creating transaction
commits. Owner-scoped `xport_cancel()` may control other latches, but refuses the
exact latch being created by its enclosing Export or whose XPOP transition is
owned by its enclosing Import callback path.

Intent admission creates an origin-keyed `ltEXPORT_LATCH` immediately. The latch
records the normalized identity digest, origin `W` and source sequence,
publication end, destination TicketSequence, committee digest, owner-directory
link, and pending-work link. Lack of signatures does not make the source
transaction retry. The pending directory is durable scheduling state across
rounds and restarts.

An owner may have at most one live latch for a destination TicketSequence,
regardless of `W`. Flagless control and publication expiry retain the latch, so
they also retain that one-shot destination-authority slot. Reissuing the same
normalized target after terminal erasure creates a new outer transaction and a
new `W`; it does not recreate the old latch. Clients must not reissue a target
TicketSequence that the destination chain has already consumed.

Honest signature release is keyed to fully validated history, not to the open
ledger or current transaction candidate set. When an exact origin ledger becomes
validated, eligible committee members reconstruct the target payload, append
the canonical release anchor `(source domain, target domain, W, origin sequence,
origin hash)`, and sign that target with their current manifest signing key.
Ledger building and validation are asynchronous; a later build may already be
in progress. Export waits for finality without waiting or rewinding base
consensus.

Only structurally valid, attributable, cryptographically verified shares enter
the collector. A share identifies the owner, `W`, exact origin ledger, committee
position, signing key, and ordinary XRPL multisignature. Live admission resolves
the immutable committee master at that position through the manifest cache. A
missing binding is deferred or rejected; it never silently selects another
member.

The dedicated `mtEXPORT_SHARES` relay is the immediate post-validation flood and
subscription path. Proposal carriage republishes each validator's own retained
shares as a bounded authenticated backup; it does not carry that node's full
collector union. Both feed the same collector and verification logic.
Proposal-carried material is still untrusted until the proposal and share
semantics verify.

The collector forms one complete bounded unique-share union over all live
origins, keyed by `(W, committeePosition)`. A second distinct valid contribution
at one position is conflicting and contributes zero for that origin. The union
is projected into an ephemeral sidecar map; its root is advertised in signed
`ExtendedPosition` traffic. qC is not a root filter: below-qC partial material
still participates in the common union, while qC is checked only when deciding
which witnesses can be materialized.

Export sidecar publication is local-material only. Peer-advertised roots are qV
alignment evidence; they do not reconstruct missing signatures. A node injects
from an accepted root only when its exact local sidecar snapshot matches that
root and independently verifies. The bounded gate is a short chance for
already-in-flight material to align after ordinary transaction-set convergence,
not a wait for any particular Export to reach qC. Timeout always closes the
ledger and injects no Export witness from an unavailable or unaligned root.
Standalone test execution has no peer qV gate and substitutes exact possession
of its locally verified sidecar map; intent admission still requires the
UNLReport-backed parent state.

For each still-pending origin that reaches qC in the accepted root, pre-build
materializes at most one canonical later-ledger `ttEXPORT_SIGNATURES`. The
pseudo carries `W`, the release-stamped target transaction, a committee-relative
contributor bitmap, and the selected signatures. It is the replay witness for
the accepted sidecar evidence.

`Change::applyExportSignatures` reads the referenced immutable committee from
parent ledger state, derives its size and qC, validates the contributor bitmap,
normalization and release anchor, checks every target signature, records the
witness transaction ID on the latch, and removes the pending link. It does not
read current manifests. Historical replay consumes the same witness bytes and
committee SLE and never regenerates shares. An explicitly erased latch makes a
concurrently ordered witness an evidence-only no-op.

The witness is quorum-attested evidence, not a self-verifying archive of
historical manifest bindings. Live admission proves each signing-key-to-master
attribution before collector entry; validated witness inclusion records that
the materializing validators performed that check. Replay can recheck the
stored signatures, bitmap, qC, and immutable committee, but cannot reconstruct
the historical manifest cache from witness bytes alone.

The full release-stamped payload and signatures are stored once in the witness.
The latch stores only `sfExportSignatureHash`. Read-time assembly uses the
witness's target and signatures, empty `SigningPubKey`, canonical AccountID
ordering, and the target's 32-signer cap. The normalized latch digest is
signature-subset-independent, so a different sufficient signer subset does not
orphan the source callback.

Import/XPOP and witness arrival are independent monotonic facts. Whichever
arrives second erases the latch and releases reserve. Flagless W-only lifecycle
control unlinks publication but retains callback readiness; `tfExportEraseLatch`
irreversibly erases the exact issuance. Publication expiry follows the retain
path. Committee deletion remains blocked while any owner latch exists. v1 has
no permanent tombstone or paid-bump transition.

The `export_signatures` subscription projects each admitted post-validation
share immediately and republishes retained live shares once per validated
cursor. Clients deduplicate by origin and committee position. Slow subscribers
must not hold collector or consensus locks, and silence after an origin leaves
the pending set is not a durable terminal event.

This remains intentionally leaner than XPOP. XRPL sees ordinary multisignatures,
not a proof of Xahau finality. XPOP carries the reverse-chain proof material and
drives the matching callback after target finality.

Export sig convergence runs in parallel with RNG. An export-side convergence
failure must not change RNG semantics; an RNG fallback must not make export
unsafe. Each feature has its own gate and fallback.

Accept-time cleanup must preserve Export state through `onPreBuild` whenever
`featureExport` is enabled so the signature witness pseudo can be injected.
After the ordered transaction set contains that witness, replay must not need
the round's export sidecar convergence state.

CSF consensus tests model the Export sidecar gate directly. Unit and testnet
coverage must additionally exercise committee setup/use/deletion, post-validation
relay and subscription, restart recovery, below-qC persistence, and later
witness/XPOP ordering.

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
- Can an Export share be produced before the exact origin ledger validates? It
  must not.
- Does every intent name an immutable account-owned committee whose complete
  roster is checked against the exact admitting parent's pre-NegativeUNL
  `UNLReport`?
- Are qC (committee sufficiency) and qV (source root alignment) kept separate?
- Are Export signatures attributable and verified before collector admission?
- Does witness materialization require local possession of the exact
  qV-aligned `exportSigSetHash`, not merely a local qC?
- Does every ledger-defining Export signature enter replay through a later
  self-contained `ttEXPORT_SIGNATURES` witness?
- Does accepted witness apply derive qC from the immutable committee SLE without
  consulting mutable manifests?
- Can timeout select a largest-but-below-qC signature set? It must not.
- Are CE and Export still independently gated and independently stoppable?
