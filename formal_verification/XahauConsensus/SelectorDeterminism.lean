import XahauConsensus.EntropySelector

namespace XahauConsensus

/-- A minimal digest model: the payload is opaque to the selector, while the
label is the entropy tier chosen from the consensus metadata. -/
structure LabeledDigest (α : Type) where
  payload : α
  label : EntropyTier
  deriving Repr

def labelDigest
    (fromUNLReport : Bool)
    (participantCount effectiveView originalView : Nat)
    (payload : α) : LabeledDigest α :=
  { payload
    label :=
      selectEntropyTier
        fromUNLReport
        participantCount
        effectiveView
        originalView }

/-- The digest payload itself does not affect the selected tier. The label is
entirely determined by the consensus metadata. -/
theorem payload_does_not_affect_tier
    {α : Type}
    {payloadA payloadB : α}
    (fromUNLReport : Bool)
    (participantCount effectiveView originalView : Nat) :
    (labelDigest
        fromUNLReport
        participantCount
        effectiveView
        originalView
        payloadA).label =
      (labelDigest
        fromUNLReport
        participantCount
        effectiveView
        originalView
        payloadB).label := by
  rfl

/-- Without a UNLReport anchor the same count and views can receive a different
label. -/
theorem label_can_differ_when_fromUNLReport_differs :
    (labelDigest true 8 10 10 0).label ≠
      (labelDigest false 8 10 10 0).label := by
  native_decide

/-- Changing the effective validator view can change the digest label. -/
theorem label_can_differ_when_effective_view_differs :
    (labelDigest true 7 8 10 0).label ≠
      (labelDigest true 7 10 10 0).label := by
  native_decide

/-- Changing the original validator view can change the digest label. -/
theorem label_can_differ_when_original_view_differs :
    (labelDigest true 6 10 8 0).label ≠
      (labelDigest true 6 10 10 0).label := by
  native_decide

end XahauConsensus
