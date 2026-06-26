# Export — Design Intent (canonical spine)

This is the **normative** intent for `featureExport`: the invariants that must
hold regardless of how the implementation is refactored. The verbose mechanics
live in `ConsensusExtensionsDesign.md`; the reviewer-facing walkthrough lives in
the PR description. **Both defer to this file.**

How to use it: if code contradicts an invariant below, the *code* is wrong — or
the invariant is being changed and **this file must be consciously edited in the
same change, with the rationale**. In particular, Export is not replay-clean
unless every closed-ledger effect can be rebuilt from the parent ledger and the
closed transaction set alone.

## Purpose (one line)

Export lets a quorum of active validators produce a foreign-chain-submittable
transaction from an agreed `ttEXPORT`, without letting local sidecar timing,
collector state, or validator silence change the closed ledger result.

## Invariants

**INV-1 — Quorum, not unanimity.**
Export success is gated by active-validator quorum alignment on the export
signature set. A missing, delayed, or silent minority must not veto an otherwise
quorum-aligned export.
*Anti-pattern:* requiring full observation of every tx-converged validator before
success.

**INV-2 — Fixed active-view denominator.**
Export thresholds are computed over the parent-ledger active validator view. The
denominator must never be derived from locally observed peers, locally available
signatures, or the subset that happened to advertise sidecar hashes.
*Anti-pattern:* letting silence shrink the quorum threshold.

**INV-3 — Accepted sidecar root, not live collector.**
Any successful Export apply path must use the signature set rooted at the
`exportSigSetHash` accepted by the tick gate. Late local collector arrivals,
timeout flags, fetch completions, or unverified proposal-carried signatures must
not change the signer set selected for the ledger.
*Anti-pattern:* assembling from `ExportSigCollector` at apply time.

**INV-4 — Replay witness in the transaction stream.**
If export sidecar material changes closed-ledger output, that material must be
represented by canonical ledger input before apply. The closed ledger must be
replayable from `(parent ledger, ordered closed transaction set)` without live
consensus sidecar memory. Export signatures are such a witness: if they determine
the shadow-ticket hash or exported result, they must be carried by a replayable
companion pseudo transaction or equivalent transaction-stream artifact.
*Anti-pattern:* using ephemeral accepted sidecar state to create a shadow ticket
or result that cannot be reconstructed by ledger delta replay.

**INV-5 — Store the witness once.**
The signature witness is canonical input; metadata is output. Metadata may carry
hashes and references for client discovery, but it should not duplicate the full
signature payload merely to avoid a client dereference. A convenience RPC may
expand `ttEXPORT + witness` into the foreign-chain-submittable blob on read.
*Anti-pattern:* storing the same validator signatures once as replay input and
again as a full metadata blob without a separate consensus reason.

**INV-6 — Bounded retry window.**
An Export that cannot obtain quorum-aligned signatures within its bounded ledger
window retries or expires through normal transaction semantics. It must not wait
unboundedly, pick the largest sub-quorum set, or finalize against local trusted
configuration as a fallback.

**INV-7 — Shadow tickets are latches, not global tombstones.**
The current shadow-ticket object prevents a different XPOP from consuming the
same live latch, but deletion permits a later re-mint of the same
`(account, ticketSequence)` latch. Replay protection beyond that is a separate
protocol decision, not an implicit property of shadow tickets.

## Current Open Repair

`INV-4` is the open replay repair. The current sidecar-only signature witness is
safe for live consensus, but historical delta replay cannot reconstruct a
successful Export without reacquiring the ledger. The intended fix is to make the
accepted export signature witness a canonical transaction-stream input, then have
metadata reference that witness instead of owning the signature payload.
