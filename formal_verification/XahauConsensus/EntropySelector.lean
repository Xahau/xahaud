import XahauConsensus.Threshold

namespace XahauConsensus

inductive EntropyTier where
  | consensusFallback
  | participantAligned
  | validatorQuorum
  deriving DecidableEq, Repr

/-- Minimal model of `ConsensusExtensions::selectEntropy`'s network,
non-failed, non-empty tier ladder.

The real C++ also computes a digest. This model deliberately focuses on the
part that can fork by labeling the same agreed set differently: the tier
decision from `(fromUNLReport, participantCount, effectiveView, originalView)`.
It does not model the standalone development shortcut, timeout-driven
`entropyFailed_` downgrade, or empty-map fallback; those paths all bypass or
downgrade this ladder rather than producing a stronger non-fallback label.
-/
def selectEntropyTier
    (fromUNLReport : Bool)
    (participantCount effectiveView originalView : Nat) : EntropyTier :=
  if !fromUNLReport then
    EntropyTier.consensusFallback
  else if participantCount >= safeQuorumThreshold effectiveView then
    EntropyTier.validatorQuorum
  else if participantCount >= safeParticipantThreshold originalView then
    EntropyTier.participantAligned
  else
    EntropyTier.consensusFallback

/-- Non-standalone nodes must fail closed to fallback until the validator view
is ledger-anchored by a UNLReport. -/
theorem no_unl_report_selects_fallback
    (participantCount effectiveView originalView : Nat) :
    selectEntropyTier false participantCount effectiveView originalView =
      EntropyTier.consensusFallback := by
  rfl

/-- At or above the effective-view quorum threshold, the ladder selects the
strongest entropy tier. -/
theorem quorum_count_selects_validator_quorum
    {participantCount effectiveView originalView : Nat}
    (hQuorum : safeQuorumThreshold effectiveView <= participantCount) :
    selectEntropyTier true participantCount effectiveView originalView =
      EntropyTier.validatorQuorum := by
  unfold selectEntropyTier
  simp [hQuorum]

/-- Below validator quorum but at or above the original-view participant floor,
the ladder selects Tier 2. -/
theorem participant_band_selects_tier2
    {participantCount effectiveView originalView : Nat}
    (hBelowQuorum : participantCount < safeQuorumThreshold effectiveView)
    (hParticipant : safeParticipantThreshold originalView <= participantCount) :
    selectEntropyTier true participantCount effectiveView originalView =
      EntropyTier.participantAligned := by
  unfold selectEntropyTier
  simp [Nat.not_le_of_gt hBelowQuorum, hParticipant]

/-- Below both thresholds, the ladder falls back. -/
theorem below_participant_floor_selects_fallback
    {participantCount effectiveView originalView : Nat}
    (hBelowQuorum : participantCount < safeQuorumThreshold effectiveView)
    (hBelowParticipant : participantCount < safeParticipantThreshold originalView) :
    selectEntropyTier true participantCount effectiveView originalView =
      EntropyTier.consensusFallback := by
  unfold selectEntropyTier
  simp [
    Nat.not_le_of_gt hBelowQuorum,
    Nat.not_le_of_gt hBelowParticipant]

end XahauConsensus
