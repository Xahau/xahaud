import XahauConsensus.Threshold
import XahauConsensus.EntropySelector
import XahauConsensus.ExportGate

namespace XahauConsensus

/-!
Small cross-module invariants that state the design contract in one place.

These do not verify C++ directly. They pin the consensus arguments that the C++
is intended to implement.
-/

/-- Same-count band fact: with both thresholds computed from one view size,
Tier 2 is never stricter than validator quorum. Production nUNL rounds use
cross-view thresholds instead; see `entropyGateThresholdForView`. -/
theorem same_count_tier2_not_stricter_than_validator_quorum (count : Nat) :
    safeParticipantThreshold count <= safeQuorumThreshold count :=
  safeParticipantThreshold_le_safeQuorumThreshold count

/-- Same-view shorthand: the live entropy gate is the weaker of Tier 2 and
validator quorum, so it is never above validator quorum. -/
def entropyGateThresholdModel (count : Nat) : Nat :=
  min (safeQuorumThreshold count) (safeParticipantThreshold count)

theorem entropy_gate_le_validator_quorum (count : Nat) :
    entropyGateThresholdModel count <= safeQuorumThreshold count := by
  unfold entropyGateThresholdModel
  exact Nat.min_le_left _ _

theorem entropy_gate_le_participant_threshold (count : Nat) :
    entropyGateThresholdModel count <= safeParticipantThreshold count := by
  unfold entropyGateThresholdModel
  exact Nat.min_le_right _ _

/-- Production shape: validator quorum is over the effective post-nUNL view,
while Tier 2 is over the original pre-nUNL view. -/
def entropyGateThresholdForView (effectiveView originalView : Nat) : Nat :=
  min (safeQuorumThreshold effectiveView) (safeParticipantThreshold originalView)

theorem entropy_gate_for_view_le_validator_quorum
    (effectiveView originalView : Nat) :
    entropyGateThresholdForView effectiveView originalView <=
      safeQuorumThreshold effectiveView := by
  unfold entropyGateThresholdForView
  exact Nat.min_le_left _ _

theorem entropy_gate_for_view_le_participant_threshold
    (effectiveView originalView : Nat) :
    entropyGateThresholdForView effectiveView originalView <=
      safeParticipantThreshold originalView := by
  unfold entropyGateThresholdForView
  exact Nat.min_le_right _ _

/-- The entropy gate is exactly the selector's non-fallback boundary: reaching
the lower of the validator-quorum and participant-aligned thresholds is enough
to select a non-fallback tier, and below it the selector falls back. -/
theorem selectEntropyTier_nonfallback_iff_entropy_gate
    (participantCount effectiveView originalView : Nat) :
    selectEntropyTier true participantCount effectiveView originalView ≠
        EntropyTier.consensusFallback ↔
      entropyGateThresholdForView effectiveView originalView <=
        participantCount := by
  unfold selectEntropyTier entropyGateThresholdForView
  by_cases hQuorum : safeQuorumThreshold effectiveView <= participantCount
  · constructor
    · intro _
      exact Nat.le_trans (Nat.min_le_left _ _) hQuorum
    · intro _
      simp [hQuorum]
  · by_cases hParticipant :
      safeParticipantThreshold originalView <= participantCount
    · constructor
      · intro _
        exact Nat.le_trans (Nat.min_le_right _ _) hParticipant
      · intro _
        simp [hQuorum, hParticipant]
    · constructor
      · intro hNonfallback
        simp [hQuorum, hParticipant] at hNonfallback
      · intro hGate
        have hBelowQuorum :
            participantCount < safeQuorumThreshold effectiveView :=
          Nat.lt_of_not_ge hQuorum
        have hBelowParticipant :
            participantCount < safeParticipantThreshold originalView :=
          Nat.lt_of_not_ge hParticipant
        have hBelowGate :
            participantCount <
              min (safeQuorumThreshold effectiveView)
                (safeParticipantThreshold originalView) :=
          (Nat.lt_min).mpr ⟨hBelowQuorum, hBelowParticipant⟩
        exact False.elim (Nat.not_lt_of_ge hGate hBelowGate)

/-- Until the view is ledger-anchored, entropy tier labeling fails closed. -/
theorem non_unl_report_cannot_mint_nonfallback
    (participantCount effectiveView originalView : Nat) :
    selectEntropyTier false participantCount effectiveView originalView =
      EntropyTier.consensusFallback :=
  no_unl_report_selects_fallback participantCount effectiveView originalView

/-- Export success is a quorum-alignment property, not a full-observation
property. -/
theorem export_success_independent_of_full_observation
    (alignedParticipants quorumThreshold : Nat) :
    (ExportGate.mk alignedParticipants quorumThreshold true).proceed =
      (ExportGate.mk alignedParticipants quorumThreshold false).proceed :=
  changing_fullObservation_alone_does_not_change_proceed
    alignedParticipants
    quorumThreshold

end XahauConsensus
