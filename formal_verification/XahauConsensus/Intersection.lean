import XahauConsensus.Threshold

namespace XahauConsensus

/-!
Abstract cardinality arithmetic for quorum intersection arguments.

The variables are plain natural-number cardinalities:

* `n`: universe size
* `a`, `b`: cohort sizes
* `o`: overlap size
* `t`: quorum threshold
* `f`: tolerated faulty overlap

The shape `a + b <= n + o` captures the inclusion-exclusion upper bound
without committing to a concrete `Finset` model.
-/

/-- If two threshold-sized cohorts fit in an `n`-sized universe only by
overlapping by `o`, and `n + f < 2 * t`, then the overlap is larger than the
fault bound `f`. -/
theorem overlap_gt_fault_of_two_threshold_cohorts
    {n a b o t f : Nat}
    (hCardinality : a + b <= n + o)
    (hA : t <= a)
    (hB : t <= b)
    (hSafety : n + f < 2 * t) :
    f < o := by
  omega

/-- Reviewer-facing contrapositive form: if the overlap is no larger than the
fault bound, then under the strict safety inequality the two cohorts cannot
both meet threshold. -/
theorem not_both_threshold_cohorts_of_overlap_le_fault
    {n a b o t f : Nat}
    (hOverlap : o <= f)
    (hCardinality : a + b <= n + o)
    (hSafety : n + f < 2 * t) :
    ¬ (t <= a ∧ t <= b) := by
  intro hBoth
  have hStrict :
      f < o :=
    overlap_gt_fault_of_two_threshold_cohorts
      hCardinality hBoth.1 hBoth.2 hSafety
  omega

/-- Equivalent disjunctive form of the reviewer fact: with insufficient
overlap, at least one candidate cohort must be below threshold. -/
theorem overlap_le_fault_forces_cohort_below_threshold
    {n a b o t f : Nat}
    (hOverlap : o <= f)
    (hCardinality : a + b <= n + o)
    (hSafety : n + f < 2 * t) :
    a < t ∨ b < t := by
  have hNotBoth :
      ¬ (t <= a ∧ t <= b) :=
    not_both_threshold_cohorts_of_overlap_le_fault
      hOverlap hCardinality hSafety
  omega

/-- Direct Tier-2 form: two cohorts at the participant threshold in the same
original-view universe must overlap by more than the tolerated Byzantine bound.
-/
theorem participant_threshold_cohorts_overlap_gt_byzantine
    {count a b overlap : Nat}
    (hCardinality : a + b <= count + overlap)
    (hA : participantThreshold count <= a)
    (hB : participantThreshold count <= b) :
    byzantineBound count < overlap := by
  exact overlap_gt_fault_of_two_threshold_cohorts
    hCardinality
    hA
    hB
    (participantThreshold_intersection_safe count)

/-- nUNL form: when the effective universe shrinks, the original-view
participant threshold still forces overlap above the original Byzantine bound.
-/
theorem participant_threshold_cohorts_overlap_gt_byzantine_under_shrink
    {originalView effectiveView a b overlap : Nat}
    (hShrink : effectiveView <= originalView)
    (hCardinality : a + b <= effectiveView + overlap)
    (hA : participantThreshold originalView <= a)
    (hB : participantThreshold originalView <= b) :
    byzantineBound originalView < overlap := by
  exact overlap_gt_fault_of_two_threshold_cohorts
    hCardinality
    hA
    hB
    (participantThreshold_safe_under_effective_shrink
      originalView
      effectiveView
      hShrink)

end XahauConsensus
