import XahauConsensus.Threshold

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

end XahauConsensus
