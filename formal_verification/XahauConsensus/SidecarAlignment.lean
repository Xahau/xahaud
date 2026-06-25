namespace XahauConsensus

/-- Count a local boolean contribution as the `Nat` value used in threshold
comparisons. -/
def localPublishedCount (localPublished : Bool) : Nat :=
  if localPublished then 1 else 0

/-- The proof-level participant count behind sidecar alignment.

`aligned` is the count of aligned remote active-view participants; a local
publication contributes one more participant. -/
def alignedParticipants
    (aligned : Nat)
    (localIsMember localPublished : Bool) : Nat :=
  aligned + localPublishedCount (localIsMember && localPublished)

/-- Sidecar quorum predicate, kept boolean to mirror the implementation check. -/
def quorumAligned
    (threshold aligned : Nat)
    (localIsMember localPublished : Bool) : Bool :=
  decide (threshold <= alignedParticipants aligned localIsMember localPublished)

/-- Full sidecar observation means every converged transaction has been seen. -/
def fullObservation (peersSeen txConverged : Nat) : Bool :=
  peersSeen == txConverged

/-- Count aligned peers from a finite peer prefix, filtering through the active
view before any alignment bit contributes. -/
def activeAlignedCount
    (inActiveView peerAligned : Nat → Bool) : Nat → Nat
  | 0 => 0
  | peer + 1 =>
      activeAlignedCount inActiveView peerAligned peer +
        localPublishedCount (inActiveView peer && peerAligned peer)

theorem localPublishedCount_true :
    localPublishedCount true = 1 := by
  rfl

theorem localPublishedCount_false :
    localPublishedCount false = 0 := by
  rfl

theorem localPublishedCount_le_one (published : Bool) :
    localPublishedCount published <= 1 := by
  cases published <;> simp [localPublishedCount]

/-- Core participant-count equation: aligned remotes plus the local published
contribution. -/
theorem alignedParticipants_eq_aligned_plus_localPublished
    (aligned : Nat) (localIsMember localPublished : Bool) :
    alignedParticipants aligned localIsMember localPublished =
      aligned + localPublishedCount (localIsMember && localPublished) := by
  rfl

/-- A non-active local node cannot pad the participant count. -/
theorem alignedParticipants_local_nonmember
    (aligned : Nat) (localPublished : Bool) :
    alignedParticipants aligned false localPublished = aligned := by
  cases localPublished <;> rfl

/-- An active local node contributes exactly when it published the sidecar hash. -/
theorem alignedParticipants_local_member
    (aligned : Nat) (localPublished : Bool) :
    alignedParticipants aligned true localPublished =
      aligned + localPublishedCount localPublished := by
  cases localPublished <;> rfl

/-- The local node can add at most one participant to the remote aligned count. -/
theorem alignedParticipants_le_aligned_succ
    (aligned : Nat) (localIsMember localPublished : Bool) :
    alignedParticipants aligned localIsMember localPublished <= aligned + 1 := by
  cases localIsMember <;> cases localPublished <;>
    simp [alignedParticipants, localPublishedCount]

/-- The boolean quorum predicate is exactly the threshold comparison over
`alignedParticipants`. -/
theorem quorumAligned_iff_threshold_le_alignedParticipants
    (threshold aligned : Nat) (localIsMember localPublished : Bool) :
    quorumAligned threshold aligned localIsMember localPublished = true ↔
      threshold <= alignedParticipants aligned localIsMember localPublished := by
  unfold quorumAligned
  simp

/-- The boolean full-observation predicate is exactly equality of the observed
and converged counts. -/
theorem fullObservation_iff_peersSeen_eq_txConverged
    (peersSeen txConverged : Nat) :
    fullObservation peersSeen txConverged = true ↔
      peersSeen = txConverged := by
  unfold fullObservation
  simp

/-- A peer outside the active view contributes zero, even if its sidecar
alignment bit is set. -/
theorem activeAlignedCount_succ_nonmember
    {inActiveView peerAligned : Nat → Bool} {peer : Nat}
    (hNonmember : inActiveView peer = false) :
    activeAlignedCount inActiveView peerAligned (peer + 1) =
      activeAlignedCount inActiveView peerAligned peer := by
  simp [activeAlignedCount, hNonmember, localPublishedCount]

/-- A prefix of `n` peer positions can contribute at most `n` aligned active
remote participants. -/
theorem activeAlignedCount_le_prefix
    (inActiveView peerAligned : Nat → Bool) (n : Nat) :
    activeAlignedCount inActiveView peerAligned n <= n := by
  induction n with
  | zero =>
      simp [activeAlignedCount]
  | succ n ih =>
      cases hAligned : inActiveView n && peerAligned n
      · simp [activeAlignedCount, hAligned, localPublishedCount]
        exact Nat.le_trans ih (Nat.le_succ n)
      · simp [activeAlignedCount, hAligned, localPublishedCount]
        exact ih

/-- With the optional local contribution included, the participant count is
bounded by the inspected remote prefix plus one. -/
theorem alignedParticipants_le_prefix_succ
    (inActiveView peerAligned : Nat → Bool)
    (n : Nat)
    (localIsMember localPublished : Bool) :
    alignedParticipants
        (activeAlignedCount inActiveView peerAligned n)
        localIsMember
        localPublished <= n + 1 := by
  have hRemote := activeAlignedCount_le_prefix inActiveView peerAligned n
  cases localIsMember <;> cases localPublished <;>
    simp [alignedParticipants, localPublishedCount]
  · exact Nat.le_trans hRemote (Nat.le_succ n)
  · exact Nat.le_trans hRemote (Nat.le_succ n)
  · exact Nat.le_trans hRemote (Nat.le_succ n)
  · exact hRemote

/-- Adding a nonmember peer to the inspected prefix cannot increase
`alignedParticipants`. -/
theorem alignedParticipants_succ_nonmember
    {inActiveView peerAligned : Nat → Bool} {peer : Nat}
    (localIsMember localPublished : Bool)
    (hNonmember : inActiveView peer = false) :
    alignedParticipants
        (activeAlignedCount inActiveView peerAligned (peer + 1))
        localIsMember
        localPublished =
      alignedParticipants
        (activeAlignedCount inActiveView peerAligned peer)
        localIsMember
        localPublished := by
  simp [alignedParticipants, activeAlignedCount_succ_nonmember hNonmember]

/-- Consequently, a nonmember peer cannot change the quorum-aligned result. -/
theorem quorumAligned_succ_nonmember
    {inActiveView peerAligned : Nat → Bool} {peer threshold : Nat}
    (localIsMember localPublished : Bool)
    (hNonmember : inActiveView peer = false) :
    quorumAligned threshold
        (activeAlignedCount inActiveView peerAligned (peer + 1))
        localIsMember
        localPublished =
      quorumAligned threshold
        (activeAlignedCount inActiveView peerAligned peer)
        localIsMember
        localPublished := by
  simp [
    quorumAligned,
    alignedParticipants_succ_nonmember
      localIsMember
      localPublished
      hNonmember]

/-- Active-view filtering: only member peers' alignment bits can affect the
aligned remote count. -/
theorem activeAlignedCount_ext_on_members
    {n : Nat} {inActiveView alignedA alignedB : Nat → Bool}
    (hSameOnMembers :
      ∀ peer, peer < n → inActiveView peer = true →
        alignedA peer = alignedB peer) :
    activeAlignedCount inActiveView alignedA n =
      activeAlignedCount inActiveView alignedB n := by
  induction n with
  | zero =>
      rfl
  | succ n ih =>
      have hPrefix :
          ∀ peer, peer < n → inActiveView peer = true →
            alignedA peer = alignedB peer := by
        intro peer hLt hMember
        exact hSameOnMembers peer (Nat.lt_trans hLt (Nat.lt_succ_self n)) hMember
      have hAt :
          localPublishedCount (inActiveView n && alignedA n) =
            localPublishedCount (inActiveView n && alignedB n) := by
        cases hMember : inActiveView n
        · simp [localPublishedCount]
        · have hEq := hSameOnMembers n (Nat.lt_succ_self n) hMember
          simp [hEq, localPublishedCount]
      simp [activeAlignedCount, ih hPrefix, hAt]

/-- Changing sidecar alignment reports for nonmembers cannot change the final
participant count. -/
theorem alignedParticipants_ext_on_members
    {n : Nat} {inActiveView alignedA alignedB : Nat → Bool}
    {localIsMember : Bool}
    {localPublished : Bool}
    (hSameOnMembers :
      ∀ peer, peer < n → inActiveView peer = true →
        alignedA peer = alignedB peer) :
    alignedParticipants
        (activeAlignedCount inActiveView alignedA n)
        localIsMember
        localPublished =
      alignedParticipants
        (activeAlignedCount inActiveView alignedB n)
        localIsMember
        localPublished := by
  simp [
    alignedParticipants,
    activeAlignedCount_ext_on_members hSameOnMembers]

/-- Changing sidecar alignment reports for nonmembers cannot turn quorum on or
off. -/
theorem quorumAligned_ext_on_members
    {n threshold : Nat} {inActiveView alignedA alignedB : Nat → Bool}
    {localIsMember : Bool}
    {localPublished : Bool}
    (hSameOnMembers :
      ∀ peer, peer < n → inActiveView peer = true →
        alignedA peer = alignedB peer) :
    quorumAligned threshold
        (activeAlignedCount inActiveView alignedA n)
        localIsMember
        localPublished =
      quorumAligned threshold
        (activeAlignedCount inActiveView alignedB n)
        localIsMember
        localPublished := by
  simp [
    quorumAligned,
    alignedParticipants_ext_on_members hSameOnMembers]

end XahauConsensus
