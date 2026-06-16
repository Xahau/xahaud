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
the same safe fallback. RNG can safely close with a deterministic
consensus-bound fallback digest (Tier 1) when peers cannot establish an
accepted participant_aligned or validator_quorum entropy set in time: every input to the fallback
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

   `ExtendedPosition::operator==` intentionally compares only `txSetHash`.
   RNG, export sig, commit-set, and entropy-set hashes are proposal sidecars.
   They are coordinated during establish, but they do not define whether peers
   agree on the ordinary transaction set.

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
  early ledgers and dev/test networks can make progress.
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
- It does not participate in `ExtendedPosition::operator==`.
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

Commit quorum counts only proofed commits from active validators. A commit that
cannot be emitted as a verifiable sidecar leaf does not count.

Reveal collection targets all known committers, because the commit sidecar set
defines who is expected to reveal. The reveal wait is still bounded. A node
that crashes, withholds, or partitions after committing must not stop the
ledger forever.

Final entropy is computed from the agreed entropy sidecar SHAMap, not from a
node's opportunistic local `pendingReveals_` map. This prevents different
local reveal subsets at timeout boundaries from producing different entropy.

## Entropy Alignment Rules

Validator-tier entropy requires quorum alignment on the entropy sidecar
hash. Participant-aligned entropy uses the lower `tier2Threshold()` floor over
the original pre-nUNL view.

The alignment count is:

```
our published entropySetHash + tx-converged peers with the same entropySetHash
```

If that count reaches `quorumThreshold()`, the node labels the agreed set
`validator_quorum`. If it is below `quorumThreshold()` but reaches
`tier2Threshold()`, the node labels the agreed set `participant_aligned`.
In both cases, a below-threshold minority can advertise a conflicting or
unacquirable entropy hash without vetoing the aligned cohort.

If no entropy hash reaches the entropy gate threshold before the bounded
deadline, the round must fall back to the Tier 1 consensus-bound digest. This
is the safe degradation path, not a consensus failure.

Examples with five active validators, validator_quorum threshold four, and
participant_aligned threshold four:

- Four honest validators align on one entropy hash and one validator advertises
  a bogus hash: proceed with validator_quorum entropy for the honest quorum.
- Two validators advertise different bogus hashes and only three align on the
  honest hash: fall back to the Tier 1 digest.
- No peer entropy hash is observed in time: fall back to the Tier 1 digest.

The fallback pseudo-transaction is deterministic — every node derives the same
digest from `(HashPrefix::entropyFallback, parentLedgerHash, baseTxSetHash,
seq)` — and labeled with `EntropyTier = consensus_fallback` and
`EntropyCount = 0`. Hooks state their own requirements via the required
`min_tier`/`min_count` arguments to `dice()`/`random()`: a hook that demands
validator-tier entropy fails closed with `TOO_LITTLE_ENTROPY` on fallback
ledgers, while a hook that opts into fallback-grade randomness must do so
explicitly at the call site. The fallback digest derives from the BASE
(pre-injection) tx set hash to avoid circularity, and entropy pseudo-tx
deduplication is value-based: if an explicit-final synthetic set already
contains the exact pseudo-tx, injection skips it; a present-but-different
pseudo-tx is logged as a determinism violation and left in the agreed set.

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
Closed-ledger apply snapshots that collector, so the sidecar convergence state
and the signer set used by `ttEXPORT` stay on the same path.

If the consensus candidate contains a `ttEXPORT` but the node has no eligible
local export signatures yet, the export sidecar gate opens only a bounded
safety window for tx-converged peers to advertise `exportSigSetHash`. This is
not a wait-for-Export-success mechanism; it is a short opportunity to avoid
closing a minority ledger while sidecar convergence is already reachable. If no
advertised sidecar appears by the deadline, the gate stops waiting and the
export retries or expires through normal transaction rules.

Export success requires quorum alignment on `exportSigSetHash`, not merely a
local collector quorum. If a quorum of tx-converged participants advertises the
same export signature sidecar hash, that hash is aligned and below-quorum
conflicts are ignored. If no export signature hash reaches quorum alignment by
the bounded deadline, do not choose the largest non-quorum set; the export
retries or expires according to normal transaction rules.

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

- Does this preserve transaction-set equality as the core consensus identity?
- Does every extension wait have a bounded fallback?
- Does validator_quorum entropy require active-validator quorum alignment?
- Can one bad validator deny entropy to an honest quorum? It must not.
- Can a sub-quorum set produce participant_aligned entropy only after reaching
  the intersection-safe tier2Threshold()?
- Are quorum calculations using the active validator view, not recent
  proposers as the denominator?
- Are sidecar entries typed as sidecars, not pseudo-transactions?
- Are proposal-visible or validation-visible sidecar fields covered by the
  relevant signature and duplicate/replay identity?
- Are export signatures verified before they count?
- Does export success require `exportSigSetHash` alignment, not just local
  collector quorum?
- Can one bad validator deny Export to an honest quorum? It must not.
- Can timeout select a largest-but-below-quorum export sidecar set? It must not.
- Are CE and Export still independently gated and independently stoppable?
