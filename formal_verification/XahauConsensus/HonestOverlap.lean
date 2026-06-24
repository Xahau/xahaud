import XahauConsensus.Intersection

namespace XahauConsensus

/-!
Bridge from cardinality arithmetic to the consensus-language statement:
if cohort overlap is larger than the maximum faulty overlap, then the overlap
contains at least one honest validator.
-/

/-- If the overlap is larger than the number of faulty validators in it, then
some honest validator remains in the overlap. -/
theorem honest_overlap_exists
    {overlap faultyInOverlap : Nat}
    (hFaultyLtOverlap : faultyInOverlap < overlap) :
    0 < overlap - faultyInOverlap := by
  omega

/-- If total faulty validators are bounded by `faultBound`, and the overlap is
larger than `faultBound`, then the overlap contains an honest validator. -/
theorem honest_overlap_exists_of_fault_bound
    {overlap faultyInOverlap faultBound : Nat}
    (hFaultyBound : faultyInOverlap <= faultBound)
    (hOverlapGtFaultBound : faultBound < overlap) :
    0 < overlap - faultyInOverlap := by
  omega

/-- Direct bridge from the abstract two-cohort intersection theorem: two
threshold-sized cohorts under the strict safety inequality have honest overlap,
provided faulty validators in the overlap are bounded by `f`.
-/
theorem honest_overlap_of_two_threshold_cohorts
    {n a b overlap threshold faultBound faultyInOverlap : Nat}
    (hCardinality : a + b <= n + overlap)
    (hA : threshold <= a)
    (hB : threshold <= b)
    (hSafety : n + faultBound < 2 * threshold)
    (hFaultyBound : faultyInOverlap <= faultBound) :
    0 < overlap - faultyInOverlap := by
  have hOverlapGtFaultBound :
      faultBound < overlap :=
    overlap_gt_fault_of_two_threshold_cohorts
      hCardinality
      hA
      hB
      hSafety
  exact honest_overlap_exists_of_fault_bound
    hFaultyBound
    hOverlapGtFaultBound

/-- Direct participant-threshold form: two Tier-2-sized cohorts in the same
view have honest overlap under the `floor(n/5)` Byzantine bound. -/
theorem honest_overlap_of_participant_threshold_cohorts
    {count a b overlap faultyInOverlap : Nat}
    (hCardinality : a + b <= count + overlap)
    (hA : participantThreshold count <= a)
    (hB : participantThreshold count <= b)
    (hFaultyBound : faultyInOverlap <= byzantineBound count) :
    0 < overlap - faultyInOverlap := by
  have hOverlapGtBound :
      byzantineBound count < overlap :=
    participant_threshold_cohorts_overlap_gt_byzantine
      hCardinality
      hA
      hB
  exact honest_overlap_exists_of_fault_bound
    hFaultyBound
    hOverlapGtBound

end XahauConsensus
