import XahauConsensus.Threshold

namespace XahauConsensus

/-!
Additional arithmetic facts about the Xahau consensus thresholds.

These lemmas are deliberately small and review-oriented: they expose concrete
edge cases, exact multiples-of-five behavior, participant/quorum band facts,
and monotonicity of the threshold functions.
-/

theorem byzantineBound_zero : byzantineBound 0 = 0 := by
  native_decide

theorem participantThreshold_zero : participantThreshold 0 = 1 := by
  native_decide

theorem quorumThreshold_zero : quorumThreshold 0 = 0 := by
  native_decide

theorem safeQuorumThreshold_zero : safeQuorumThreshold 0 = 1 := by
  native_decide

theorem safeParticipantThreshold_zero : safeParticipantThreshold 0 = 1 := by
  native_decide

theorem byzantineBound_one : byzantineBound 1 = 0 := by
  native_decide

theorem participantThreshold_one : participantThreshold 1 = 1 := by
  native_decide

theorem quorumThreshold_one : quorumThreshold 1 = 1 := by
  native_decide

theorem safeQuorumThreshold_one : safeQuorumThreshold 1 = 1 := by
  native_decide

theorem safeParticipantThreshold_one : safeParticipantThreshold 1 = 1 := by
  native_decide

theorem participantThreshold_two : participantThreshold 2 = 2 := by
  native_decide

theorem quorumThreshold_two : quorumThreshold 2 = 2 := by
  native_decide

theorem participantThreshold_three : participantThreshold 3 = 2 := by
  native_decide

theorem quorumThreshold_three : quorumThreshold 3 = 3 := by
  native_decide

theorem participantThreshold_four : participantThreshold 4 = 3 := by
  native_decide

theorem quorumThreshold_four : quorumThreshold 4 = 4 := by
  native_decide

theorem byzantineBound_five : byzantineBound 5 = 1 := by
  native_decide

theorem participantThreshold_five : participantThreshold 5 = 4 := by
  native_decide

theorem quorumThreshold_five : quorumThreshold 5 = 4 := by
  native_decide

theorem byzantineBound_ten : byzantineBound 10 = 2 := by
  native_decide

theorem participantThreshold_ten : participantThreshold 10 = 7 := by
  native_decide

theorem quorumThreshold_ten : quorumThreshold 10 = 8 := by
  native_decide

theorem byzantineBound_twenty : byzantineBound 20 = 4 := by
  native_decide

theorem participantThreshold_twenty : participantThreshold 20 = 13 := by
  native_decide

theorem quorumThreshold_twenty : quorumThreshold 20 = 16 := by
  native_decide

theorem byzantineBound_five_mul (k : Nat) :
    byzantineBound (5 * k) = k := by
  unfold byzantineBound
  omega

theorem participantThreshold_five_mul (k : Nat) :
    participantThreshold (5 * k) = 3 * k + 1 := by
  unfold participantThreshold byzantineBound
  omega

theorem quorumThreshold_five_mul (k : Nat) :
    quorumThreshold (5 * k) = 4 * k := by
  unfold quorumThreshold
  omega

/-- On exact multiples of five, the strict safety margin is exactly two. -/
theorem participantThreshold_five_mul_margin (k : Nat) :
    2 * participantThreshold (5 * k) =
      (5 * k + byzantineBound (5 * k)) + 2 := by
  rw [participantThreshold_five_mul, byzantineBound_five_mul]
  omega

/-- One below the multiple-of-five participant threshold reaches only equality
with the unsafe boundary, so the strict safety inequality fails. -/
theorem below_participantThreshold_five_mul_hits_boundary (k : Nat) :
    2 * (participantThreshold (5 * k) - 1) =
      5 * k + byzantineBound (5 * k) := by
  rw [participantThreshold_five_mul, byzantineBound_five_mul]
  omega

theorem participantThreshold_five_mul_lt_quorumThreshold_five_mul
    {k : Nat} (h : 1 < k) :
    participantThreshold (5 * k) < quorumThreshold (5 * k) := by
  rw [participantThreshold_five_mul, quorumThreshold_five_mul]
  omega

theorem participantThreshold_five_eq_quorumThreshold_five :
    participantThreshold 5 = quorumThreshold 5 := by
  native_decide

theorem participantThreshold_ten_lt_quorumThreshold_ten :
    participantThreshold 10 < quorumThreshold 10 := by
  native_decide

theorem participant_band_nonempty {count : Nat}
    (h : participantThreshold count < quorumThreshold count) :
    ∃ participants,
      participantThreshold count <= participants ∧
        participants < quorumThreshold count := by
  exact ⟨participantThreshold count, Nat.le_refl _, h⟩

theorem participant_band_empty {count : Nat}
    (h : quorumThreshold count <= participantThreshold count) :
    ¬ ∃ participants,
      participantThreshold count <= participants ∧
        participants < quorumThreshold count := by
  intro hExists
  rcases hExists with ⟨participants, hParticipant, hBelowQuorum⟩
  omega

theorem participant_band_empty_zero :
    ¬ ∃ participants,
      participantThreshold 0 <= participants ∧
        participants < quorumThreshold 0 := by
  apply participant_band_empty
  native_decide

theorem participant_band_empty_one :
    ¬ ∃ participants,
      participantThreshold 1 <= participants ∧
        participants < quorumThreshold 1 := by
  apply participant_band_empty
  native_decide

theorem participant_band_empty_two :
    ¬ ∃ participants,
      participantThreshold 2 <= participants ∧
        participants < quorumThreshold 2 := by
  apply participant_band_empty
  native_decide

theorem participant_band_empty_five :
    ¬ ∃ participants,
      participantThreshold 5 <= participants ∧
        participants < quorumThreshold 5 := by
  apply participant_band_empty
  native_decide

theorem participant_band_nonempty_three :
    ∃ participants,
      participantThreshold 3 <= participants ∧
        participants < quorumThreshold 3 := by
  apply participant_band_nonempty
  native_decide

theorem participant_band_nonempty_four :
    ∃ participants,
      participantThreshold 4 <= participants ∧
        participants < quorumThreshold 4 := by
  apply participant_band_nonempty
  native_decide

theorem participant_band_nonempty_ten :
    ∃ participants,
      participantThreshold 10 <= participants ∧
        participants < quorumThreshold 10 := by
  apply participant_band_nonempty
  native_decide

theorem participant_band_nonempty_five_mul {k : Nat} (h : 1 < k) :
    ∃ participants,
      participantThreshold (5 * k) <= participants ∧
        participants < quorumThreshold (5 * k) := by
  exact participant_band_nonempty
    (participantThreshold_five_mul_lt_quorumThreshold_five_mul h)

theorem byzantineBound_mono {a b : Nat} (h : a <= b) :
    byzantineBound a <= byzantineBound b := by
  unfold byzantineBound
  exact Nat.div_le_div_right h

theorem participantThreshold_mono {a b : Nat} (h : a <= b) :
    participantThreshold a <= participantThreshold b := by
  unfold participantThreshold
  apply Nat.succ_le_succ
  apply Nat.div_le_div_right
  have hByzantine := byzantineBound_mono h
  omega

theorem quorumThreshold_mono {a b : Nat} (h : a <= b) :
    quorumThreshold a <= quorumThreshold b := by
  unfold quorumThreshold
  apply Nat.div_le_div_right
  omega

end XahauConsensus
