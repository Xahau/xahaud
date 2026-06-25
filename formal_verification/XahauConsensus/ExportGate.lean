namespace XahauConsensus

/-- Minimal model of the sidecar export gate.

`alignedParticipants` is the number of participants observed on the export
sidecar, `quorumThreshold` is the required aligned count, and
`fullObservation` records whether every participant was observed. The C++ gate
must use quorum alignment for success; full observation is only diagnostic.
-/
structure ExportGate where
  alignedParticipants : Nat
  quorumThreshold : Nat
  fullObservation : Bool
  deriving DecidableEq, Repr

/-- Export sidecar-gate outcome. This is not the final `Export::doApply`
result: closed-ledger apply re-validates the frozen agreed signature snapshot
before it can create a shadow ticket. -/
inductive ExportOutcome where
  | proceed
  | retryOrExpire
  deriving DecidableEq, Repr

/-- The success predicate used by export: enough participants are aligned. -/
def ExportGate.quorumAligned (gate : ExportGate) : Bool :=
  decide (gate.quorumThreshold <= gate.alignedParticipants)

/-- Export proceeds exactly when quorum alignment is met. -/
def ExportGate.proceed (gate : ExportGate) : Bool :=
  gate.quorumAligned

/-- Export's externally visible decision shape. -/
def ExportGate.outcome (gate : ExportGate) : ExportOutcome :=
  if gate.proceed then ExportOutcome.proceed else ExportOutcome.retryOrExpire

/-- Minimal model of the additional closed-ledger apply preconditions.

The sidecar gate only proves that one `exportSigSetHash` had quorum alignment.
Network-mode `Export::doApply` then independently requires a ledger-anchored
validator view, no convergence failure for the round, a frozen agreed sidecar
map, a parseable/valid signature set, and enough verified signers in that map.
The model intentionally excludes cryptography and metadata construction; it
exists to prevent reading `ExportGate.proceed` as final apply success.
-/
structure ExportApplySnapshot where
  fromUNLReport : Bool
  convergenceFailed : Bool
  agreedSetPresent : Bool
  agreedSetValid : Bool
  signerCount : Nat
  quorumThreshold : Nat
  deriving DecidableEq, Repr

/-- Closed-ledger apply can use only a valid, frozen agreed sidecar snapshot. -/
def ExportApplySnapshot.validAgreedSnapshot
    (snapshot : ExportApplySnapshot) : Bool :=
  snapshot.fromUNLReport &&
    !snapshot.convergenceFailed &&
    snapshot.agreedSetPresent &&
    snapshot.agreedSetValid &&
    decide (snapshot.quorumThreshold <= snapshot.signerCount)

/-- Minimal network-mode apply decision: valid agreed snapshot applies; all
other cases retry or expire. -/
def ExportApplySnapshot.outcome
    (snapshot : ExportApplySnapshot) : ExportOutcome :=
  if snapshot.validAgreedSnapshot then
    ExportOutcome.proceed
  else
    ExportOutcome.retryOrExpire

theorem apply_success_iff_valid_agreed_snapshot
    (snapshot : ExportApplySnapshot) :
    snapshot.outcome = ExportOutcome.proceed ↔
      snapshot.validAgreedSnapshot = true := by
  unfold ExportApplySnapshot.outcome
  by_cases h : snapshot.validAgreedSnapshot <;> simp [h]

/-- Gate success alone is not final apply success. For example, the sidecar
gate may have quorum alignment while the final apply path has no frozen agreed
sidecar map available and therefore retries. -/
theorem gate_proceed_does_not_imply_apply_success :
    ∃ gate : ExportGate, ∃ snapshot : ExportApplySnapshot,
      ExportGate.proceed gate = true ∧
        ExportApplySnapshot.outcome snapshot =
          ExportOutcome.retryOrExpire := by
  refine ⟨
    ExportGate.mk 4 4 false,
    ExportApplySnapshot.mk true false false true 4 4,
    ?_,
    ?_⟩ <;> rfl

/-- A missing minority, represented by `fullObservation = false`, does not
prevent export when the quorum threshold is met. -/
theorem missing_minority_does_not_prevent_proceed
    {alignedParticipants quorumThreshold : Nat}
    (hQuorum : quorumThreshold <= alignedParticipants) :
    (ExportGate.mk alignedParticipants quorumThreshold false).proceed = true := by
  unfold ExportGate.proceed ExportGate.quorumAligned
  simp [hQuorum]

theorem missing_minority_proceeds
    {alignedParticipants quorumThreshold : Nat}
    (hQuorum : quorumThreshold <= alignedParticipants) :
    (ExportGate.mk alignedParticipants quorumThreshold false).outcome =
      ExportOutcome.proceed := by
  unfold ExportGate.outcome
  simp [missing_minority_does_not_prevent_proceed hQuorum]

/-- Export must not proceed below the aligned-participant quorum threshold. -/
theorem below_quorum_does_not_proceed
    {alignedParticipants quorumThreshold : Nat}
    (fullObservation : Bool)
    (hBelow : alignedParticipants < quorumThreshold) :
    (ExportGate.mk alignedParticipants quorumThreshold fullObservation).proceed =
      false := by
  unfold ExportGate.proceed ExportGate.quorumAligned
  simp [Nat.not_le_of_gt hBelow]

/-- Below quorum, export retries or expires. There is no deterministic fallback
signature set analogous to RNG's Tier 1 fallback digest. -/
theorem below_quorum_retries_or_expires
    {alignedParticipants quorumThreshold : Nat}
    (fullObservation : Bool)
    (hBelow : alignedParticipants < quorumThreshold) :
    (ExportGate.mk alignedParticipants quorumThreshold fullObservation).outcome =
      ExportOutcome.retryOrExpire := by
  unfold ExportGate.outcome
  simp [below_quorum_does_not_proceed fullObservation hBelow]

/-- Flipping only the diagnostic `fullObservation` field cannot change the
export decision. -/
theorem changing_fullObservation_alone_does_not_change_proceed
    (alignedParticipants quorumThreshold : Nat) :
    (ExportGate.mk alignedParticipants quorumThreshold true).proceed =
      (ExportGate.mk alignedParticipants quorumThreshold false).proceed := by
  rfl

end XahauConsensus
