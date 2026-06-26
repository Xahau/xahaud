import XahauConsensus.SidecarAlignment

namespace XahauConsensus

/-- Minimal model of the RNG entropy sidecar accept gate.

The C++ gate accepts validator-derived entropy only when the local view has a
quorum-aligned sidecar hash and full local observation of tx-converged peers.
This model deliberately excludes the sidecar-map contents; those are covered by
the accepted-hash discipline in `selectEntropy`.
-/
def entropyObservationGate
    (threshold aligned : Nat)
    (localIsMember localPublished : Bool)
    (peersSeen txConverged : Nat) : Bool :=
  quorumAligned threshold aligned localIsMember localPublished &&
    fullObservation peersSeen txConverged

/-- A quorum-only variant for comparison. This is not a claim that the C++ must
use this policy; it is a small model used to isolate what the full-observation
term contributes to the decision. -/
def entropyQuorumOnlyGate
    (threshold aligned : Nat)
    (localIsMember localPublished : Bool) : Bool :=
  quorumAligned threshold aligned localIsMember localPublished

theorem entropyObservationGate_iff_quorum_and_full
    (threshold aligned : Nat)
    (localIsMember localPublished : Bool)
    (peersSeen txConverged : Nat) :
    entropyObservationGate
        threshold
        aligned
        localIsMember
        localPublished
        peersSeen
        txConverged = true ↔
      quorumAligned threshold aligned localIsMember localPublished = true ∧
        fullObservation peersSeen txConverged = true := by
  unfold entropyObservationGate
  simp

/-- `fullObservation` is local equality of two observed counts. If the same node
has already seen `seen` advertised sidecar hashes, then merely learning about
one more tx-converged peer that has not advertised flips full observation from
true to false. -/
theorem fullObservation_flips_when_unadvertised_peer_is_seen (seen : Nat) :
    fullObservation seen seen = true ∧
      fullObservation seen (seen + 1) = false := by
  constructor
  · unfold fullObservation
    simp
  · unfold fullObservation
    simp

/-- Concrete counterexample for the RNG gate.

Both local views have the same quorum-aligned sidecar hash: three aligned remote
participants plus the local validator meet threshold four. The only difference
is whether the observer has seen a fourth tx-converged peer that did not
advertise an entropy sidecar hash.

The first view has `peersSeen = txConverged = 3` and accepts. The second has
`peersSeen = 3`, `txConverged = 4` and does not. This captures the subtle point:
`fullObservation` is not global completeness and does not by itself synchronize
accept-vs-fallback decisions across nodes.
-/
theorem same_quorum_alignment_fullObservation_can_change_accept :
    entropyObservationGate 4 3 true true 3 3 = true ∧
      entropyObservationGate 4 3 true true 3 4 = false := by
  native_decide

/-- In the same counterexample, a quorum-only gate would make the same decision
for both observers. This theorem is descriptive only: it isolates the source of
the branch difference; it does not model the liveness/timing costs of changing
the production gate. -/
theorem same_quorum_alignment_quorumOnly_same_decision :
    entropyQuorumOnlyGate 4 3 true true = true ∧
      entropyQuorumOnlyGate 4 3 true true = true := by
  native_decide

/-- Quorum alignment and full observation are independent predicates: a view can
have quorum alignment while still lacking full observation. -/
theorem quorumAligned_does_not_imply_fullObservation :
    quorumAligned 4 3 true true = true ∧
      fullObservation 3 4 = false := by
  native_decide

/-- Full observation also does not imply quorum alignment. A node can have seen
all tx-converged peers in its local view while too few participants align with
its sidecar hash. -/
theorem fullObservation_does_not_imply_quorumAligned :
    fullObservation 3 3 = true ∧
      quorumAligned 4 2 true true = false := by
  native_decide

end XahauConsensus
