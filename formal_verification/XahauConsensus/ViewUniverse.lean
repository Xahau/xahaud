import XahauConsensus.ThresholdFacts

namespace XahauConsensus

/-!
Concrete arithmetic examples for the distinction between the active effective
view, the original pre-nUNL view, and any larger trusted counting universe.

The safety shape is deliberately Nat-only: two cohorts of size `threshold` in
an `activeView` overlap strictly beyond the Byzantine bound charged to
`byzantineUniverse` when

  `activeView + byzantineBound byzantineUniverse < 2 * threshold`.
-/

def strictIntersectionSafe
    (activeView byzantineUniverse threshold : Nat) : Prop :=
  activeView + byzantineBound byzantineUniverse < 2 * threshold

/-- Strict intersection safety plus reachability of the threshold inside the
active view. This separates "safe if it happens" from "possible to happen". -/
def nonvacuousStrictIntersectionSafe
    (activeView byzantineUniverse threshold : Nat) : Prop :=
  threshold <= activeView ∧ strictIntersectionSafe activeView byzantineUniverse threshold

/-- Cross-view Tier-2 band: participant floor is anchored to the original view,
validator quorum to the effective view. -/
def participantBandNonempty
    (effectiveView originalView : Nat) : Prop :=
  ∃ participants,
    participantThreshold originalView <= participants ∧
      participants < quorumThreshold effectiveView

theorem participantBandNonempty_iff
    (effectiveView originalView : Nat) :
    participantBandNonempty effectiveView originalView ↔
      participantThreshold originalView < quorumThreshold effectiveView := by
  constructor
  · intro h
    rcases h with ⟨participants, hParticipant, hBelowQuorum⟩
    omega
  · intro h
    exact ⟨participantThreshold originalView, Nat.le_refl _, h⟩

/-- The original-view participant threshold remains safe when nUNL shrinks the
active effective view. -/
theorem original_threshold_safe_under_nunl_shrink
    {originalView effectiveView : Nat}
    (hShrink : effectiveView <= originalView) :
    strictIntersectionSafe
      effectiveView
      originalView
      (participantThreshold originalView) := by
  unfold strictIntersectionSafe
  exact participantThreshold_safe_under_effective_shrink
    originalView
    effectiveView
    hShrink

theorem original_threshold_nonvacuous_under_nunl_shrink
    {originalView effectiveView : Nat}
    (hShrink : effectiveView <= originalView)
    (hReachable : participantThreshold originalView <= effectiveView) :
    nonvacuousStrictIntersectionSafe
      effectiveView
      originalView
      (participantThreshold originalView) := by
  constructor
  · exact hReachable
  · exact original_threshold_safe_under_nunl_shrink hShrink

/-- The original-view threshold is also safe if the Byzantine counting universe
is no larger than the original view. -/
theorem original_threshold_safe_for_no_larger_counting_universe
    {originalView effectiveView countingUniverse : Nat}
    (hShrink : effectiveView <= originalView)
    (hCounting : countingUniverse <= originalView) :
    strictIntersectionSafe
      effectiveView
      countingUniverse
      (participantThreshold originalView) := by
  unfold strictIntersectionSafe
  have hOriginal :=
    participantThreshold_safe_under_effective_shrink
      originalView
      effectiveView
      hShrink
  have hBound := byzantineBound_mono hCounting
  omega

/-- Any threshold at or below the overlap boundary is not strictly safe. -/
theorem not_strictIntersectionSafe_of_threshold_le_boundary
    {activeView byzantineUniverse threshold : Nat}
    (hBoundary : 2 * threshold <= activeView + byzantineBound byzantineUniverse) :
    ¬ strictIntersectionSafe activeView byzantineUniverse threshold := by
  unfold strictIntersectionSafe
  omega

/-- If the effective-view threshold is below what the original Byzantine bound
requires, it cannot prove strict intersection safety against that original
bound. -/
theorem effective_threshold_not_safe_against_original_bound
    {originalView effectiveView : Nat}
    (hBelow :
      participantThreshold effectiveView <
        (effectiveView + byzantineBound originalView) / 2 + 1) :
    ¬ strictIntersectionSafe
      effectiveView
      originalView
      (participantThreshold effectiveView) := by
  apply not_strictIntersectionSafe_of_threshold_le_boundary
  exact below_threshold_not_safe_for_boundary
    (effectiveView + byzantineBound originalView)
    (participantThreshold effectiveView)
    hBelow

/-- A larger trusted counting universe increases the Byzantine side of the
boundary, eroding the strict-intersection margin. -/
theorem original_boundary_le_trusted_superset_boundary
    {originalView effectiveView trustedUniverse : Nat}
    (hSuperset : originalView <= trustedUniverse) :
    effectiveView + byzantineBound originalView <=
      effectiveView + byzantineBound trustedUniverse := by
  have hBound := byzantineBound_mono hSuperset
  omega

/-- Concrete nUNL example: `originalView = 10`, `effectiveView = 8`, and the
original threshold still clears the original Byzantine bound. -/
theorem original_ten_effective_eight_original_threshold_safe :
    strictIntersectionSafe 8 10 (participantThreshold 10) := by
  unfold strictIntersectionSafe
  native_decide

theorem original_ten_effective_eight_participant_band_empty :
    ¬ participantBandNonempty 8 10 := by
  rw [participantBandNonempty_iff]
  native_decide

theorem original_ten_effective_eight_original_threshold_reachable :
    nonvacuousStrictIntersectionSafe 8 10 (participantThreshold 10) := by
  apply original_threshold_nonvacuous_under_nunl_shrink
  · native_decide
  · native_decide

/-- Concrete regression: for `originalView = 10` and `effectiveView = 8`, the
effective threshold does not strictly clear the original Byzantine bound. -/
theorem original_ten_effective_eight_effective_threshold_not_safe :
    ¬ strictIntersectionSafe 8 10 (participantThreshold 8) := by
  apply not_strictIntersectionSafe_of_threshold_le_boundary
  native_decide

/-- The same failure as a direct boundary comparison, useful when reviewing the
raw arithmetic. -/
theorem original_ten_effective_eight_effective_threshold_hits_boundary :
    2 * participantThreshold 8 <= 8 + byzantineBound 10 := by
  native_decide

/-- Larger concrete nUNL example with the original threshold anchored at
`20`. -/
theorem original_twenty_effective_sixteen_original_threshold_safe :
    strictIntersectionSafe 16 20 (participantThreshold 20) := by
  unfold strictIntersectionSafe
  native_decide

theorem original_twenty_effective_sixteen_participant_band_empty :
    ¬ participantBandNonempty 16 20 := by
  rw [participantBandNonempty_iff]
  native_decide

theorem original_twenty_effective_fifteen_participant_band_empty :
    ¬ participantBandNonempty 15 20 := by
  rw [participantBandNonempty_iff]
  native_decide

theorem original_twenty_effective_fifteen_original_threshold_reachable :
    nonvacuousStrictIntersectionSafe 15 20 (participantThreshold 20) := by
  apply original_threshold_nonvacuous_under_nunl_shrink
  · native_decide
  · native_decide

/-- With `originalView = 20` and `effectiveView = 16`, using the effective
threshold again reaches the unsafe boundary. -/
theorem original_twenty_effective_sixteen_effective_threshold_not_safe :
    ¬ strictIntersectionSafe 16 20 (participantThreshold 16) := by
  apply not_strictIntersectionSafe_of_threshold_le_boundary
  native_decide

/-- Counting Byzantine stake over a trusted universe of `20` instead of the
original view of `10` erodes the margin all the way to equality. -/
theorem trusted_superset_twenty_erodes_original_ten_margin_to_boundary :
    2 * participantThreshold 10 = 10 + byzantineBound 20 := by
  native_decide

/-- The equality above means the original threshold for `10` is not strictly
safe if Byzantine weight is counted over the larger trusted universe `20`. -/
theorem trusted_superset_twenty_original_ten_threshold_not_safe :
    ¬ strictIntersectionSafe 10 20 (participantThreshold 10) := by
  apply not_strictIntersectionSafe_of_threshold_le_boundary
  native_decide

end XahauConsensus
