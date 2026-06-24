import XahauConsensus.Threshold
import XahauConsensus.Invariants
import XahauConsensus.NunlCap
import XahauConsensus.SidecarAlignment

namespace XahauConsensus

/-! Scalar C ABI exports used by the optional C++ drift tests.

These functions intentionally expose only plain integer formulas. The broader
Lean project proves properties about these definitions; the C++ tests then
check that the production formulas still compute the same values.
-/

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

@[export xahau_disabled_cap]
def xahauDisabledCap (originalView : UInt64) : UInt64 :=
  (disabledCap originalView.toNat).toUInt64

@[export xahau_effective_view]
def xahauEffectiveView (originalView disabled : UInt64) : UInt64 :=
  (effectiveView originalView.toNat disabled.toNat).toUInt64

end XahauConsensus
