import XahauConsensus.Threshold

namespace XahauConsensus

/-!
Review-oriented facts about the tempting `ceil(60%)` participant threshold.

The live `participantThreshold` is one higher than naive 60% at exact
multiples of five. That extra vote is what turns equality at the
Byzantine-overlap boundary into strict intersection safety.
-/

/-- A naive `ceil(0.6 * count)` threshold. -/
def naiveSixtyPercentThreshold (count : Nat) : Nat :=
  (count * 60 + 99) / 100

theorem naiveSixtyPercentThreshold_five_mul (k : Nat) :
    naiveSixtyPercentThreshold (5 * k) = 3 * k := by
  unfold naiveSixtyPercentThreshold
  omega

theorem participantThreshold_five_mul_eq_naiveSixtyPercentThreshold_succ
    (k : Nat) :
    participantThreshold (5 * k) =
      naiveSixtyPercentThreshold (5 * k) + 1 := by
  unfold participantThreshold byzantineBound naiveSixtyPercentThreshold
  omega

/-- At exact multiples of five, naive 60% only reaches the unsafe boundary. -/
theorem naiveSixtyPercentThreshold_five_mul_hits_intersection_boundary
    (k : Nat) :
    2 * naiveSixtyPercentThreshold (5 * k) =
      5 * k + byzantineBound (5 * k) := by
  unfold naiveSixtyPercentThreshold byzantineBound
  omega

theorem naiveSixtyPercentThreshold_five_mul_not_intersection_safe
    (k : Nat) :
    ¬ 5 * k + byzantineBound (5 * k) <
      2 * naiveSixtyPercentThreshold (5 * k) := by
  rw [naiveSixtyPercentThreshold_five_mul_hits_intersection_boundary k]
  omega

theorem participantThreshold_five_mul_intersection_safe (k : Nat) :
    5 * k + byzantineBound (5 * k) <
      2 * participantThreshold (5 * k) := by
  exact participantThreshold_intersection_safe (5 * k)

/-- At exact multiples of five, the live threshold clears the boundary by two. -/
theorem participantThreshold_five_mul_intersection_margin (k : Nat) :
    2 * participantThreshold (5 * k) =
      (5 * k + byzantineBound (5 * k)) + 2 := by
  unfold participantThreshold byzantineBound
  omega

end XahauConsensus
