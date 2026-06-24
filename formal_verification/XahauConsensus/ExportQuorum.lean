import XahauConsensus.Intersection
import XahauConsensus.NunlCap
import XahauConsensus.ThresholdFacts

namespace XahauConsensus

/-!
Nat-cardinality arithmetic for export sidecar quorum uniqueness.

The model deliberately stays at the level used by `Intersection.lean`:

* `n` is the active validator universe size.
* `a` and `b` are the numbers of validators supporting two export sidecar
  hashes in that same universe.
* `overlap` is the size of the intersection between those two support sets.
* `faultyOverlap + honestOverlap = overlap` splits that intersection.

No `Finset` structure is needed here; callers supply the usual
inclusion-exclusion cardinality inequality `a + b <= n + overlap`.
-/

theorem disabled_le_cap_mul_four_le
    {originalView disabled : Nat}
    (hCap : disabled <= disabledCap originalView) :
    disabled * 4 <= originalView + 3 := by
  unfold disabledCap ceilDiv at hCap
  have hFour : 0 < 4 := by decide
  simp at hCap
  have hMul :=
    (Nat.le_div_iff_mul_le hFour).mp hCap
  omega

theorem quorumThreshold_mul_five_ge_four_mul (n : Nat) :
    4 * n <= 5 * quorumThreshold n := by
  unfold quorumThreshold
  have hHundred : 0 < 100 := by decide
  have hDiv :
      (n * 80 + 99) / 100 <= (n * 80 + 99) / 100 :=
    Nat.le_refl _
  have hBound :=
    (Nat.div_le_iff_le_mul hHundred).mp hDiv
  omega

theorem byzantineBound_mul_five_le (n : Nat) :
    byzantineBound n * 5 <= n := by
  unfold byzantineBound
  exact Nat.div_mul_le_self n 5

/-- Two 80% export quorums in one active universe overlap by at least
`2 * quorumThreshold n - n`. -/
theorem two_export_quorums_overlap_lower_bound
    {n a b overlap : Nat}
    (hCardinality : a + b <= n + overlap)
    (hA : quorumThreshold n <= a)
    (hB : quorumThreshold n <= b) :
    2 * quorumThreshold n - n <= overlap := by
  omega

/-- The 80% quorum threshold is intersection-safe against the standard
`floor(n / 5)` fault bound for every nonempty active universe. -/
theorem quorumThreshold_intersection_safe
    {n : Nat} (hPositive : 0 < n) :
    n + byzantineBound n < 2 * quorumThreshold n := by
  unfold quorumThreshold byzantineBound
  omega

/-- The unconditional version is false: the empty active universe has raw
quorum threshold zero, so there is no strict intersection margin. -/
theorem quorumThreshold_empty_not_intersection_safe :
    ¬ 0 + byzantineBound 0 < 2 * quorumThreshold 0 := by
  native_decide

/-- Two export sidecar hashes both clearing 80% quorum in the same nonempty
active universe must have overlap larger than the standard fault bound. -/
theorem export_hash_quorums_overlap_gt_byzantine
    {n a b overlap : Nat}
    (hPositive : 0 < n)
    (hCardinality : a + b <= n + overlap)
    (hA : quorumThreshold n <= a)
    (hB : quorumThreshold n <= b) :
    byzantineBound n < overlap := by
  exact overlap_gt_fault_of_two_threshold_cohorts
    hCardinality
    hA
    hB
    (quorumThreshold_intersection_safe hPositive)

/-- If the overlap between two quorum-clearing export hashes is split into
faulty and honest validators, and at most `floor(n / 5)` validators in that
overlap are faulty, then the overlap contains an honest validator. -/
theorem export_hash_quorums_force_honest_overlap
    {n a b overlap faultyOverlap honestOverlap : Nat}
    (hPositive : 0 < n)
    (hCardinality : a + b <= n + overlap)
    (hA : quorumThreshold n <= a)
    (hB : quorumThreshold n <= b)
    (hSplit : overlap = faultyOverlap + honestOverlap)
    (hFaulty : faultyOverlap <= byzantineBound n) :
    0 < honestOverlap := by
  have hOverlap :
      byzantineBound n < overlap :=
    export_hash_quorums_overlap_gt_byzantine
      hPositive
      hCardinality
      hA
      hB
  omega

/-- Export quorum intersection remains above the original-view Byzantine bound
when nUNL shrinkage is within the protocol's ceil-25% cap. -/
theorem export_quorum_intersection_safe_under_nunl_cap
    {originalView effectiveView disabled : Nat}
    (hEffective : effectiveView = originalView - disabled)
    (hCap : disabled <= disabledCap originalView)
    (hPositive : 0 < effectiveView) :
    effectiveView + byzantineBound originalView <
      2 * quorumThreshold effectiveView := by
  have hCapBound :
      disabled * 4 <= originalView + 3 :=
    disabled_le_cap_mul_four_le hCap
  have hQuorumBound :
      4 * effectiveView <= 5 * quorumThreshold effectiveView :=
    quorumThreshold_mul_five_ge_four_mul effectiveView
  have hByzBound :
      byzantineBound originalView * 5 <= originalView :=
    byzantineBound_mul_five_le originalView
  omega

/-- Two export sidecar hashes both clearing 80% quorum in an nUNL-shrunk
effective view must still overlap above the original-view Byzantine bound,
provided the shrinkage stays within the protocol cap. -/
theorem export_hash_quorums_overlap_gt_original_byzantine_under_nunl_cap
    {originalView effectiveView disabled a b overlap : Nat}
    (hEffective : effectiveView = originalView - disabled)
    (hCap : disabled <= disabledCap originalView)
    (hPositive : 0 < effectiveView)
    (hCardinality : a + b <= effectiveView + overlap)
    (hA : quorumThreshold effectiveView <= a)
    (hB : quorumThreshold effectiveView <= b) :
    byzantineBound originalView < overlap := by
  exact overlap_gt_fault_of_two_threshold_cohorts
    hCardinality
    hA
    hB
    (export_quorum_intersection_safe_under_nunl_cap
      hEffective
      hCap
      hPositive)

/-- A Byzantine minority at the standard bound cannot veto export quorum:
after removing `floor(n / 5)` validators, enough validators remain to meet the
80% quorum threshold. -/
theorem byzantineBound_cannot_veto_quorum (n : Nat) :
    byzantineBound n + quorumThreshold n <= n := by
  unfold byzantineBound quorumThreshold
  omega

/-- Equivalent no-veto form using subtraction. -/
theorem quorumThreshold_le_universe_minus_byzantineBound (n : Nat) :
    quorumThreshold n <= n - byzantineBound n := by
  have hNoVeto := byzantineBound_cannot_veto_quorum n
  omega

/-- Concrete regression anchor: in a 5-validator active universe, two 80%
export quorums overlap in at least three validators. -/
theorem export_quorum_five_overlap_at_least_three
    {a b overlap : Nat}
    (hCardinality : a + b <= 5 + overlap)
    (hA : quorumThreshold 5 <= a)
    (hB : quorumThreshold 5 <= b) :
    3 <= overlap := by
  have hLower :
      2 * quorumThreshold 5 - 5 <= overlap :=
    two_export_quorums_overlap_lower_bound
      hCardinality
      hA
      hB
  have hExact : 2 * quorumThreshold 5 - 5 = 3 := by
    native_decide
  omega

/-- Concrete regression anchor: in a 10-validator active universe, two 80%
export quorums overlap in at least six validators. -/
theorem export_quorum_ten_overlap_at_least_six
    {a b overlap : Nat}
    (hCardinality : a + b <= 10 + overlap)
    (hA : quorumThreshold 10 <= a)
    (hB : quorumThreshold 10 <= b) :
    6 <= overlap := by
  have hLower :
      2 * quorumThreshold 10 - 10 <= overlap :=
    two_export_quorums_overlap_lower_bound
      hCardinality
      hA
      hB
  have hExact : 2 * quorumThreshold 10 - 10 = 6 := by
    native_decide
  omega

/-- Concrete regression anchor: in a 20-validator active universe, two 80%
export quorums overlap in at least twelve validators. -/
theorem export_quorum_twenty_overlap_at_least_twelve
    {a b overlap : Nat}
    (hCardinality : a + b <= 20 + overlap)
    (hA : quorumThreshold 20 <= a)
    (hB : quorumThreshold 20 <= b) :
    12 <= overlap := by
  have hLower :
      2 * quorumThreshold 20 - 20 <= overlap :=
    two_export_quorums_overlap_lower_bound
      hCardinality
      hA
      hB
  have hExact : 2 * quorumThreshold 20 - 20 = 12 := by
    native_decide
  omega

/-- On exact multiples of five, two 80% export quorums overlap in at least
`3 * k` validators. -/
theorem export_quorum_five_mul_overlap_at_least_three_mul
    {k a b overlap : Nat}
    (hCardinality : a + b <= 5 * k + overlap)
    (hA : quorumThreshold (5 * k) <= a)
    (hB : quorumThreshold (5 * k) <= b) :
    3 * k <= overlap := by
  have hLower :
      2 * quorumThreshold (5 * k) - 5 * k <= overlap :=
    two_export_quorums_overlap_lower_bound
      hCardinality
      hA
      hB
  rw [quorumThreshold_five_mul] at hLower
  omega

/-- On exact multiples of five, quorum overlap strictly exceeds the standard
fault bound by at least `2 * k`. For `k = 0` this is only a non-strict
difference statement; strict safety is provided by
`export_hash_quorums_overlap_gt_byzantine` for nonempty universes. -/
theorem export_quorum_five_mul_overlap_margin
    {k a b overlap : Nat}
    (hCardinality : a + b <= 5 * k + overlap)
    (hA : quorumThreshold (5 * k) <= a)
    (hB : quorumThreshold (5 * k) <= b) :
    byzantineBound (5 * k) + 2 * k <= overlap := by
  have hOverlap :
      3 * k <= overlap :=
    export_quorum_five_mul_overlap_at_least_three_mul
      hCardinality
      hA
      hB
  rw [byzantineBound_five_mul]
  omega

end XahauConsensus
