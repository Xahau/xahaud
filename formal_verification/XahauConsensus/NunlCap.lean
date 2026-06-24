import XahauConsensus.Threshold

namespace XahauConsensus

/-!
Arithmetic facts for nUNL-capped view shrinkage.

The examples here intentionally use the original view for the participant
floor and the effective post-nUNL view for validator quorum.  That is the
cross-view comparison that matters when disabled validators collapse the space
between the Tier-2 participant floor and the Tier-3 validator-quorum floor.
-/

/-- Integer ceiling division, defined defensively for `d = 0`. -/
def ceilDiv (n d : Nat) : Nat :=
  if d = 0 then 0 else (n + d - 1) / d

/-- The protocol's ceil-25% nUNL disablement cap for an original validator view. -/
def disabledCap (originalView : Nat) : Nat :=
  ceilDiv originalView 4

/-- The post-nUNL effective validator view after `disabled` validators drop. -/
def effectiveView (originalView disabled : Nat) : Nat :=
  originalView - disabled

theorem ceilDiv_zero_right (n : Nat) : ceilDiv n 0 = 0 := by
  simp [ceilDiv]

theorem ceilDiv_four_eight : ceilDiv 8 4 = 2 := by
  native_decide

theorem ceilDiv_four_ten : ceilDiv 10 4 = 3 := by
  native_decide

theorem ceilDiv_four_twenty : ceilDiv 20 4 = 5 := by
  native_decide

theorem disabledCap_eight : disabledCap 8 = 2 := by
  native_decide

theorem disabledCap_ten : disabledCap 10 = 3 := by
  native_decide

theorem disabledCap_twenty : disabledCap 20 = 5 := by
  native_decide

theorem effectiveView_eight_at_disabledCap :
    effectiveView 8 (disabledCap 8) = 6 := by
  native_decide

theorem effectiveView_ten_at_disabledCap :
    effectiveView 10 (disabledCap 10) = 7 := by
  native_decide

theorem effectiveView_twenty_at_disabledCap :
    effectiveView 20 (disabledCap 20) = 15 := by
  native_decide

/-- Original 8 with two disabled validators collapses the participant/quorum band. -/
theorem band_collapse_original8_effective6 :
    quorumThreshold 6 = participantThreshold 8 := by
  native_decide

theorem quorum_original8_effective6_meets_participant_floor :
    participantThreshold 8 <= quorumThreshold 6 := by
  native_decide

/-- Original 10 with two disabled validators collapses the participant/quorum band. -/
theorem band_collapse_original10_effective8 :
    quorumThreshold 8 = participantThreshold 10 := by
  native_decide

theorem quorum_original10_effective8_meets_participant_floor :
    participantThreshold 10 <= quorumThreshold 8 := by
  native_decide

/-- Original 10 at the full ceil-25% cap leaves effective view 7, below the participant floor. -/
theorem quorum_original10_effective7_below_participant_floor :
    quorumThreshold 7 < participantThreshold 10 := by
  native_decide

theorem max_cap_original10_below_participant_floor :
    quorumThreshold (effectiveView 10 (disabledCap 10)) <
      participantThreshold 10 := by
  native_decide

/-- At original 20, the full ceil-25% cap leaves effective view 15, which is too small. -/
theorem quorum_original20_effective15_below_participant_floor :
    quorumThreshold 15 < participantThreshold 20 := by
  native_decide

theorem quorum_original20_effective15_does_not_meet_participant_floor :
    ¬ participantThreshold 20 <= quorumThreshold 15 := by
  native_decide

/-- Original 20 with four disabled validators collapses the participant/quorum band. -/
theorem band_collapse_original20_effective16 :
    quorumThreshold 16 = participantThreshold 20 := by
  native_decide

theorem quorum_original20_effective16_meets_participant_floor :
    participantThreshold 20 <= quorumThreshold 16 := by
  native_decide

/-- The ceil-25% cap does not by itself guarantee collapse at size 20. -/
theorem max_cap_original20_below_participant_floor :
    quorumThreshold (effectiveView 20 (disabledCap 20)) <
      participantThreshold 20 := by
  native_decide

/--
General cross-view comparison: an effective-view quorum satisfies the
original-view participant floor whenever that quorum clears the original
intersection boundary.
-/
theorem quorumThreshold_meets_participantThreshold_of_intersection_premise
    {originalView effectiveView : Nat}
    (h :
      originalView + byzantineBound originalView <
        2 * quorumThreshold effectiveView) :
    participantThreshold originalView <= quorumThreshold effectiveView := by
  exact participantThreshold_minimal originalView (quorumThreshold effectiveView) h

/--
Once the effective-view quorum threshold meets the original-view participant
floor, any validator count meeting validator quorum also meets the participant
floor anchored to the original view.
-/
theorem validators_meet_participant_floor_of_meet_quorum
    {originalView effectiveView validators : Nat}
    (hBand : participantThreshold originalView <= quorumThreshold effectiveView)
    (hQuorum : quorumThreshold effectiveView <= validators) :
    participantThreshold originalView <= validators :=
  Nat.le_trans hBand hQuorum

/-- If cross-view quorum is no higher than the participant floor, the in-between band is empty. -/
theorem cross_view_participant_band_empty
    {originalView effectiveView : Nat}
    (hCollapse : quorumThreshold effectiveView <= participantThreshold originalView) :
    ¬ ∃ participants,
      participantThreshold originalView <= participants ∧
        participants < quorumThreshold effectiveView := by
  intro hExists
  rcases hExists with ⟨participants, hParticipant, hBelowQuorum⟩
  omega

end XahauConsensus
