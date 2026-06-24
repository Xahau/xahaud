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

/-- Export has no RNG-style deterministic fallback value. Below quorum it can
only retry/expire; it must not proceed against local-only signature material. -/
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
