import Mathlib.Data.Finset.Card
import XahauConsensus.ExportQuorum
import XahauConsensus.Intersection

namespace XahauConsensus

/-!
Finite-set bridge for the quorum-intersection arithmetic.

The arithmetic modules prove useful facts from the premise
`a + b <= n + overlap`. This module discharges that premise for actual finite
cohorts `A` and `B` that are both subsets of a common validator universe `U`.
-/

open Finset

/-- Inclusion-exclusion bridge: two finite cohorts inside one universe satisfy
the cardinality premise used by `Intersection.lean`. -/
theorem finset_cardinality_bound
    [DecidableEq α]
    {U A B : Finset α}
    (hA : A ⊆ U)
    (hB : B ⊆ U) :
    A.card + B.card <= U.card + (A ∩ B).card := by
  have hUnionSubset : A ∪ B ⊆ U := by
    intro x hx
    rcases Finset.mem_union.mp hx with hxA | hxB
    · exact hA hxA
    · exact hB hxB
  have hUnionCard : (A ∪ B).card <= U.card :=
    Finset.card_le_card hUnionSubset
  have hInclusion :
      (A ∪ B).card + (A ∩ B).card = A.card + B.card :=
    Finset.card_union_add_card_inter A B
  omega

/-- Set-level Tier-2 form: two participant-threshold cohorts in the same
validator universe overlap above the Byzantine bound. -/
theorem finset_participant_threshold_cohorts_overlap_gt_byzantine
    [DecidableEq α]
    {U A B : Finset α}
    (hAUniverse : A ⊆ U)
    (hBUniverse : B ⊆ U)
    (hAThreshold : participantThreshold U.card <= A.card)
    (hBThreshold : participantThreshold U.card <= B.card) :
    byzantineBound U.card < (A ∩ B).card := by
  exact participant_threshold_cohorts_overlap_gt_byzantine
    (finset_cardinality_bound hAUniverse hBUniverse)
    hAThreshold
    hBThreshold

/-- nUNL/set-level form: two original-view participant-threshold cohorts in a
shrunk effective universe still overlap above the original Byzantine bound. -/
theorem finset_participant_threshold_cohorts_overlap_gt_byzantine_under_shrink
    [DecidableEq α]
    {Original Effective A B : Finset α}
    (hEffectiveSubset : Effective ⊆ Original)
    (hAUniverse : A ⊆ Effective)
    (hBUniverse : B ⊆ Effective)
    (hAThreshold : participantThreshold Original.card <= A.card)
    (hBThreshold : participantThreshold Original.card <= B.card) :
    byzantineBound Original.card < (A ∩ B).card := by
  have hShrink : Effective.card <= Original.card :=
    Finset.card_le_card hEffectiveSubset
  exact participant_threshold_cohorts_overlap_gt_byzantine_under_shrink
    hShrink
    (finset_cardinality_bound hAUniverse hBUniverse)
    hAThreshold
    hBThreshold

/-- Set-level export form: two 80% export sidecar quorums in the same nonempty
active universe overlap above the standard Byzantine bound. -/
theorem finset_export_hash_quorums_overlap_gt_byzantine
    [DecidableEq α]
    {U A B : Finset α}
    (hNonempty : 0 < U.card)
    (hAUniverse : A ⊆ U)
    (hBUniverse : B ⊆ U)
    (hAThreshold : quorumThreshold U.card <= A.card)
    (hBThreshold : quorumThreshold U.card <= B.card) :
    byzantineBound U.card < (A ∩ B).card := by
  exact export_hash_quorums_overlap_gt_byzantine
    hNonempty
    (finset_cardinality_bound hAUniverse hBUniverse)
    hAThreshold
    hBThreshold

end XahauConsensus
