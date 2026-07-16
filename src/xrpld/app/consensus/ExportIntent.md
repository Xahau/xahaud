# Export - Design Intent (canonical spine)

This is the normative intent for `featureExport`: the invariants that must hold
regardless of implementation refactoring. Detailed mechanics live in
`ConsensusExtensionsDesign.md`; both documents must change with any deliberate
protocol change.

Export is not replay-clean unless every closed-ledger effect can be rebuilt
from the parent ledger and ordered closed transaction set alone.

## Purpose

Export lets an account choose a bounded validator committee whose members sign
an ordinary target-chain transaction only after the exact source intent has
validated. A later source-ledger witness records the sufficient signatures.

## Invariants

**INV-1 - Source intent precedes authority release.**
`ttEXPORT` first validates its target payload, committee reference, source
account, destination TicketSequence, reserved origin Memo, and bounded timing
fields. Successful apply creates a durable origin-keyed latch. It does not
claim that signatures or a witness already exist.

The target must be foreign. An explicit target `sfNetworkID` equal to the
source network is rejected. When the source network itself uses transaction
encoding without `sfNetworkID`, an absent target `sfNetworkID` is also rejected
because it is ambiguous with self-targeting.

The outer transaction's mandatory `sfLastLedgerSequence` bounds only intent
admission. Successful admission starts a separate publication lifetime on the
new latch; queue delay before admission never consumes that publication window.

*Anti-pattern:* treating provisional open-ledger execution or source
transaction-set agreement as authorization to release destination signatures.

**INV-2 - Committee selection is explicit and account-owned.**
An Export committee is an immutable canonical roster of at most 32 validator
master keys stored in an account-owned `ltEXPORT_COMMITTEE`. Its identity is a
network-neutral content digest over that roster. A latch stores only the
digest; committee size and qC are derived from the referenced object.

There is no protocol-default committee. A bare setup transaction may pre-stage
a structurally valid future roster without creating a latch or checking current
membership. Every actual Export intent, including inline create-and-use,
requires every roster master to appear in the exact admitting parent ledger's
pre-NegativeUNL `UNLReport`; otherwise it returns
`tecEXPORT_COMMITTEE_UNAVAILABLE`. A client may offer "copy current UNLReport"
as an explicit template, but the protocol never silently selects it.

Committee deletion is rejected while the owner has any live Export latch. This
coarse rule keeps every accepted witness replayable without storing the roster
again in every latch or maintaining a metadata-expensive reference count. The
deletion guard and witness apply's committee lookup are one load-bearing
invariant: do not weaken the guard to pending-only while latches retain only a
committee digest.

A live Export latch is itself a non-deletable account obligation: an account
owning any latch cannot be deleted. The committee SLE is otherwise a deletable
owned entry. Its required survival while latches exist comes from the explicit
committee-deletion guard together with the account-level latch obligation, not
from making the committee intrinsically non-deletable.

**INV-3 - qC and qV answer different questions.**
qC is the content threshold for one account-selected committee:
`ceil(0.8 * committeeSize)`. The intent cannot lower it. Committee positions
are indexes into the immutable canonical roster.

qV is `safeQuorumThreshold(N)` over the exact parent-ledger effective active
validator view: `ceil(0.8 * N)` when `N > 0`, otherwise the fail-closed floor
of one. The view is the parent `UNLReport` active set after NegativeUNL
subtraction. qV coordinates which complete bounded sidecar union may
materialize witnesses. It does not decide committee membership, prove target
authorization, or replace local possession and verification of qC signatures.

Neither denominator is derived from locally observed peers or signatures, and
silence never lowers either threshold.

**INV-4 - Honest shares are post-validation attestations.**
The local producer signs only after its node accepts the exact source ledger
containing the intent as validated. Closing or building that ledger, emitting
the node's own validation, or advancing the build cursor does not unlock
signing. Ledger building may run ahead of the validated cursor; Export waits
without holding base consensus open.

Signature verification proves identity and content, not release time. The
enforced receiving-side rule is therefore separate: a share contributes to the
union only after its origin resolves against that node's validated state.
Shares for provisional or otherwise unvalidated origins contribute nothing,
regardless of transport or signature validity.

The release-stamped target binds source domain, target domain, origin
transaction ID `W`, origin ledger sequence, and origin ledger hash in a reserved
final `xahau/export` Memo covered by each ordinary target multisignature. Existing
user Memos remain byte-identical and in their original order. Malformed,
duplicate, misplaced, unsupported, or oversized reserved Memos fail before
signing.

**INV-5 - Live manifests bind committee masters to signing keys.**
The immutable roster names validator master identities. A local producer finds
its position using its configured validator master key and signs with its
configured current validation signing key. Every locally produced or remotely
received share then passes the same admission path, where the live manifest
cache must map that concrete signing key to the roster master at the claimed
committee position. Missing, ambiguous, or mismatched attribution contributes
no share. Manifests do not redefine the stored committee and are never read by
accepted witness apply or historical replay.

Target SignerList compatibility with current validator signing-key AccountIDs
is an operator/client responsibility. Source consensus cannot inspect or prove
that remote configuration.

**INV-6 - Align the accepted sidecar root, not the live collector.**
Valid shares from the direct post-validation relay and signed proposal carriage
join one bounded, commutative collector keyed by `(W, committeePosition)`. A
second distinct valid contribution at one position is conflicting and that
position contributes nothing to qC.

The complete bounded admitted-share union is content-addressed and its root is
advertised in signed extended positions. In network mode, witness
materialization uses only the local node's own currently published root after
qV alignment. qV is a go/no-go gate for that root; it never selects a root from
peer positions. The local node must possess a SIDECAR map with that exact root
and independently verify its content addresses, leaf type and required fields,
origin-relative committee positions, unique positions and signer AccountIDs,
signature bounds, and every target multisignature before qC can materialize.
Standalone test execution substitutes exact possession of its locally verified
map; intent admission still requires the UNLReport-backed parent state. Late
collector arrivals cannot mutate the accepted root.

Materialization also requires the consensus parent to descend from the node's
validated ledger, the origin ledger hash to agree through both ancestry views,
and the exact origin ledger and `ttEXPORT` transaction to be locally available.
If any of that validated possession is absent, the latch remains pending; a
build cursor ahead of validation never forces witness production.

*Anti-pattern:* assembling from the current collector at apply time or treating
peer root support as remote payload availability.

**INV-7 - Ledger-defining signatures enter the transaction stream.**
Live transaction-set membership grants no authority to mint an Export witness.
Live-set preparation and `onPreBuild` discard every supplied
`ttEXPORT_SIGNATURES`; only the local deterministic builder may derive the
synthetic stream from the accepted sidecar root and immutable parent. For each
eligible latch that reaches qC it emits zero or one canonical later-ledger
witness, with zero Account, Sequence, and Fee, no authorization fields, and
`sfLedgerSequence` equal to the ledger being built. The witness contains the
exact release-stamped target, committee-relative contributor bitmap, and
signature entries bound to `W`.

Witness apply validates durable evidence against the immutable parent: exact
pseudo ledger sequence, stamp and latch identity, normalized intent digest,
parent committee, contributor bitmap and qC, unique signer accounts, and every
target signature. It does not consult mutable manifest state. Only after that
does apply consult the evolving view for a state transition. If the latch still
exists and its publication deadline has not passed, apply records the witness
transaction ID and removes the pending-work link, or erases the latch if XPOP
was already recorded. If an earlier same-ledger erase or XPOP removed it, or
the deadline has passed, fully valid evidence remains an evidence-only
`tesSUCCESS`; malformed evidence still fails.

Sidecar memory is deliberation state, not replay input. Historical replay never
regenerates or strips persisted witnesses; it applies the same ordered bytes
against the same parent state. The witness bytes therefore do not independently
prove historical master-to-signing-key bindings. Validated witness inclusion
attests that live materializing validators checked those bindings before the
witness entered the ledger; replay rechecks the durable signatures and
committee-relative shape.

**INV-8 - Store the signature evidence once.**
The full release-stamped payload and signatures live in the witness transaction.
The latch stores only the witness transaction reference. Read-time tooling may
expand that canonical witness into the ordinary foreign-chain transaction, but
state and metadata must not duplicate its signature payload merely for client
convenience.

Assembly follows ordinary multisign rules: empty `SigningPubKey`, canonical
signer AccountID ordering, no duplicate signer accounts, and at most
`STTx::maxMultiSigners()` entries.

**INV-9 - Export latches name issuance, not one assembled target hash.**
The latch key is `(owner, origin transaction ID W)`. Its `sfDigest` commits to
the normalized identity-form target, not a signer-subset-dependent final target
transaction ID. Any valid qC subset for the exact stamped intent may therefore
execute and return without orphaning source state.

At most one live latch may name a given `(owner, destination TicketSequence)`,
even when distinct outer transactions would produce distinct `W` values. A
retained canceled or publication-expired latch therefore blocks another
issuance for that TicketSequence until terminal erasure. Reissuing the same
normalized intent after erasure creates a new outer transaction and new `W`;
it never recreates the old latch. A client must not reissue a destination Ticket
that has already been consumed.

Witness and XPOP are independent monotonic facts. Whichever arrives second
symmetrically erases the latch and releases reserve. Flagless lifecycle control
names exact `W`, unlinks pending work, and retains callback readiness because
published signatures cannot be revoked. `tfExportEraseLatch` explicitly erases
that latch and knowingly forfeits a later callback. Publication expiry follows
the non-revoking retain path. v1 has no permanent tombstones or paid bump.

**INV-10 - Released signatures are public capabilities.**
After exact source validation, each admitted share is immediately available on
the `export_signatures` subscription and may also ride proposals. Anyone may
assemble and submit a target transaction once the destination SignerList's
actual threshold is met. The source witness is durable evidence, not a secrecy
or submission gate.

An operator signer may be required by a destination SignerList for assembly
control or defense in depth, but it is not a protocol prerequisite and cannot
replace qC. Reserving such an entry reduces the validator committee that fits
the target's 32-signer limit.

Extension semantics are latched once per round from the exact consensus parent
ledger. A ledger that activates `featureExport` or `featureConsensusEntropy`
does not retroactively enable that extension in the same round; the following
parent-enabled round does. `ttEXPORT`, `ttEXPORT_SIGNATURES`, share production,
carriage, alignment, materialization, and ticket callbacks fail closed or
become no-ops while Export is disabled; callbacks additionally require Import.
Starting a disabled-parent round clears cached Export collector material and
the accepted root so pre-activation evidence cannot cross the boundary.

A proposal with no populated sidecar fields may still use the legacy 32-byte
serialization, but that compact message is only a wire-format case and never
restores legacy network semantics after either extension is active.

**INV-11 - The destination trust claim is configuration-specific.**
XRPL verifies ordinary keys and signatures, not Xahau finality, committee
eligibility, or qV. The account operator must configure its SignerList so the
chosen committee identities and weight threshold match the intended custody
claim. A lower remote threshold weakens that claim; a higher threshold can
strand an otherwise sufficient source witness.

The target network's ledger-validation quorum is separate. It validates the
containing target ledger, and XPOP proves that target finality on return.

**INV-12 - Work, storage, and waiting are bounded.**
Committee size, account latches, global pending work, per-round lanes, relay
entries, message bytes, sidecar leaves, publication windows, and witness
signatures all have hard caps. Export may use one fixed sidecar alignment
window after ordinary transaction-set convergence, but timeout always permits
base consensus to continue. Below-qC or unaligned work remains pending until
witness, cancellation, explicit erase, or publication expiry.

The admission and publication clocks are distinct. The outer
`sfLastLedgerSequence` follows ordinary inclusive transaction admission and may
be at most `maxAdmissionWindowLedgers` beyond the ledger evaluating the intent.
On successful apply, the latch stores an independent inclusive publication
deadline of `admittingLedger + maxPublicationLedgers`; it never copies the
outer deadline. That final candidate ledger may still publish and materialize a
witness. After it, no new witness material is eligible. Later paid Export work
deterministically but lazily removes the expired pending-work link. Expiry does
not revoke released signatures or erase the latch: owner reserve and callback
readiness remain until symmetric completion or explicit erase.

Hook emission is a first-class but separately bounded intent source. One Hook
execution may reserve at most `maxExportsPerHook` exports (currently two), and
those wrappers also consume the normal emitted-transaction allowance. An
emitted `ttEXPORT` must carry an intent and reference a pre-existing committee
digest; it cannot inline-create a committee roster. The protocol constant and
the Hook ABI's `max_export` constant must remain equal.

**INV-13 - Return callbacks remain owner-authorized Imports.**
An XPOP whose proven target transaction carries `sfTicketSequence` takes the
Export callback path only while both Import and Export are enabled. The outer
Import is authorized normally, and its `sfAccount` must equal the proven target
transaction's `sfAccount`, which Export admission already bound to the exporter
and latch owner. Possession of an XPOP alone never authorizes another account's
callback.

The callback may omit the burn-to-mint `sfOperationLimit` and outer/inner
signing-key-equality checks because the validator-multisigned target and Export
latch replace those bindings. It still requires a fully canonical target
signature and the ordinary Import proof: a recognized, currently valid
validator list, its quorum validations, and the publisher-sequence
anti-downgrade rule. A proved target `tes` or `tec` result may drive the
callback; this reports final target execution, not only target success.

The canonical reserved Memo must name this source domain, the target
transaction's encoded domain, exact origin `W`, and an anchor sequence earlier
than the callback ledger. The owner-keyed `(owner, W)` latch must exist and its
stored origin sequence and normalized target digest must match. A future anchor
or not-yet-visible latch returns retryable `telEXPORT_LATCH_REQUIRED`; malformed
or mismatched identity is permanently malformed; a latch that already records
XPOP returns `tecDUPLICATE`. Apply ratchets the accepted validator-list sequence
before recording XPOP on the latch. It performs no burn-to-mint credit or
account creation; the existing owner account's Hook observes the proved result.

## Replay Witness Shape

`ttEXPORT_SIGNATURES` is a later-ledger protocol pseudo transaction. It is a
self-contained source-history record of the exact target bytes signed and the
committee-relative sufficient signature set. The accepted sidecar root selects
the material before injection; closed-ledger apply sees only transaction bytes
and parent ledger state.

The witness is not an XPOP-style proof that unmodified XRPL can interpret, nor
is it a self-verifying archive of historical manifest state. Validated
inclusion is the source-chain attestation that live membership attribution was
checked. XPOP remains the reverse proof: it demonstrates that one assembled
target transaction reached target finality and drives the Hook callback against
the matching origin latch.
