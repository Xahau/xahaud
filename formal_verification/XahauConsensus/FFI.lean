import XahauConsensus.Threshold
import XahauConsensus.Invariants
import XahauConsensus.NunlCap
import XahauConsensus.SidecarAlignment
import XahauConsensus.ViewUniverse
import XahauConsensus.ExportQuorum
import XahauConsensus.SixtyPercent

namespace XahauConsensus

/-! Scalar C ABI exports used by the optional C++ drift tests.

These functions intentionally expose only plain integer formulas. The broader
Lean project proves properties about these definitions; the C++ tests then
check that selected production formulas and helper predicates still compute the
same values.
-/

-- @@start ffi-scalar-export-surface
@[export xahau_byzantine_bound]
def xahauByzantineBound (count : UInt64) : UInt64 :=
  (byzantineBound count.toNat).toUInt64

@[export xahau_participant_threshold]
def xahauParticipantThreshold (count : UInt64) : UInt64 :=
  (participantThreshold count.toNat).toUInt64

@[export xahau_quorum_threshold]
def xahauQuorumThreshold (count : UInt64) : UInt64 :=
  (quorumThreshold count.toNat).toUInt64

@[export xahau_safe_quorum_threshold]
def xahauSafeQuorumThreshold (count : UInt64) : UInt64 :=
  (safeQuorumThreshold count.toNat).toUInt64

@[export xahau_safe_participant_threshold]
def xahauSafeParticipantThreshold (count : UInt64) : UInt64 :=
  (safeParticipantThreshold count.toNat).toUInt64

@[export xahau_entropy_gate_threshold_for_view]
def xahauEntropyGateThresholdForView
    (effectiveView originalView : UInt64) : UInt64 :=
  (entropyGateThresholdForView effectiveView.toNat originalView.toNat).toUInt64

def entropyTierCode : EntropyTier → UInt8
  | EntropyTier.consensusFallback => 1
  | EntropyTier.participantAligned => 2
  | EntropyTier.validatorQuorum => 3

@[export xahau_select_entropy_tier]
def xahauSelectEntropyTier
    (fromUNLReport participantCount effectiveView originalView : UInt64) : UInt8 :=
  entropyTierCode <|
    selectEntropyTier
      (fromUNLReport != 0)
      participantCount.toNat
      effectiveView.toNat
      originalView.toNat

@[export xahau_aligned_participants]
def xahauAlignedParticipants
    (aligned localIsMember localPublished : UInt64) : UInt64 :=
  (alignedParticipants
      aligned.toNat
      (localIsMember != 0)
      (localPublished != 0)).toUInt64

@[export xahau_quorum_aligned]
def xahauQuorumAligned
    (threshold aligned localIsMember localPublished : UInt64) : UInt8 :=
  if quorumAligned
      threshold.toNat
      aligned.toNat
      (localIsMember != 0)
      (localPublished != 0) then
    1
  else
    0

@[export xahau_full_observation]
def xahauFullObservation (peersSeen txConverged : UInt64) : UInt8 :=
  if fullObservation peersSeen.toNat txConverged.toNat then 1 else 0

@[export xahau_export_gate_proceed]
def xahauExportGateProceed
    (alignedParticipants quorumThreshold fullObservation : UInt64) : UInt8 :=
  if (ExportGate.mk
        alignedParticipants.toNat
        quorumThreshold.toNat
        (fullObservation != 0)).proceed then
    1
  else
    0


@[export xahau_strict_intersection_safe]
def xahauStrictIntersectionSafe
    (activeView byzantineUniverse threshold : UInt64) : UInt8 :=
  if activeView.toNat + byzantineBound byzantineUniverse.toNat <
      2 * threshold.toNat then
    1
  else
    0

@[export xahau_nonvacuous_strict_intersection_safe]
def xahauNonvacuousStrictIntersectionSafe
    (activeView byzantineUniverse threshold : UInt64) : UInt8 :=
  if threshold.toNat <= activeView.toNat ∧
      activeView.toNat + byzantineBound byzantineUniverse.toNat <
        2 * threshold.toNat then
    1
  else
    0

@[export xahau_participant_band_nonempty]
def xahauParticipantBandNonempty
    (effectiveView originalView : UInt64) : UInt8 :=
  if participantThreshold originalView.toNat < quorumThreshold effectiveView.toNat then
    1
  else
    0

@[export xahau_export_quorum_overlap_lower_bound]
def xahauExportQuorumOverlapLowerBound (activeView : UInt64) : UInt64 :=
  (2 * quorumThreshold activeView.toNat - activeView.toNat).toUInt64

@[export xahau_export_quorum_safe_under_nunl_cap]
def xahauExportQuorumSafeUnderNunlCap
    (originalView effectiveView disabled : UInt64) : UInt8 :=
  if effectiveView.toNat = originalView.toNat - disabled.toNat ∧
      disabled.toNat <= disabledCap originalView.toNat ∧
      0 < effectiveView.toNat ∧
      effectiveView.toNat + byzantineBound originalView.toNat <
        2 * quorumThreshold effectiveView.toNat then
    1
  else
    0

private def maskBit (mask : UInt64) (peer : Nat) : Bool :=
  ((mask.toNat / (2 ^ peer)) % 2) == 1

@[export xahau_active_aligned_count_mask]
def xahauActiveAlignedCountMask
    (count activeMask alignedMask : UInt64) : UInt64 :=
  (activeAlignedCount
      (maskBit activeMask)
      (maskBit alignedMask)
      count.toNat).toUInt64

@[export xahau_quorum_aligned_mask]
def xahauQuorumAlignedMask
    (threshold count activeMask alignedMask localIsMember localPublished : UInt64) : UInt8 :=
  let aligned :=
    activeAlignedCount
      (maskBit activeMask)
      (maskBit alignedMask)
      count.toNat
  if quorumAligned
      threshold.toNat
      aligned
      (localIsMember != 0)
      (localPublished != 0) then
    1
  else
    0

@[export xahau_naive_sixty_percent_threshold]
def xahauNaiveSixtyPercentThreshold (count : UInt64) : UInt64 :=
  (naiveSixtyPercentThreshold count.toNat).toUInt64

@[export xahau_naive_sixty_percent_is_safe]
def xahauNaiveSixtyPercentIsSafe (count : UInt64) : UInt8 :=
  if count.toNat + byzantineBound count.toNat <
      2 * naiveSixtyPercentThreshold count.toNat then
    1
  else
    0

@[export xahau_disabled_cap]
def xahauDisabledCap (originalView : UInt64) : UInt64 :=
  (disabledCap originalView.toNat).toUInt64

@[export xahau_effective_view]
def xahauEffectiveView (originalView disabled : UInt64) : UInt64 :=
  (effectiveView originalView.toNat disabled.toNat).toUInt64
-- @@end ffi-scalar-export-surface

end XahauConsensus
