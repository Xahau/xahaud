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
collector state, or validator silence change the bytes of a successful closed
ledger result.

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
On networks that use NegativeUNL, `featureNegativeUNLActiveViewCap` should be
active before or with Export so producer-side nUNL voting caps against the same
UNLReport active-source universe that bounds NegativeUNL shrink. Export quorum
itself is computed over the effective post-NegativeUNL active view. Direct
Export apply still rebuilds and defensively caps the parent active view before
checking the witness threshold.
*Anti-pattern:* letting silence shrink the quorum threshold.

**INV-3 — Accepted sidecar root, not live collector.**
Any successful Export apply path must use the signature set rooted at the
`exportSigSetHash` accepted by the tick gate. Late local collector arrivals,
timeout flags, or unverified proposal-carried signatures must not change the
signer set selected for the ledger.
*Anti-pattern:* assembling from `ExportSigCollector` at apply time.

**INV-4 — Replay witness in the transaction stream.**
If export sidecar material changes closed-ledger output, that material must be
represented by canonical ledger input before apply. The closed ledger must be
replayable from `(parent ledger, ordered closed transaction set)` without live
consensus sidecar memory. Export signatures are such a witness: they determine
source quorum success and exported-result metadata, so they must be carried by
a replayable companion pseudo transaction or equivalent transaction-stream
artifact. The shadow-ticket intent hash is signature-independent.
*Anti-pattern:* using ephemeral accepted sidecar state to create a shadow ticket
or result that cannot be reconstructed by ledger delta replay.

Current shape: `ttEXPORT_SIGNATURES` is the signature witness interface. It
carries the full source-validator signature witness and binds it to the matching
`ttEXPORT` via `sfTransactionHash`. Network consensus produces it from the
accepted sidecar set; standalone/dev helpers may produce the same witness from
the local validator key. Ledger build pre-scans the ordered transaction stream
into a build-local index, and `ttEXPORT` apply consumes that pre-scanned witness.
The index is not an extra consensus input; it is only an efficient lookup over
the canonical transaction set. In live consensus builds, `onPreBuild` first
removes any pre-existing export witness and re-materializes the witness from the
accepted sidecar root. That witness is the signer-membership source for apply;
current manifest-cache state must not re-decide which accepted signing keys
count. Historical `LedgerReplay` uses the same membership rule because current
manifests may no longer map old rotated signing keys. Both paths still verify
each signature against the inner transaction and require the parent-view
threshold. Direct apply paths that did not run `onPreBuild` remain conservative
and filter witness signers through the live active-validator view.

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

The success-vs-retry decision remains a bounded timing edge, like ordinary
consensus convergence: one node may observe the quorum-aligned witness before
its deadline while another retries. Validation resolves that ledger disagreement.
What must never happen is a "successful" export whose signature bytes come from
live collector state, late proposal arrivals, or a node-local sub-quorum set
instead of the accepted witness in the transaction stream.

**INV-7 — Shadow tickets are latches, not global tombstones.**
The shadow-ticket object binds the canonical target signing intent,
not one authorization-envelope-dependent target transaction ID. Any
destination-valid execution of that exact intent may complete the callback.
Deletion permits a later re-mint of the same `(account, ticketSequence)` latch,
so replay protection beyond the live latch is a separate protocol decision, not
an implicit property of shadow tickets.

Cancellation exists to reclaim the account reserve and bounded outstanding-
ticket slot when a round trip will not complete. It deletes callback readiness;
it does not revoke target-chain signatures already published in proposals. Safe
cleanup therefore depends on external evidence that the capability is no longer
executable or the callback is intentionally abandoned, such as target
`LastLedgerSequence` expiry, destination Ticket consumption, or SignerList
invalidation.

**INV-8 — Export signatures are public capabilities.**
Proposal-carried signature shares may be observed, assembled, and submitted as
soon as destination quorum exists. Source-side witness agreement governs what
Xahau records; it is not a confidentiality or destination-execution gate.
Import therefore waits outside consensus when its shadow ticket does not yet
exist and matches a later XPOP against the signature-independent intent.
The main defense for exposing shares before source finality is authority
equivalence: destination execution must require the same validator-derived
authority that Xahau requires to validate and materialize the Export.
*Anti-pattern:* relying on proposal timing or canonical signer selection to
hide or delay an otherwise valid destination transaction.

**INV-9 — The active source authority must fit the destination protocol.**
Export does not publish shares without a ledger-anchored `UNLReport`, and does
not publish shares or materialize a result when the source validator population
before NegativeUNL filtering exceeds `STTx::maxMultiSigners()`. Silently
selecting a capped subset would replace source-view authority with an implicit
bridge committee. Any future bounded committee must be an explicit, separately
reviewed policy.

**INV-10 — Source and destination authorization must be equivalent.**
The bounded deployment contract uses the same validator-derived key universe
and equivalent weighted threshold for Xahau Export/validation and the target
account's SignerList. A lower destination threshold defeats the pre-finality
share-exposure defense. A higher threshold is safety-conservative but can leave
a successful source latch without enough witness authority to execute.

The target network's ledger-validation quorum is independent: the SignerList
authorizes the account transaction, target consensus validates the containing
ledger, and XPOP later proves that finality. Because an ordinary target
SignerList is static, configure it against the original pre-NegativeUNL source
universe and accept reduced Export liveness during NegativeUNL periods rather
than lowering destination authority.

An optional stronger deployment profile adds a required submitter co-signer held
by the target-submission process. If validator weights total `V`, validator
threshold is `q`, submitter weight is `C`, and target quorum is `C + q > V`,
validator shares alone cannot execute and the submitter still needs validator
weight `q`. It signs only after observing the validated source latch, turning
the submitter into a liveness/censorship dependency rather than a sole safety
authority. The separate key consumes one target SignerList entry, leaving at
most 31 source validator identities under a 32-entry destination limit.

## Replay Witness Shape

The accepted local export sidecar snapshot is not consumed directly by
`Export::doApply`.
Before ledger build, a producer injects one `ttEXPORT_SIGNATURES` pseudo for
each export that has usable signatures. In network mode that producer is the
consensus extension accept path; in standalone/dev mode it can be a local helper. The
pseudo has no ledger-state effect by itself; it is the ledger's replay witness
for the validator signatures.

Metadata stores `sfExportSignatureHash`, a direct reference to the witness
pseudo, rather than duplicating the signature payload as an assembled
`sfExportedTxn` blob. Clients assemble the final foreign-chain transaction from
the original `ttEXPORT` inner transaction plus the witness signatures. Assembly
must follow the same deterministic contract as `ExportResultBuilder`: sort
signers canonically by AccountID, use an empty `SigningPubKey`, and cap the
target-chain `Signers` array at `STTx::maxMultiSigners()` before computing or
submitting the blob. The witness may contain extra source-side signatures that
are valid replay input but are not part of the target-chain blob. Source-chain
export does not prove the destination account's SignerList or quorum policy;
that compatibility is an operator/client contract for the chosen target chain.

This is not an XPOP-style self-contained proof. XPOP embeds its UNL and manifest
bundle because it is imported as external proof material. Export witnesses are
validated-history replay inputs. If we later want trustless historical
re-verification without relying on validated inclusion, the larger design is to
ledger-anchor validator signing-key history (for example via `UNLReport`) or to
embed manifest proof material; that is intentionally out of scope here.
