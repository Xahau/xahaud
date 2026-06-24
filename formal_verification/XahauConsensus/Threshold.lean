namespace XahauConsensus

/-- C++: `count / 5`, the conservative Byzantine bound used by
`calculateParticipantThreshold`. -/
def byzantineBound (count : Nat) : Nat :=
  count / 5

/-- C++: `calculateParticipantThreshold(count)`.

This is the smallest integer `t` satisfying `2 * t > count + floor(count / 5)`.
-/
def participantThreshold (count : Nat) : Nat :=
  (count + byzantineBound count) / 2 + 1

/-- C++: `calculateQuorumThreshold(count)`, i.e. `ceil(0.8 * count)`. -/
def quorumThreshold (count : Nat) : Nat :=
  (count * 80 + 99) / 100

/-- C++: `ConsensusExtensions::quorumThreshold()`.

The raw formula gives `0` for an empty view, but the live consensus-extension
gate requires at least one aligned participant for safety.
-/
def safeQuorumThreshold (count : Nat) : Nat :=
  if count = 0 then 1 else quorumThreshold count

/-- C++: `ConsensusExtensions::tier2Threshold()`.

`participantThreshold 0` already returns `1`; this wrapper makes the
zero-view safety rule explicit and mirrors the C++ method shape.
-/
def safeParticipantThreshold (count : Nat) : Nat :=
  if count = 0 then 1 else participantThreshold count

/-- The Tier-2 threshold strictly exceeds the Byzantine-overlap boundary.

This is the load-bearing equivocation invariant behind participant-aligned
entropy: two cohorts of this size in a `count`-sized universe overlap in more
than `floor(count / 5)` validators.
-/
theorem participantThreshold_intersection_safe (count : Nat) :
    count + byzantineBound count < 2 * participantThreshold count := by
  unfold participantThreshold byzantineBound
  omega

/-- Anchoring the Tier-2 threshold to the original pre-nUNL view remains safe
when the effective post-nUNL view shrinks.

This is the arithmetic reason `originalViewSize` is the right denominator:
smaller effective universes only increase the intersection margin.
-/
theorem participantThreshold_safe_under_effective_shrink
    (originalView effectiveView : Nat)
    (hShrink : effectiveView <= originalView) :
    effectiveView + byzantineBound originalView <
      2 * participantThreshold originalView := by
  have hSafe := participantThreshold_intersection_safe originalView
  omega

/-- Concrete regression example: if `originalView = 10` and `effectiveView = 8`,
using the effective view's participant threshold (`5`) leaves the overlap equal
to the original-view Byzantine bound (`2`), not strictly greater than it.

This is why the C++ must not replace `originalViewSize` with `size()` for the
Tier-2 floor.
-/
theorem effective_threshold_regression_hits_boundary_example :
    2 * participantThreshold 8 <= 8 + byzantineBound 10 := by
  native_decide

theorem threshold_minimal_for_boundary (boundary threshold : Nat) :
    boundary < 2 * threshold → boundary / 2 + 1 <= threshold := by
  omega

theorem below_threshold_not_safe_for_boundary (boundary threshold : Nat) :
    threshold < boundary / 2 + 1 → 2 * threshold <= boundary := by
  omega

/-- `participantThreshold` is the smallest threshold satisfying the strict
intersection-safety inequality. -/
theorem participantThreshold_minimal (count threshold : Nat) :
    count + byzantineBound count < 2 * threshold →
      participantThreshold count <= threshold := by
  intro hSafe
  unfold participantThreshold
  exact threshold_minimal_for_boundary
    (count + byzantineBound count)
    threshold
    hSafe

/-- Anything below `participantThreshold` fails the strict intersection-safety
inequality. -/
theorem below_participantThreshold_not_safe (count threshold : Nat) :
    threshold < participantThreshold count →
      2 * threshold <= count + byzantineBound count := by
  intro hBelow
  unfold participantThreshold at hBelow
  exact below_threshold_not_safe_for_boundary
    (count + byzantineBound count)
    threshold
    hBelow

/-- The participant threshold never exceeds the 80% validator-quorum threshold.

This is useful because Tier 2 should form a band below Tier 3, not a stricter
condition than validator quorum.
-/
theorem participantThreshold_le_quorumThreshold (count : Nat) :
    0 < count → participantThreshold count <= quorumThreshold count := by
  intro hCount
  unfold participantThreshold quorumThreshold byzantineBound
  omega

/-- With the live safety wrappers, the participant threshold never exceeds the
validator-quorum threshold, including the empty-view edge case. -/
theorem safeParticipantThreshold_le_safeQuorumThreshold (count : Nat) :
    safeParticipantThreshold count <= safeQuorumThreshold count := by
  unfold safeParticipantThreshold safeQuorumThreshold
  by_cases hZero : count = 0
  · simp [hZero]
  · have hPositive : 0 < count := Nat.pos_of_ne_zero hZero
    simp [hZero, participantThreshold_le_quorumThreshold count hPositive]

end XahauConsensus
