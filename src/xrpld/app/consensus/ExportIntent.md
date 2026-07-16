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

**INV-3 - qC and qV answer different questions.**
qC is the content threshold for one account-selected committee:
`ceil(0.8 * committeeSize)`. The intent cannot lower it. Committee positions
are indexes into the immutable canonical roster.

qV is the root-alignment threshold over the source consensus active-validator
view. It coordinates which complete bounded sidecar union may materialize
witnesses. qV does not decide committee membership, prove target authorization,
or replace local possession and verification of qC signatures.

Neither denominator is derived from locally observed peers or signatures, and
silence never lowers either threshold.

**INV-4 - Honest shares are post-validation attestations.**
A validator signs only after its node accepts the exact source ledger containing
the intent as validated. Closing/building that ledger, emitting the node's own
validation, or advancing the build cursor does not unlock signing. Ledger
building may run ahead of the validated cursor; Export waits without holding
base consensus open.

The release-stamped target binds source domain, target domain, origin
transaction ID `W`, origin ledger sequence, and origin ledger hash in a reserved
final `xahau/export` Memo covered by each ordinary target multisignature. Existing
user Memos remain byte-identical and in their original order. Malformed,
duplicate, misplaced, unsupported, or oversized reserved Memos fail before
signing.

**INV-5 - Live manifests bind committee masters to signing keys.**
The immutable roster names validator master identities. Live share production
and admission use the manifest cache to map a concrete signing key to the
master at its committee position. Missing or ambiguous attribution contributes
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

Activating `featureExport` or `featureConsensusEntropy` switches proposal
participants to extension-aware semantics. A proposal with no populated
sidecar fields may still use the legacy 32-byte serialization, but that compact
message does not restore legacy network semantics.

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
