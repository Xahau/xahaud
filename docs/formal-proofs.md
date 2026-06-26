<!--
rendered_from: formal-proofs.md.j2
rendered_at: 2026-06-26T04:47:51Z
branch: feature-export-rng
commit: 938d3055c
commit_message: fix(consensus): harden threshold helpers
-->

---

<sub>Last updated: 2026-06-26 | branch: feature-export-rng | commit: 938d3055c (fix(consensus): harden threshold helpers)</sub>

---

# Formal Proofs For Consensus Extensions

> Status: supporting review artifact. Normal `feature-export-rng` builds do not
> require Lean. The optional C++/Lean drift-test harness lives only on the
> experimental `feature-export-rng-lean` branch and may be proposed later as a
> separate follow-up.


This document is a hand-maintained trace from selected consensus-extension C++
formulas/branches to Lean models of the same decision predicates. This rendered
snapshot is published on the experimental `feature-export-rng-lean` branch so
reviewers can inspect the formal-support artifact without relying on local
`.ai-docs` symlinks. The Lean proof workspace lives in `formal_verification`
on this branch. This is not a refinement proof, extraction, or machine-checked proof that the C++ implements
the Lean. It also does not model heterogeneous/dynamic UNL topology or prove
base XRP Ledger consensus safety/liveness. The point is narrower and more
useful: identify the formulas and decision ladders that carry
consensus-extension safety weight, show the C++ anchors that were manually
cross-checked against the model, and show the Lean theorem that pins the
reasoning.

The branch has two related but different mechanisms:

- **Entropy/RNG** has a protocol-defined deterministic fallback digest when
  entropy selection downgrades.
- **Export** has no deterministically derivable, quorum-valid replacement
  signature set; below quorum, network mode retries or expires.

The proofs focus on the places where a one-line threshold or view change could
turn into a fork: the participant-aligned floor, nUNL denominator choice,
sidecar alignment universe, tier labeling, and export quorum/no-veto behavior.

## Join Table

| Claim | C++ anchor | Lean anchor | Correspondence |
| --- | --- | --- | --- |
| Tier 2 threshold is derived from strict quorum intersection, not plain `ceil(0.6n)`. | `calculateParticipantThreshold` | `participantThreshold_intersection_safe`, `participantThreshold_minimal` | Modeled / manually cross-checked |
| Exact multiples of five need one more than naive 60%. | `calculateParticipantThreshold` | `naiveSixtyPercentThreshold_five_mul_not_intersection_safe` | Modeled / manually cross-checked |
| Tier 2 denominator is original pre-nUNL view; validator quorum denominator is effective post-nUNL view. | `buildActiveValidatorView`, `tier2Threshold`, `quorumThreshold` | `participantThreshold_safe_under_effective_shrink`, `disabledCap`, `max_cap_original10_below_participant_floor` | Safety modeled; reachability handled separately |
| Sidecar alignment counts only active-view members; trusted-but-non-active peers and non-active local nodes cannot pad the count. | `inspectTxConvergedSidecarPeers` | `alignedParticipants_local_nonmember`, `quorumAligned_ext_on_members` | Modeled / manually cross-checked |
| Non-UNLReport views fail closed to fallback for entropy tier labels. | `selectEntropy` UNLReport gate | `no_unl_report_selects_fallback` | Modeled for non-standalone network mode |
| The selector tier-label non-fallback boundary is the lower of validator quorum and participant floor. | `entropyGateThreshold`, `selectEntropy` ladder | `selectEntropyTier_nonfallback_iff_entropy_gate` | Models selector ladder, not timeout/full-observation state |
| Export sidecar gate proceeds on quorum alignment, not full observation. | export no-veto branch | `missing_minority_does_not_prevent_proceed`, `changing_fullObservation_alone_does_not_change_proceed` | Models sidecar gate only; `Export::doApply` still verifies agreed signatures |
| Export below quorum retries/expires; it does not synthesize a deterministic fallback signature set. | export timeout path / `Export::doApply` uses agreed signatures | `below_quorum_retries_or_expires` | Model-only outcome shape; C++ retry path projected below |
| Assuming nonempty effective view, `disabled <= ceil(original/4)`, and both support sets are counted inside that view, two 80% export quorums overlap above the original Byzantine bound. | `exportSigQuorumThreshold`, NegativeUNL cap | `export_hash_quorums_overlap_gt_original_byzantine_under_nunl_cap` | Modeled with explicit cap/universe premises |
| Optional Lean/C++ drift tests compare selected scalar Lean exports with production formulas and helper predicates when `formal_verification=ON`. | `LeanConsensus_test` | `XahauConsensus.FFI` exported defs | Gated model-to-code cross-check; still not an implementation proof |

## 1. The Participant Floor Is Not “Just 60%”

The Tier 2 floor is the smallest integer `t` such that two `t`-sized cohorts in
an `n`-validator universe must overlap in more than the tolerated Byzantine
count `floor(n / 5)`. That is stricter than naive `ceil(0.6n)` at exact
multiples of five.

📍 [`src/xrpld/consensus/ConsensusParms.h:285-292`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/consensus/ConsensusParms.h#L285-L292)
```cpp
 285 inline std::size_t
 286 calculateParticipantThreshold(std::size_t count)
 287 {
 288     // f = floor(0.2 * count) tolerated Byzantine validators; the smallest t
 289     // with 2t - count > f is floor((count + f) / 2) + 1.
 290     auto const byzantine = count / 5;
 291     return detail::floorHalfSum(count, byzantine) + 1;
 292 }
```

📍 `formal/lean/xahau_consensus/XahauConsensus/Threshold.lean:12-13`
```text
  12 def participantThreshold (count : Nat) : Nat :=
  13   (count + byzantineBound count) / 2 + 1
```

📍 `formal/lean/xahau_consensus/XahauConsensus/Threshold.lean:41-46`
```text
  41 theorem participantThreshold_intersection_safe (count : Nat) :
  42     count + byzantineBound count < 2 * participantThreshold count := by
  43   unfold participantThreshold byzantineBound
  44   omega
  45
```

The minimality theorem is what keeps the function honest: anything below this
floor fails the strict-intersection inequality.

📍 `formal/lean/xahau_consensus/XahauConsensus/Threshold.lean:81-91`
```text
  81 theorem participantThreshold_minimal (count threshold : Nat) :
  82     count + byzantineBound count < 2 * threshold →
  83       participantThreshold count <= threshold := by
  84   intro hSafe
  85   unfold participantThreshold
  86   exact threshold_minimal_for_boundary
  87     (count + byzantineBound count)
  88     threshold
  89     hSafe
  90
```

The branch originally talked in terms of “60%.” The formal model records why
that shorthand is dangerous at multiples of five:

📍 `formal/lean/xahau_consensus/XahauConsensus/SixtyPercent.lean:37-44`
```text
  37 theorem naiveSixtyPercentThreshold_five_mul_not_intersection_safe
  38     (k : Nat) :
  39     ¬ 5 * k + byzantineBound (5 * k) <
  40       2 * naiveSixtyPercentThreshold (5 * k) := by
  41   rw [naiveSixtyPercentThreshold_five_mul_hits_intersection_boundary k]
  42   omega
  43
```

## 2. nUNL Changes The Denominators

The implementation deliberately keeps two counts:

- `size()`: effective post-nUNL active validator view, used for 80% validator
  quorum.
- `originalViewSize`: pre-nUNL active validator view, used for the Tier 2
  participant floor.

📍 [`src/xrpld/app/consensus/ActiveValidatorView.cpp:24-60`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ActiveValidatorView.cpp#L24-L60)
```cpp
  24 ActiveValidatorView
  25 buildActiveValidatorView(
  26     ActiveValidatorViewSource const& source,
  27     ActiveValidatorViewFallback const& fallback)
  28 {
  29     ActiveValidatorView view;
  30     view.sourceLedgerHash = source.sourceLedgerHash;
  31
  32     if (source.unlReportMasterKeys && !source.unlReportMasterKeys->empty())
  33     {
  34         for (auto const& masterKey : *source.unlReportMasterKeys)
  35             view.insertMaster(masterKey);
  36         view.fromUNLReport = true;
  37     }
  38     else
  39     {
  40         for (auto const& masterKey : fallback.trustedMasterKeys)
  41             view.insertMaster(masterKey);
  42         if (fallback.localMasterKey)
  43             view.insertMaster(*fallback.localMasterKey);
  44     }
  45
  46     // Capture the pre-nUNL denominator before subtracting negatives. size()
  47     // remains the effective (post-nUNL) count used by the 80% validator-quorum
  48     // gate; originalViewSize is the original-UNL count that the Tier 2
  49     // (participant_aligned) intersection floor anchors to, since nUNL can
  50     // shrink the effective view while leaving faulty nodes in it.
  51     view.originalViewSize = view.masterKeys.size();
  52
  53     if (source.negativeUNLEnabled)
  54     {
  55         for (auto const& masterKey : source.negativeUNL)
  56             view.eraseMaster(masterKey);
  57     }
  58
  59     return view;
  60 }
```

📍 [`src/xrpld/app/consensus/ConsensusExtensions.cpp:208-219`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ConsensusExtensions.cpp#L208-L219)
```cpp
 208 std::size_t
 209 ConsensusExtensions::quorumThreshold() const
 210 {
 211     // Validator_quorum entropy uses a fixed 80% threshold over the effective
 212     // active UNL snapshot. Tier 2 participant_aligned entropy has its own
 213     // lower intersection-safe floor; recent proposers are useful for liveness
 214     // heuristics, but they do not lower either threshold.
 215     // Use the shared validator view so Tier 3 RNG and Export use the same
 216     // denominator.
 217     auto const base = activeValidatorView()->size();
 218     return safeQuorumThreshold(base);
 219 }
```

📍 [`src/xrpld/app/consensus/ConsensusExtensions.cpp:233-248`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ConsensusExtensions.cpp#L233-L248)
```cpp
 233 std::size_t
 234 ConsensusExtensions::tier2Threshold() const
 235 {
 236     // Tier 2 (participant_aligned) lowers the alignment bar from the 80%
 237     // validator-quorum gate to the quorum-intersection floor over the ORIGINAL
 238     // (pre-nUNL) view (~60%; exact value from calculateParticipantThreshold,
 239     // which keeps two aligned cohorts sharing an honest validator so an
 240     // equivocator cannot mint two distinct digests). Anchored to
 241     // originalViewSize, not size(): nUNL can shrink the effective view while
 242     // leaving faulty nodes in it, so a fraction of the effective view could
 243     // exceed the Byzantine fraction (which is bounded over the original UNL).
 244     // Regressing this to size() is a consensus fork under nUNL and is pinned by
 245     // ConsensusExtensions_test::testTier2ThresholdAnchorsToOriginalView.
 246     auto const base = activeValidatorView()->originalViewSize;
 247     return safeParticipantThreshold(base);
 248 }
```

The core theorem says original-view anchoring remains intersection-safe when
nUNL shrinks the effective view:

📍 `formal/lean/xahau_consensus/XahauConsensus/Threshold.lean:52-60`
```text
  52 theorem participantThreshold_safe_under_effective_shrink
  53     (originalView effectiveView : Nat)
  54     (hShrink : effectiveView <= originalView) :
  55     effectiveView + byzantineBound originalView <
  56       2 * participantThreshold originalView := by
  57   have hSafe := participantThreshold_intersection_safe originalView
  58   omega
  59
```

The Lean workspace also records an important correction from review: the protocol
cap is ceil-25%, and exact integer thresholds can cross either way. At
`original=10`, max disabled is `3`, so effective view is `7` and effective
quorum no longer reaches the original participant floor.

📍 `formal/lean/xahau_consensus/XahauConsensus/NunlCap.lean:19-20`
```text
  19 def disabledCap (originalView : Nat) : Nat :=
  20   ceilDiv originalView 4
```

📍 `formal/lean/xahau_consensus/XahauConsensus/NunlCap.lean:51-55`
```text
  51 theorem effectiveView_ten_at_disabledCap :
  52     effectiveView 10 (disabledCap 10) = 7 := by
  53   native_decide
  54
```

📍 `formal/lean/xahau_consensus/XahauConsensus/NunlCap.lean:82-87`
```text
  82 theorem max_cap_original10_below_participant_floor :
  83     quorumThreshold (effectiveView 10 (disabledCap 10)) <
  84       participantThreshold 10 := by
  85   native_decide
  86
```

That is not a bug. The gate only decides whether the pipeline continues;
`selectEntropy` later labels the agreed set from the same count. When
effective-view quorum is the lower threshold, a non-fallback set that reaches
that threshold is labeled `validator_quorum`, not Tier 2.

## 3. The Sidecar Alignment Universe Must Match The View

The F1 issue was that sidecar alignment originally risked counting a larger
universe than the threshold denominator. The fixed C++ filters peers through the
active view and only counts the local `+1` if this node is active.

📍 [`src/xrpld/consensus/ConsensusExtensionsTick.h:101-157`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/consensus/ConsensusExtensionsTick.h#L101-L157)
```cpp
 101 template <
 102     class PeerPositions,
 103     class Position,
 104     class GetHash,
 105     class IsMember,
 106     class OnMismatch>
 107 SidecarPeerAlignment
 108 inspectTxConvergedSidecarPeers(
 109     PeerPositions const& peerPositions,
 110     Position const& pos,
 111     bool localIsMember,
 112     GetHash getHash,
 113     IsMember isMember,
 114     OnMismatch onMismatch)
 115 {
 116     SidecarPeerAlignment state;
 117     auto const localHash = getHash(pos);
 118     if (!localHash)
 119         return state;
 120
 121     // The alignment-counting universe must be the active validator view, the
 122     // same denominator the entropy/export thresholds use (quorumThreshold /
 123     // tier2Threshold are computed over that view). A trusted-but-non-active
 124     // proposer can tx-converge and advertise a sidecar hash, but it must NOT
 125     // pad alignedParticipants(): counting outside the active view inflates the
 126     // universe N above originalViewSize and erodes the Tier-2 intersection
 127     // margin (2t - N) below the Byzantine floor f, breaking equivocation
 128     // uniqueness. Mirror buildEntropySet/hasQuorumOfCommits' containsNode
 129     // filter, and only count our own +1 when this node is itself active.
 130     state.localCounts = localIsMember;
 131     for (auto const& [nodeId, peerPos] : peerPositions)
 132     {
 133         if (!isMember(nodeId))
 134             continue;  // outside the active view -> not in the counting
 135                        // universe
 136         auto const& pp = peerPos.proposal().position();
 137         if (positionTxSetID(pp) != positionTxSetID(pos))
 138             continue;  // not tx-converged
 139         ++state.txConverged;
 140
 141         auto const peerHash = getHash(pp);
 142         if (!peerHash)
 143             continue;  // peer hasn't published this sidecar hash yet
 144         ++state.peersSeen;
 145
 146         if (*peerHash == *localHash)
 147         {
 148             ++state.aligned;
 149             continue;
 150         }
 151
 152         state.conflict = true;
 153         onMismatch(peerHash);
 154     }
 155
 156     return state;
 157 }
```

The Lean model has the same shape: a local publication contributes only when
`localIsMember && localPublished`, and nonmember peers cannot change the aligned
participant count.

📍 `formal/lean/xahau_consensus/XahauConsensus/SidecarAlignment.lean:12-15`
```text
  12 def alignedParticipants
  13     (aligned : Nat)
  14     (localIsMember localPublished : Bool) : Nat :=
  15   aligned + localPublishedCount (localIsMember && localPublished)
```

📍 `formal/lean/xahau_consensus/XahauConsensus/SidecarAlignment.lean:57-62`
```text
  57 theorem alignedParticipants_local_nonmember
  58     (aligned : Nat) (localPublished : Bool) :
  59     alignedParticipants aligned false localPublished = aligned := by
  60   cases localPublished <;> rfl
  61
```

📍 `formal/lean/xahau_consensus/XahauConsensus/SidecarAlignment.lean:222-241`
```text
 222 theorem quorumAligned_ext_on_members
 223     {n threshold : Nat} {inActiveView alignedA alignedB : Nat → Bool}
 224     {localIsMember : Bool}
 225     {localPublished : Bool}
 226     (hSameOnMembers :
 227       ∀ peer, peer < n → inActiveView peer = true →
 228         alignedA peer = alignedB peer) :
 229     quorumAligned threshold
 230         (activeAlignedCount inActiveView alignedA n)
 231         localIsMember
 232         localPublished =
 233       quorumAligned threshold
 234         (activeAlignedCount inActiveView alignedB n)
 235         localIsMember
 236         localPublished := by
 237   simp [
 238     quorumAligned,
 239     alignedParticipants_ext_on_members hSameOnMembers]
 240
```

The finite-set bridge gives the arithmetic overlap proof its real cardinality
premise instead of assuming it informally:

📍 `formal/lean/xahau_consensus/XahauConsensus/FinsetIntersection.lean:54-71`
```text
  54 theorem finset_participant_threshold_cohorts_overlap_gt_byzantine_under_shrink
  55     [DecidableEq α]
  56     {Original Effective A B : Finset α}
  57     (hEffectiveSubset : Effective ⊆ Original)
  58     (hAUniverse : A ⊆ Effective)
  59     (hBUniverse : B ⊆ Effective)
  60     (hAThreshold : participantThreshold Original.card <= A.card)
  61     (hBThreshold : participantThreshold Original.card <= B.card) :
  62     byzantineBound Original.card < (A ∩ B).card := by
  63   have hShrink : Effective.card <= Original.card :=
  64     Finset.card_le_card hEffectiveSubset
  65   exact participant_threshold_cohorts_overlap_gt_byzantine_under_shrink
  66     hShrink
  67     (finset_cardinality_bound hAUniverse hBUniverse)
  68     hAThreshold
  69     hBThreshold
  70
```

## 4. Tier Labels Require A Ledger-Anchored View

The selector has a fail-closed rule: if the active validator view is not backed
by a UNLReport, non-standalone nodes mint `consensus_fallback`. That avoids a
node-local trusted-config view changing the label for the same agreed entropy
set.

📍 [`src/xrpld/app/consensus/ConsensusExtensions.cpp:483-503`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ConsensusExtensions.cpp#L483-L503)
```cpp
 483     if (!validatorView->fromUNLReport)
 484     {
 485         if (validatorView->sourceLedgerHash)
 486         {
 487             XRPL_ASSERT(
 488                 *validatorView->sourceLedgerHash == roundPrevLedgerHash_,
 489                 "ripple::ConsensusExtensions::selectEntropy : "
 490                 "active view source matches round parent");
 491         }
 492         JLOG(j_.warn()) << "RNG: using consensus fallback entropy"
 493                         << " reason=no-unl-report"
 494                         << " seq=" << seq
 495                         << " activeValidators=" << validatorView->size()
 496                         << " originalView=" << validatorView->originalViewSize
 497                         << " sourceLedgerHash="
 498                         << (validatorView->sourceLedgerHash
 499                                 ? to_string(*validatorView->sourceLedgerHash)
 500                                 : std::string{"none"})
 501                         << " roundPrevLedgerHash=" << roundPrevLedgerHash_;
 502         return fallback();
 503     }
```

📍 `formal/lean/xahau_consensus/XahauConsensus/EntropySelector.lean:35-41`
```text
  35 theorem no_unl_report_selects_fallback
  36     (participantCount effectiveView originalView : Nat) :
  37     selectEntropyTier false participantCount effectiveView originalView =
  38       EntropyTier.consensusFallback := by
  39   rfl
  40
```

The ladder itself is over the agreed entropy set count. Quorum tier is checked
first; then participant-aligned; otherwise fallback.

📍 [`src/xrpld/app/consensus/ConsensusExtensions.cpp:555-568`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ConsensusExtensions.cpp#L555-L568)
```cpp
 555     // Tier ladder over the AGREED participant count — deterministic on every
 556     // node holding this entropySetHash. quorumThreshold() = ceil(0.8 *
 557     // effective view); tier2Threshold() = the equivocation-safe intersection
 558     // floor over the original view (~0.6*n; see calculateParticipantThreshold).
 559     // Below tier2Threshold too few aligned participants contributed to trust
 560     // the result — fall back.
 561     auto const tier = selectEntropyTierForView(
 562         validatorView->fromUNLReport,
 563         count,
 564         validatorView->size(),
 565         validatorView->originalViewSize);
 566     if (tier != entropyTierConsensusFallback)
 567         return {digest, static_cast<std::uint8_t>(tier), count};
 568     return fallback();
```

📍 `formal/lean/xahau_consensus/XahauConsensus/EntropySelector.lean:21-31`
```text
  21 def selectEntropyTier
  22     (fromUNLReport : Bool)
  23     (participantCount effectiveView originalView : Nat) : EntropyTier :=
  24   if !fromUNLReport then
  25     EntropyTier.consensusFallback
  26   else if participantCount >= safeQuorumThreshold effectiveView then
  27     EntropyTier.validatorQuorum
  28   else if participantCount >= safeParticipantThreshold originalView then
  29     EntropyTier.participantAligned
  30   else
  31     EntropyTier.consensusFallback
```

The cross-module invariant ties the threshold formula to the selector's tier
labels: reaching the lower of validator quorum and participant floor is exactly
the selector's non-fallback boundary. It does not model the runtime state
machine's extra full-observation wait or timeout-driven `entropyFailed_`
downgrade. The theorem is intentionally scoped to the network, non-failed,
non-empty selector path; standalone, timeout downgrade, and empty-map fallback
are explicit bypass/downgrade paths.

📍 [`src/xrpld/app/consensus/ConsensusExtensions.cpp:250-266`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ConsensusExtensions.cpp#L250-L266)
```cpp
 250 std::size_t
 251 ConsensusExtensions::entropyGateThreshold() const
 252 {
 253     // The bar at which the commit/reveal/entropy pipeline engages and the
 254     // entropy conflict gate resolves: the lowest ENABLED accepted tier's
 255     // threshold. In the normal band tier2Threshold (~0.6*original) < quorum
 256     // (0.8*effective), so this is the participant-alignment floor and
 257     // sub-quorum rounds reach injection. Under nUNL the exact integer
 258     // thresholds can cross either way; that only changes which threshold lets
 259     // the pipeline proceed. Final tier labels are computed later from the
 260     // agreed entropy set count in selectEntropy(). Non-fallback tier labels are
 261     // allowed only when the round view is anchored by UNLReport; the
 262     // trusted-fallback view is local configuration and selectEntropy() maps it
 263     // to consensus_fallback.
 264     auto const view = activeValidatorView();
 265     return entropyGateThresholdForView(view->size(), view->originalViewSize);
 266 }
```

📍 `formal/lean/xahau_consensus/XahauConsensus/Invariants.lean:38-39`
```text
  38 def entropyGateThresholdForView (effectiveView originalView : Nat) : Nat :=
  39   min (safeQuorumThreshold effectiveView) (safeParticipantThreshold originalView)
```

📍 `formal/lean/xahau_consensus/XahauConsensus/Invariants.lean:58-95`
```text
  58 theorem selectEntropyTier_nonfallback_iff_entropy_gate
  59     (participantCount effectiveView originalView : Nat) :
  60     selectEntropyTier true participantCount effectiveView originalView ≠
  61         EntropyTier.consensusFallback ↔
  62       entropyGateThresholdForView effectiveView originalView <=
  63         participantCount := by
  64   unfold selectEntropyTier entropyGateThresholdForView
  65   by_cases hQuorum : safeQuorumThreshold effectiveView <= participantCount
  66   · constructor
  67     · intro _
  68       exact Nat.le_trans (Nat.min_le_left _ _) hQuorum
  69     · intro _
  70       simp [hQuorum]
  71   · by_cases hParticipant :
  72       safeParticipantThreshold originalView <= participantCount
  73     · constructor
  74       · intro _
  75         exact Nat.le_trans (Nat.min_le_right _ _) hParticipant
  76       · intro _
  77         simp [hQuorum, hParticipant]
  78     · constructor
  79       · intro hNonfallback
  80         simp [hQuorum, hParticipant] at hNonfallback
  81       · intro hGate
  82         have hBelowQuorum :
  83             participantCount < safeQuorumThreshold effectiveView :=
  84           Nat.lt_of_not_ge hQuorum
  85         have hBelowParticipant :
  86             participantCount < safeParticipantThreshold originalView :=
  87           Nat.lt_of_not_ge hParticipant
  88         have hBelowGate :
  89             participantCount <
  90               min (safeQuorumThreshold effectiveView)
  91                 (safeParticipantThreshold originalView) :=
  92           (Nat.lt_min).mpr ⟨hBelowQuorum, hBelowParticipant⟩
  93         exact False.elim (Nat.not_lt_of_ge hGate hBelowGate)
  94
```

## 5. Export Has No Fallback Value

Export uses the same sidecar-alignment machinery, but its failure mode is
different. RNG has a protocol-defined deterministic Tier 1 fallback digest.
Export cannot synthesize a quorum-valid replacement signature set. Below quorum
network mode retries or expires.

The no-veto decision is that a quorum-aligned export sidecar hash lets the
sidecar gate proceed even when not every tx-converged active peer has published
a sidecar hash. Otherwise one missing minority sidecar could force retry/expiry
every round. Final export application still happens later in `Export::doApply`,
which assembles from the agreed sidecar snapshot and verifies quorum material.

📍 [`src/xrpld/consensus/ConsensusExtensionsTick.h:1199-1228`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/consensus/ConsensusExtensionsTick.h#L1199-L1228)
```cpp
1199                 if (exportState.conflict && quorumAligned())
1200                 {
1201                     // Export sidecar roots are signed through ExtendedPosition
1202                     // whenever featureExport is active. A quorum-aligned hash
1203                     // is therefore enough to proceed; requiring every
1204                     // tx-converged active peer to publish an exportSigSetHash
1205                     // would let a missing minority sidecar force retry/expiry.
1206                     JLOG(ext.j_.info())
1207                         << "Export: exportSigSetHash conflict ignored"
1208                         << " reason=quorum-aligned"
1209                         << " buildSeq=" << buildSeqExport
1210                         << " alignedParticipants="
1211                         << exportState.alignedParticipants()
1212                         << " quorum=" << exportQuorum
1213                         << " peersSeen=" << exportState.peersSeen
1214                         << " txConverged=" << exportState.txConverged;
1215                 }
1216                 else if (quorumAligned() && !exportState.fullObservation())
1217                 {
1218                     JLOG(ext.j_.info())
1219                         << "Export: missing exportSigSetHash observation "
1220                            "ignored"
1221                         << " reason=quorum-aligned"
1222                         << " buildSeq=" << buildSeqExport
1223                         << " alignedParticipants="
1224                         << exportState.alignedParticipants()
1225                         << " quorum=" << exportQuorum
1226                         << " peersSeen=" << exportState.peersSeen
1227                         << " txConverged=" << exportState.txConverged;
1228                 }
```

📍 [`src/xrpld/app/consensus/ConsensusExtensions.cpp:221-231`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/consensus/ConsensusExtensions.cpp#L221-L231)
```cpp
 221 std::size_t
 222 ConsensusExtensions::exportSigQuorumThreshold() const
 223 {
 224     auto const base = activeValidatorView()->size();
 225
 226     // Export sidecar hashes are signed through ExtendedPosition even when RNG
 227     // is disabled, so a quorum-aligned exportSigSetHash is deterministic
 228     // enough for Export-only mode. Unanimity would let one active validator
 229     // veto an otherwise converged export round.
 230     return safeQuorumThreshold(base);
 231 }
```

📍 `formal/lean/xahau_consensus/XahauConsensus/ExportGate.lean:19-22`
```text
  19 inductive ExportOutcome where
  20   | proceed
  21   | retryOrExpire
  22   deriving DecidableEq, Repr
```

In this minimal model, `proceed` means the export sidecar gate proceeds; it is
not a claim that `Export::doApply` has already assembled and applied the export.

📍 `formal/lean/xahau_consensus/XahauConsensus/ExportGate.lean:95-102`
```text
  95 theorem missing_minority_does_not_prevent_proceed
  96     {alignedParticipants quorumThreshold : Nat}
  97     (hQuorum : quorumThreshold <= alignedParticipants) :
  98     (ExportGate.mk alignedParticipants quorumThreshold false).proceed = true := by
  99   unfold ExportGate.proceed ExportGate.quorumAligned
 100   simp [hQuorum]
 101
```

📍 `formal/lean/xahau_consensus/XahauConsensus/ExportGate.lean:102-110`
```text
 102 theorem missing_minority_proceeds
 103     {alignedParticipants quorumThreshold : Nat}
 104     (hQuorum : quorumThreshold <= alignedParticipants) :
 105     (ExportGate.mk alignedParticipants quorumThreshold false).outcome =
 106       ExportOutcome.proceed := by
 107   unfold ExportGate.outcome
 108   simp [missing_minority_does_not_prevent_proceed hQuorum]
 109
```

📍 `formal/lean/xahau_consensus/XahauConsensus/ExportGate.lean:133-139`
```text
 133 theorem changing_fullObservation_alone_does_not_change_proceed
 134     (alignedParticipants quorumThreshold : Nat) :
 135     (ExportGate.mk alignedParticipants quorumThreshold true).proceed =
 136       (ExportGate.mk alignedParticipants quorumThreshold false).proceed := by
 137   rfl
 138
```

📍 `formal/lean/xahau_consensus/XahauConsensus/ExportGate.lean:122-131`
```text
 122 theorem below_quorum_retries_or_expires
 123     {alignedParticipants quorumThreshold : Nat}
 124     (fullObservation : Bool)
 125     (hBelow : alignedParticipants < quorumThreshold) :
 126     (ExportGate.mk alignedParticipants quorumThreshold fullObservation).outcome =
 127       ExportOutcome.retryOrExpire := by
 128   unfold ExportGate.outcome
 129   simp [below_quorum_does_not_proceed fullObservation hBelow]
 130
```

Network apply then uses the agreed sidecar snapshot, not the live collector. If
that agreed snapshot is missing or below quorum, network mode retries or expires
instead of building a local-only export result.

📍 [`src/xrpld/app/tx/detail/Export.cpp:176-189`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/tx/detail/Export.cpp#L176-L189)
```cpp
 176         std::size_t const threshold = safeQuorumThreshold(unlSize);
 177
 178         if (!validatorView->fromUNLReport)
 179         {
 180             JLOG(j_.warn())
 181                 << "Export: retrying without ledger-anchored validator view"
 182                 << " txHash=" << txId << " ledgerSeq=" << currentSeq
 183                 << " unlSize=" << unlSize << " threshold=" << threshold;
 184         }
 185         else if (!consensusExtensions.exportSigConvergenceFailed())
 186         {
 187             collectedSigs = consensusExtensions.agreedExportSignatures(
 188                 ctx_.tx, txId, *validatorView, threshold);
 189         }
```

📍 [`src/xrpld/app/tx/detail/Export.cpp:193-242`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/xrpld/app/tx/detail/Export.cpp#L193-L242)
```cpp
 193         if (!collectedSigs)
 194         {
 195             auto const sigCount =
 196                 consensusExtensions.exportSigCollector().signatureCount(
 197                     txId, isActiveSigner);
 198             // LLS semantics for retriable exports:
 199             //
 200             // Transactor::preclaim rejects with tefMAX_LEDGER when
 201             // seq > LLS, so this tx can never run past ledger LLS.
 202             // Within that window the export has three possible outcomes
 203             // each ledger:
 204             //
 205             //   ledger < LLS:  tesSUCCESS (quorum) or terRETRY_EXPORT
 206             //   ledger == LLS: tesSUCCESS (quorum) or tecEXPORT_EXPIRED
 207             //   ledger > LLS:  tefMAX_LEDGER (never reaches doApply)
 208             //
 209             // The >= check here only fires in the no-quorum branch, so
 210             // if quorum IS met on the LLS ledger it still succeeds.
 211             // tecEXPORT_EXPIRED consumes the sequence cleanly rather
 212             // than letting tefMAX_LEDGER silently drop the tx.
 213             if (ctx_.tx.isFieldPresent(sfLastLedgerSequence))
 214             {
 215                 auto const lls = ctx_.tx.getFieldU32(sfLastLedgerSequence);
 216                 if (currentSeq >= lls)
 217                 {
 218                     ctx_.app.getConsensusExtensions()
 219                         .exportSigCollector()
 220                         .clear(txId);
 221                     JLOG(j_.info())
 222                         << "Export: last ledger expired"
 223                         << " txHash=" << txId << " ledgerSeq=" << currentSeq
 224                         << " lastLedgerSequence=" << lls << " sigs=" << sigCount
 225                         << " threshold=" << threshold << " unlSize=" << unlSize
 226                         << " result=tecEXPORT_EXPIRED";
 227                     return tecEXPORT_EXPIRED;
 228                 }
 229             }
 230
 231             upgradeUnverifiedForNextRound();
 232
 233             JLOG(j_.info())
 234                 << "Export: insufficient signatures"
 235                 << " txHash=" << txId << " ledgerSeq=" << currentSeq
 236                 << " sigs=" << sigCount << " threshold=" << threshold
 237                 << " unlSize=" << unlSize << " exportSigConvergenceFailed="
 238                 << (consensusExtensions.exportSigConvergenceFailed() ? "yes"
 239                                                                      : "no")
 240                 << " result=terRETRY_EXPORT";
 241             return terRETRY_EXPORT;
 242         }
```

The export quorum proof also has the nUNL-shrink version: with shrinkage within
the protocol cap, two export sidecar quorums in the effective view still overlap
above the original-view Byzantine bound.

📍 `formal/lean/xahau_consensus/XahauConsensus/ExportQuorum.lean:132-150`
```text
 132 theorem export_hash_quorums_overlap_gt_original_byzantine_under_nunl_cap
 133     {originalView effectiveView disabled a b overlap : Nat}
 134     (hEffective : effectiveView = originalView - disabled)
 135     (hCap : disabled <= disabledCap originalView)
 136     (hPositive : 0 < effectiveView)
 137     (hCardinality : a + b <= effectiveView + overlap)
 138     (hA : quorumThreshold effectiveView <= a)
 139     (hB : quorumThreshold effectiveView <= b) :
 140     byzantineBound originalView < overlap := by
 141   exact overlap_gt_fault_of_two_threshold_cohorts
 142     hCardinality
 143     hA
 144     hB
 145     (export_quorum_intersection_safe_under_nunl_cap
 146       hEffective
 147       hCap
 148       hPositive)
 149
```

## 6. Regression Tests That Pin The Correspondence

The Lean proofs constrain formulas and decision predicates. The C++ and testnet
tests are what make those predicates hard to accidentally drift from the
implementation.

The threshold test checks the concrete off-by-one cases and the defining
intersection inequality for every view size up to 256:

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:1097-1127`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L1097-L1127)
```cpp
1097 void
1098     testParticipantThreshold()
1099     {
1100         testcase("participant-alignment threshold arithmetic");
1101
1102         // Smallest cohort t with 2t - n > floor(n/5) tolerated faults: ~0.6 n,
1103         // but bumped at multiples of 5 where plain ceil(0.6 n) leaves the
1104         // cohort overlap exactly == f (forkable).
1105         BEAST_EXPECT(calculateParticipantThreshold(1) == 1);
1106         BEAST_EXPECT(
1107             calculateParticipantThreshold(5) == 4);  // not 3 (n=5 forks)
1108         BEAST_EXPECT(calculateParticipantThreshold(6) == 4);  // == ceil(0.6*6)
1109         BEAST_EXPECT(calculateParticipantThreshold(7) == 5);
1110         BEAST_EXPECT(calculateParticipantThreshold(8) == 5);
1111         BEAST_EXPECT(
1112             calculateParticipantThreshold(10) == 7);  // not 6 (n=10 forks)
1113         BEAST_EXPECT(calculateParticipantThreshold(15) == 10);
1114         BEAST_EXPECT(calculateParticipantThreshold(20) == 13);
1115
1116         // The defining safety invariant, for EVERY view size: two aligned
1117         // cohorts always share an honest validator, i.e. their overlap (2t - n)
1118         // strictly exceeds the tolerated Byzantine count f = floor(n/5). And
1119         // the Tier 2 bar is never stricter than the Tier 3 validator_quorum
1120         // bar.
1121         for (std::size_t n = 1; n <= 256; ++n)
1122         {
1123             auto const t = calculateParticipantThreshold(n);
1124             BEAST_EXPECT(2 * t > n + n / 5);  // overlap 2t-n > floor(n/5)
1125             BEAST_EXPECT(t <= calculateQuorumThreshold(n));
1126         }
1127     }
```

The selector test drives the real `onPreBuild` path across all three labels in
the first reachable full-view Tier 2 band: `5/6 -> validator_quorum`,
`4/6 -> participant_aligned`, and `3/6 -> consensus_fallback`.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:1349-1446`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L1349-L1446)
```cpp
1349 void
1350     testOnPreBuildTier2ParticipantAligned()
1351     {
1352         testcase(
1353             "onPreBuild labels sub-quorum aligned set participant_aligned");
1354
1355         using namespace jtx;
1356         Env env{
1357             *this,
1358             envconfig(validator, ""),
1359             supported_amendments() | featureConsensusEntropy,
1360             nullptr};
1361         forceNonStandalone(env.app());
1362
1363         // A 6-validator UNLReport opens the tier-2 band:
1364         //   tier2Threshold  = 4 (smallest cohort whose overlap exceeds f=1)
1365         //   quorumThreshold = ceil(0.8 * 6) = 5
1366         // so an agreed aligned count of 4 is participant_aligned, 5+ is
1367         // validator_quorum, and < 4 falls back. (n=5 has no band -- there
1368         // tier2 == quorum -- so 6 is the smallest size that exercises tier 2.)
1369         constexpr std::size_t kValidators = 6;
1370         std::vector<std::pair<PublicKey, SecretKey>> vals;
1371         vals.reserve(kValidators);
1372         std::vector<PublicKey> activeKeys;
1373         activeKeys.reserve(kValidators);
1374         for (std::size_t i = 0; i < kValidators; ++i)
1375         {
1376             vals.push_back(randomKeyPair(KeyType::secp256k1));
1377             activeKeys.push_back(vals.back().first);
1378         }
1379         auto const viewLedger = makeUNLReportLedger(env, activeKeys);
1380
1381         // Thresholds derive from the cached pre-nUNL view (originalViewSize=6).
1382         {
1383             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1384             ce.cacheUNLReport(viewLedger);
1385             BEAST_EXPECT(ce.activeValidatorView()->originalViewSize == 6);
1386             BEAST_EXPECT(ce.quorumThreshold() == 5);
1387             BEAST_EXPECT(ce.tier2Threshold() == 4);
1388             BEAST_EXPECT(ce.entropyGateThreshold() == 4);
1389         }
1390
1391         // Reveal verification resolves the round's prev ledger through the
1392         // LedgerMaster, so anchor commitments to a real closed ledger (the view
1393         // above comes from the in-memory UNLReport ledger).
1394         auto const anchor = env.app().getLedgerMaster().getClosedLedger();
1395         auto const prevLedger = anchor->info().hash;
1396         auto const seq = anchor->info().seq + 1;
1397         auto const closeTime = NetClock::time_point{NetClock::duration{777}};
1398         auto const txSetHash = makeHash("tier2-txset");
1399
1400         // Harvest commit+reveal from `revealers` of the 6 validators, build the
1401         // agreed entropy set, inject, and return the labelled (tier, count).
1402         auto runWith = [&](std::size_t revealers) {
1403             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1404             ce.cacheUNLReport(viewLedger);
1405             for (std::size_t i = 0; i < revealers; ++i)
1406             {
1407                 auto const reveal =
1408                     sha512Half(vals[i].first, makeHash("tier2-reveal"));
1409                 harvestCommitReveal(
1410                     ce,
1411                     calcNodeID(vals[i].first),
1412                     vals[i].first,
1413                     vals[i].second,
1414                     txSetHash,
1415                     seq,
1416                     closeTime,
1417                     prevLedger,
1418                     reveal);
1419             }
1420             ce.buildEntropySet(seq);
1421             CanonicalTXSet txs{makeHash("tier2-salt")};
1422             ce.onPreBuild(txs, seq, txSetHash);
1423             auto const tx = singleCanonicalTx(txs);
1424             std::pair<int, std::uint16_t> out{-1, 0};
1425             if (tx)
1426                 out = {
1427                     tx->getFieldU8(sfEntropyTier),
1428                     tx->getFieldU16(sfEntropyCount)};
1429             return out;
1430         };
1431
1432         // 5 of 6 aligned -> validator_quorum (count >= quorum 5).
1433         auto const q = runWith(5);
1434         BEAST_EXPECT(q.first == entropyTierValidatorQuorum);
1435         BEAST_EXPECT(q.second == 5);
1436
1437         // 4 of 6 aligned -> participant_aligned (count >= tier2 4, < quorum 5).
1438         auto const p = runWith(4);
1439         BEAST_EXPECT(p.first == entropyTierParticipantAligned);
1440         BEAST_EXPECT(p.second == 4);
1441
1442         // 3 of 6 aligned -> below the tier-2 floor -> consensus_fallback.
1443         auto const f = runWith(3);
1444         BEAST_EXPECT(f.first == entropyTierConsensusFallback);
1445         BEAST_EXPECT(f.second == 0);
1446     }
```

The nUNL anchor test is intentionally a denominator regression test. It pins
`originalViewSize` under NegativeUNL and would fail if Tier 2 switched to the
effective view. Its chosen `10 -> 8` case collapses the Tier 2 band, so it is
paired with the next test, which mints Tier 2 under active nUNL in a
non-collapsed band.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:1448-1513`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L1448-L1513)
```cpp
1448 void
1449     testTier2ThresholdAnchorsToOriginalView()
1450     {
1451         testcase(
1452             "Tier 2 threshold anchors to pre-nUNL original view under "
1453             "NegativeUNL");
1454
1455         using namespace jtx;
1456         Env env{
1457             *this,
1458             envconfig(validator, ""),
1459             supported_amendments() | featureConsensusEntropy |
1460                 featureNegativeUNL,
1461             nullptr};
1462         forceNonStandalone(env.app());
1463
1464         // Ten active validators with two disabled via NegativeUNL: the
1465         // original-UNL denominator is 10 (Byzantine bound f = floor(10/5) = 2)
1466         // while the effective view is 8. Aligned cohorts form among the 8
1467         // effective validators, so two t-cohorts overlap by 2t - 8; safety
1468         // needs that overlap to exceed f so the two cohorts share an honest
1469         // validator. Tier 2 MUST key off the original view:
1470         //   tier2Threshold(original 10) == 7  -> overlap 2*7 - 8 = 6 > f: safe
1471         //   tier2Threshold(effective 8) == 5  -> overlap 2*5 - 8 = 2, NOT > f
1472         // A regression to size() would admit a 5-of-8 cohort as
1473         // participant_aligned, but its overlap (2) does not exceed the two
1474         // faulty validators the original UNL still tolerates, so an equivocator
1475         // could mint two distinct tier-2 digests: a fork. nUNL drops
1476         // honest-but-down nodes while leaving faulty ones, so the fault bound
1477         // stays anchored to the original UNL, not the shrunk effective view.
1478         // Under the correct anchor the band collapses (tier2 == quorum == 7)
1479         // and no tier-2 mint happens at all; this test pins the anchor so a
1480         // refactor to size() -- which re-opens the forkable [5, 7) band --
1481         // fails loudly.
1482         constexpr std::size_t kActive = 10;
1483         constexpr std::size_t kDisabled = 2;
1484         std::vector<PublicKey> activeKeys;
1485         activeKeys.reserve(kActive);
1486         for (std::size_t i = 0; i < kActive; ++i)
1487             activeKeys.push_back(randomKeyPair(KeyType::secp256k1).first);
1488         std::vector<PublicKey> const disabledKeys(
1489             activeKeys.begin(), activeKeys.begin() + kDisabled);
1490
1491         auto const viewLedger =
1492             makeUNLReportLedger(env, activeKeys, disabledKeys);
1493         BEAST_EXPECT(viewLedger->rules().enabled(featureNegativeUNL));
1494
1495         ConsensusExtensions ce{env.app(), activeNoopJournal()};
1496         ce.cacheUNLReport(viewLedger);
1497         auto const view = ce.activeValidatorView();
1498
1499         BEAST_EXPECT(view->fromUNLReport);
1500         BEAST_EXPECT(view->originalViewSize == kActive);    // 10, pre-nUNL
1501         BEAST_EXPECT(view->size() == kActive - kDisabled);  // 8, effective
1502
1503         // The anchor. Tier 2 derives from originalViewSize (10 -> 7), NOT the
1504         // effective size (8 -> 5). The gate is min(quorum, tier2); quorum
1505         // tracks the effective view (8 -> 7), so under the correct anchor gate
1506         // == 7 and the band is closed, while a regression to size() would drop
1507         // the floor to 5 and the gate to min(7, 5) == 5.
1508         BEAST_EXPECT(calculateParticipantThreshold(kActive) == 7);
1509         BEAST_EXPECT(calculateParticipantThreshold(kActive - kDisabled) == 5);
1510         BEAST_EXPECT(ce.tier2Threshold() == 7);
1511         BEAST_EXPECT(ce.quorumThreshold() == 7);
1512         BEAST_EXPECT(ce.entropyGateThreshold() == 7);
1513     }
```

The active-nUNL mint test uses `20` original validators with `1` disabled:
effective size `19`, Tier 2 floor `13`, validator quorum `16`. That directly
exercises the composed path where `buildEntropySet` filters disabled validators
but `tier2Threshold` stays anchored to the original view.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:1515-1609`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L1515-L1609)
```cpp
1515 void
1516     testOnPreBuildTier2WithNegativeUNL()
1517     {
1518         testcase("onPreBuild mints Tier 2 with active NegativeUNL");
1519
1520         using namespace jtx;
1521         Env env{
1522             *this,
1523             envconfig(validator, ""),
1524             supported_amendments() | featureConsensusEntropy |
1525                 featureNegativeUNL,
1526             nullptr};
1527         forceNonStandalone(env.app());
1528
1529         // 20 original active validators, 1 disabled by nUNL:
1530         //   originalViewSize = 20 -> tier2Threshold = 13
1531         //   effective size   = 19 -> quorumThreshold = 16
1532         // so counts 13..15 are genuinely participant_aligned under active nUNL.
1533         constexpr std::size_t kOriginal = 20;
1534         constexpr std::size_t kDisabled = 1;
1535         std::vector<std::pair<PublicKey, SecretKey>> vals;
1536         vals.reserve(kOriginal);
1537         std::vector<PublicKey> activeKeys;
1538         activeKeys.reserve(kOriginal);
1539         for (std::size_t i = 0; i < kOriginal; ++i)
1540         {
1541             vals.push_back(randomKeyPair(KeyType::secp256k1));
1542             activeKeys.push_back(vals.back().first);
1543         }
1544         std::vector<PublicKey> const disabledKeys(
1545             activeKeys.begin(), activeKeys.begin() + kDisabled);
1546         auto const viewLedger =
1547             makeUNLReportLedger(env, activeKeys, disabledKeys);
1548
1549         {
1550             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1551             ce.cacheUNLReport(viewLedger);
1552             auto const view = ce.activeValidatorView();
1553             BEAST_EXPECT(view->fromUNLReport);
1554             BEAST_EXPECT(view->originalViewSize == kOriginal);
1555             BEAST_EXPECT(view->size() == kOriginal - kDisabled);
1556             BEAST_EXPECT(ce.tier2Threshold() == 13);
1557             BEAST_EXPECT(ce.quorumThreshold() == 16);
1558             BEAST_EXPECT(ce.entropyGateThreshold() == 13);
1559         }
1560
1561         auto const anchor = env.app().getLedgerMaster().getClosedLedger();
1562         auto const prevLedger = anchor->info().hash;
1563         auto const seq = anchor->info().seq + 1;
1564         auto const closeTime = NetClock::time_point{NetClock::duration{778}};
1565         auto const txSetHash = makeHash("tier2-nunl-txset");
1566
1567         auto runWith = [&](std::size_t revealers) {
1568             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1569             ce.cacheUNLReport(viewLedger);
1570             for (std::size_t i = 0; i < revealers; ++i)
1571             {
1572                 auto const idx = i + kDisabled;  // skip disabled validator
1573                 auto const reveal =
1574                     sha512Half(vals[idx].first, makeHash("tier2-nunl-reveal"));
1575                 harvestCommitReveal(
1576                     ce,
1577                     calcNodeID(vals[idx].first),
1578                     vals[idx].first,
1579                     vals[idx].second,
1580                     txSetHash,
1581                     seq,
1582                     closeTime,
1583                     prevLedger,
1584                     reveal);
1585             }
1586             ce.buildEntropySet(seq);
1587             CanonicalTXSet txs{makeHash("tier2-nunl-salt")};
1588             ce.onPreBuild(txs, seq, txSetHash);
1589             auto const tx = singleCanonicalTx(txs);
1590             std::pair<int, std::uint16_t> out{-1, 0};
1591             if (tx)
1592                 out = {
1593                     tx->getFieldU8(sfEntropyTier),
1594                     tx->getFieldU16(sfEntropyCount)};
1595             return out;
1596         };
1597
1598         auto const q = runWith(16);
1599         BEAST_EXPECT(q.first == entropyTierValidatorQuorum);
1600         BEAST_EXPECT(q.second == 16);
1601
1602         auto const p = runWith(15);
1603         BEAST_EXPECT(p.first == entropyTierParticipantAligned);
1604         BEAST_EXPECT(p.second == 15);
1605
1606         auto const f = runWith(12);
1607         BEAST_EXPECT(f.first == entropyTierConsensusFallback);
1608         BEAST_EXPECT(f.second == 0);
1609     }
```

The F1 regression test pins the sidecar counting universe: non-active trusted
peers cannot pad the aligned count, and a non-active local node does not
contribute the local `+1`.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:858-898`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L858-L898)
```cpp
 858         // F1: the alignment-counting universe must be the active validator
 859         // view, not the full trusted-proposer set. A trusted-but-non-active
 860         // proposer (node 5) that tx-converges and aligns on the SAME hash must
 861         // NOT inflate alignedParticipants() — otherwise two equivocation
 862         // cohorts padded by non-active peers could each clear the gate.
 863         harness.position.exportSigSetHash = localHash;
 864         harness.addPeer(5, localHash);  // trusted, but outside the active view
 865         auto const activeOnly = [](auto const& id) {
 866             return id != makeNode(5);  // nodes 1..4 active; 5 is not
 867         };
 868
 869         auto const padded = detail::inspectTxConvergedSidecarPeers(
 870             harness.peers,
 871             harness.position,
 872             true,
 873             exportHashOf,
 874             allMembers,
 875             [](auto const&) {});
 876         BEAST_EXPECT(padded.aligned == 2);  // node 1 + node 5, unfiltered
 877
 878         auto const filtered = detail::inspectTxConvergedSidecarPeers(
 879             harness.peers,
 880             harness.position,
 881             true,
 882             exportHashOf,
 883             activeOnly,
 884             [](auto const&) {});
 885         BEAST_EXPECT(filtered.aligned == 1);                // node 5 excluded
 886         BEAST_EXPECT(filtered.alignedParticipants() == 2);  // node 1 + local
 887         BEAST_EXPECT(!filtered.quorumAligned(3));  // padding can't reach quorum
 888
 889         // The local +1 is likewise gated on local active-view membership.
 890         auto const nonActiveLocal = detail::inspectTxConvergedSidecarPeers(
 891             harness.peers,
 892             harness.position,
 893             false,
 894             exportHashOf,
 895             activeOnly,
 896             [](auto const&) {});
 897         BEAST_EXPECT(!nonActiveLocal.localCounts);
 898         BEAST_EXPECT(nonActiveLocal.alignedParticipants() == 1);  // node 1 only
```

The split-brain sidecar regression constructs two local views that a Byzantine
advertiser could show to different honest cohorts. It demonstrates the two
threshold mistakes the proof work was meant to rule out: strict majority at
`n=9`, and naive `ceil(0.6n)` at the exact `n=10` multiple-of-five boundary.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:902-976`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L902-L976)
```cpp
 902 void
 903     testSidecarSplitBrainEquivocationThreshold()
 904     {
 905         testcase("Sidecar split-brain equivocation threshold");
 906
 907         auto const hashA = makeHash("split-brain-sidecar-a");
 908         auto const hashB = makeHash("split-brain-sidecar-b");
 909         auto const exportHashOf = [](auto const& position) {
 910             return position.exportSigSetHash;
 911         };
 912         auto const allMembers = [](auto const&) { return true; };
 913
 914         auto inspect = [&](std::uint8_t localId,
 915                            uint256 const& localHash,
 916                            std::vector<std::uint8_t> const& hashANodes,
 917                            std::vector<std::uint8_t> const& hashBNodes) {
 918             ExportTickHarness harness;
 919             harness.position.exportSigSetHash = localHash;
 920             auto addPeer = [&](std::uint8_t id, uint256 const& hash) {
 921                 if (id != localId)
 922                     harness.addPeer(id, hash);
 923             };
 924             for (auto id : hashANodes)
 925                 addPeer(id, hashA);
 926             for (auto id : hashBNodes)
 927                 addPeer(id, hashB);
 928
 929             return detail::inspectTxConvergedSidecarPeers(
 930                 harness.peers,
 931                 harness.position,
 932                 true,
 933                 exportHashOf,
 934                 allMembers,
 935                 [](auto const&) {});
 936         };
 937
 938         // n=9, f=floor(n/5)=1. One equivocator (node 8) can show hashA to
 939         // nodes 0..3 and hashB to nodes 4..7. A plain strict-majority gate
 940         // (5/9) would let both local views proceed on different hashes. The
 941         // participant threshold is 6, so neither split cohort can pass.
 942         {
 943             auto const viewA = inspect(0, hashA, {0, 1, 2, 3, 8}, {4, 5, 6, 7});
 944             auto const viewB = inspect(4, hashB, {0, 1, 2, 3}, {4, 5, 6, 7, 8});
 945             auto const strictMajority = std::size_t{5};
 946             auto const participant = calculateParticipantThreshold(9);
 947
 948             BEAST_EXPECT(viewA.alignedParticipants() == strictMajority);
 949             BEAST_EXPECT(viewB.alignedParticipants() == strictMajority);
 950             BEAST_EXPECT(viewA.quorumAligned(strictMajority));
 951             BEAST_EXPECT(viewB.quorumAligned(strictMajority));
 952             BEAST_EXPECT(participant == 6);
 953             BEAST_EXPECT(!viewA.quorumAligned(participant));
 954             BEAST_EXPECT(!viewB.quorumAligned(participant));
 955         }
 956
 957         // n=10, f=2. Two equivocators (8,9) make two disjoint honest cohorts
 958         // appear as 6/10 each. Naive ceil(0.6n) would fork at this exact
 959         // multiple of five; calculateParticipantThreshold bumps the floor to 7.
 960         {
 961             auto const viewA =
 962                 inspect(0, hashA, {0, 1, 2, 3, 8, 9}, {4, 5, 6, 7});
 963             auto const viewB =
 964                 inspect(4, hashB, {0, 1, 2, 3}, {4, 5, 6, 7, 8, 9});
 965             auto const naiveSixty = std::size_t{6};
 966             auto const participant = calculateParticipantThreshold(10);
 967
 968             BEAST_EXPECT(viewA.alignedParticipants() == naiveSixty);
 969             BEAST_EXPECT(viewB.alignedParticipants() == naiveSixty);
 970             BEAST_EXPECT(viewA.quorumAligned(naiveSixty));
 971             BEAST_EXPECT(viewB.quorumAligned(naiveSixty));
 972             BEAST_EXPECT(participant == 7);
 973             BEAST_EXPECT(!viewA.quorumAligned(participant));
 974             BEAST_EXPECT(!viewB.quorumAligned(participant));
 975         }
 976     }
```

The CSF harness now models the same Byzantine shape through recipient-specific
proposal sidecar roots. The origin authors all recipient variants up front, and
the broadcast relay preserves that map so each peer sees the origin's intended
variant regardless of the flooding path. That keeps the extension narrow:
normal CSF messages are unchanged; only tests that populate
`equivocateSidecarsTo_` get per-recipient sidecar hashes.

📍 [`src/test/csf/Peer.h:642-653`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/csf/Peer.h#L642-L653)
```cpp
 642 bc::flat_map<PeerID, Proposal>
 643         proposalEquivocations(Proposal const& proposal) const
 644         {
 645             bc::flat_map<PeerID, Proposal> out;
 646             for (auto const& [recipient, _] : equivocateSidecarsTo_)
 647             {
 648                 out.emplace(
 649                     recipient,
 650                     proposalWithRecipientSidecarHashes(proposal, recipient));
 651             }
 652             return out;
 653         }
```

📍 [`src/test/csf/Peer.h:1632-1661`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/csf/Peer.h#L1632-L1661)
```cpp
1632 template <class M>
1633     void
1634     send(BroadcastMesg<M> const& bm, PeerID from)
1635     {
1636         for (auto const link : net.links(this))
1637         {
1638             if (link.target->id != from && link.target->id != bm.origin)
1639             {
1640                 // cheat and don't bother sending if we know it has already been
1641                 // used on the other end
1642                 if (link.target->router.lastObservedSeq[bm.origin] < bm.seq)
1643                 {
1644                     auto outbound = bm;
1645                     if (auto const it =
1646                             bm.recipientMessages.find(link.target->id);
1647                         it != bm.recipientMessages.end())
1648                     {
1649                         outbound.mesg = it->second;
1650                     }
1651                     issue(Relay<M>{link.target->id, outbound.mesg});
1652                     net.send(
1653                         this,
1654                         link.target,
1655                         [to = link.target, outbound, id = this->id] {
1656                             to->receive(outbound, id);
1657                         });
1658                 }
1659             }
1660         }
1661     }
```

The entropy CSF split-brain sim uses the smallest useful case: `n=5`, one
equivocator, two honest validators per side. A strict-majority or naive
`ceil(0.6n)` gate would admit `3/5` on both sides. The real threshold is `4/5`,
so both sides take `consensus_fallback` instead of minting split non-fallback
entropy.

📍 [`src/test/consensus/ConsensusRng_test.cpp:862-939`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusRng_test.cpp#L862-L939)
```cpp
 862 void
 863     testRngEntropyRejectsEquivocatedSplitMajorities()
 864     {
 865         using namespace csf;
 866         using namespace std::chrono;
 867
 868         testcase("RNG entropy rejects equivocated split majorities");
 869
 870         // Five active validators. Peer 2 equivocates: left peers see it
 871         // advertise H-left, right peers see it advertise H-right. Each side
 872         // locally observes 2 honest peers + the equivocator = 3/5. A strict
 873         // majority or naive ceil(0.6*n) threshold would admit both sides; the
 874         // real participant threshold is 4/5, so both sides must fall back.
 875         ConsensusParms const parms{};
 876         Sim sim;
 877
 878         PeerGroup peers = sim.createGroup(5);
 879         PeerGroup left{std::vector<Peer*>{peers[0], peers[1]}};
 880         Peer* equivocator = peers[2];
 881         PeerGroup right{std::vector<Peer*>{peers[3], peers[4]}};
 882         PeerGroup honest = left + right;
 883
 884         for (Peer* peer : peers)
 885             peer->ce().enableRngConsensus_ = true;
 886
 887         auto const fast = round<milliseconds>(0.2 * parms.ledgerGRANULARITY);
 888         peers.trustAndConnect(peers, fast);
 889
 890         sim.run(1);
 891         BEAST_EXPECT(sim.synchronized(peers));
 892
 893         left.disconnect(right);
 894
 895         auto const leftHash = sha512Half(std::string("entropy-equiv-left"));
 896         auto const rightHash = sha512Half(std::string("entropy-equiv-right"));
 897         for (Peer* peer : left)
 898             peer->ce().forcedEntropySetHash_ = leftHash;
 899         for (Peer* peer : right)
 900             peer->ce().forcedEntropySetHash_ = rightHash;
 901
 902         for (Peer* peer : left)
 903             equivocator->ce().equivocateSidecarsTo_[peer->id].entropySetHash =
 904                 leftHash;
 905         for (Peer* peer : right)
 906             equivocator->ce().equivocateSidecarsTo_[peer->id].entropySetHash =
 907                 rightHash;
 908
 909         sim.sidecarStore.publish(
 910             leftHash,
 911             SidecarStore::Type::reveal,
 912             SidecarStore::EntrySet{
 913                 {peers[0]->id, sha512Half(std::string("left-0"))},
 914                 {peers[1]->id, sha512Half(std::string("left-1"))},
 915                 {equivocator->id, sha512Half(std::string("left-equiv"))}});
 916         sim.sidecarStore.publish(
 917             rightHash,
 918             SidecarStore::Type::reveal,
 919             SidecarStore::EntrySet{
 920                 {equivocator->id, sha512Half(std::string("right-equiv"))},
 921                 {peers[3]->id, sha512Half(std::string("right-3"))},
 922                 {peers[4]->id, sha512Half(std::string("right-4"))}});
 923
 924         sim.run(3);
 925
 926         for (Peer const* peer : honest)
 927         {
 928             BEAST_EXPECT(peer->ce().lastEntropyWasFallback_);
 929             BEAST_EXPECT(peer->ce().lastEntropyTier_ == 1);
 930             BEAST_EXPECT(peer->ce().lastEntropyCount_ == 0);
 931             BEAST_EXPECT(peer->ce().lastEntropyDigest_ != uint256{});
 932         }
 933         BEAST_EXPECT(
 934             peers[0]->ce().lastEntropyDigest_ ==
 935             peers[1]->ce().lastEntropyDigest_);
 936         BEAST_EXPECT(
 937             peers[3]->ce().lastEntropyDigest_ ==
 938             peers[4]->ce().lastEntropyDigest_);
 939     }
```

The export CSF split-brain sim uses the same topology, but asserts the export
failure model: `3/5` equivocated sidecar alignment is below export quorum, so
the export retries instead of synthesizing or accepting a local sidecar result.

📍 [`src/test/consensus/ConsensusRng_test.cpp:1351-1412`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusRng_test.cpp#L1351-L1412)
```cpp
1351 void
1352     testExportSigSetRejectsEquivocatedSplitMajorities()
1353     {
1354         using namespace csf;
1355         using namespace std::chrono;
1356
1357         testcase("Export sig set rejects equivocated split majorities");
1358
1359         // Same shape as the entropy equivocation test: 2 honest validators on
1360         // each side, one equivocator advertising a matching sidecar hash to
1361         // each side. Each side sees 3/5 aligned, which must remain below the
1362         // export quorum threshold of 4/5.
1363         ConsensusParms const parms{};
1364         Sim sim;
1365
1366         PeerGroup peers = sim.createGroup(5);
1367         PeerGroup left{std::vector<Peer*>{peers[0], peers[1]}};
1368         Peer* equivocator = peers[2];
1369         PeerGroup right{std::vector<Peer*>{peers[3], peers[4]}};
1370         PeerGroup honest = left + right;
1371
1372         for (Peer* peer : peers)
1373             peer->ce().enableExportConsensus_ = true;
1374
1375         auto const fast = round<milliseconds>(0.2 * parms.ledgerGRANULARITY);
1376         peers.trustAndConnect(peers, fast);
1377
1378         sim.run(1);
1379         BEAST_EXPECT(sim.synchronized(peers));
1380
1381         left.disconnect(right);
1382
1383         auto const leftHash = sha512Half(std::string("export-equiv-left"));
1384         auto const rightHash = sha512Half(std::string("export-equiv-right"));
1385         for (Peer* peer : left)
1386         {
1387             peer->ce().forcedExportSigSetHash_ = leftHash;
1388             for (Peer const* blocked : right)
1389                 peer->ce().dropExportSigFrom_.insert(blocked->id);
1390         }
1391         for (Peer* peer : right)
1392         {
1393             peer->ce().forcedExportSigSetHash_ = rightHash;
1394             for (Peer const* blocked : left)
1395                 peer->ce().dropExportSigFrom_.insert(blocked->id);
1396         }
1397
1398         for (Peer* peer : left)
1399             equivocator->ce().equivocateSidecarsTo_[peer->id].exportSigSetHash =
1400                 leftHash;
1401         for (Peer* peer : right)
1402             equivocator->ce().equivocateSidecarsTo_[peer->id].exportSigSetHash =
1403                 rightHash;
1404
1405         sim.run(3);
1406
1407         for (Peer const* peer : honest)
1408         {
1409             BEAST_EXPECT(!peer->ce().lastExportSucceeded_);
1410             BEAST_EXPECT(peer->ce().lastExportRetried_);
1411         }
1412     }
```

The export no-veto test checks the sidecar gate specifically: quorum-aligned
export sidecar hashes can proceed even when a tx-converged minority has not
advertised any export sidecar hash.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:2928-2954`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L2928-L2954)
```cpp
2928 void
2929     testExportSigGateAllowsQuorumDespiteMissingObservation()
2930     {
2931         testcase(
2932             "Export sig gate allows quorum despite missing sidecar "
2933             "observation");
2934
2935         FakeExtensions ext;
2936         ExportTickHarness harness;
2937         auto const localHash = ext.exportHash;
2938
2939         harness.addPeer(1, localHash);
2940         harness.addPeer(2, localHash);
2941         harness.addPeer(3, localHash);
2942         harness.addPeer(4, std::nullopt);
2943
2944         auto result = harness.tick(ext);
2945         BEAST_EXPECT(!result.readyForAccept);
2946         BEAST_EXPECT(harness.position.exportSigSetHash == localHash);
2947         BEAST_EXPECT(ext.exportSigGateStarted_);
2948
2949         // A quorum-aligned signed exportSigSetHash is enough even if a
2950         // tx-converged minority peer has not advertised any exportSigSetHash.
2951         result = harness.tick(ext, std::chrono::milliseconds{100});
2952         BEAST_EXPECT(result.readyForAccept);
2953         BEAST_EXPECT(!ext.exportSigConvergenceFailed_);
2954     }
```

Fetched export sidecar validation is pinned separately: inactive signers,
signatures for transactions outside the candidate set, and invalid signatures
must not become agreed quorum material after sidecar fetch/merge.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:1821-1920`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L1821-L1920)
```cpp
1821 void
1822     testExportSidecarRejectsInvalidFetchedEntries()
1823     {
1824         testcase("Export sidecar rejects invalid fetched entries");
1825
1826         using namespace jtx;
1827         Env env{
1828             *this, envconfig(validator, ""), supported_amendments(), nullptr};
1829         auto const ledger = env.app().getLedgerMaster().getClosedLedger();
1830         auto const& valKeys = env.app().getValidatorKeys();
1831         BEAST_EXPECT(valKeys.keys);
1832         if (!valKeys.keys)
1833             return;
1834
1835         auto const& valPK = valKeys.keys->publicKey;
1836         auto const& valSK = valKeys.keys->secretKey;
1837         auto const signerAccount = calcAccountID(valPK);
1838         auto const dst = calcAccountID(randomKeyPair(KeyType::secp256k1).first);
1839         auto const innerObj = makeExportedPayment(signerAccount, dst);
1840         auto const innerTx = makeSTTx(innerObj);
1841         auto const exportTx = makeExportTx(innerObj, signerAccount);
1842         auto const txHash = exportTx->getTransactionID();
1843         auto const txSet = makeRCLTxSet(env.app(), {exportTx});
1844
1845         auto const sigData = buildMultiSigningData(innerTx, signerAccount);
1846         auto const sig = sign(valPK, valSK, sigData.slice());
1847         Buffer const validSig(sig.data(), sig.size());
1848         std::uint8_t const invalidBytes[] = {0x30, 0x03, 0x01, 0x02, 0x03};
1849         Buffer const invalidSig{invalidBytes, sizeof(invalidBytes)};
1850
1851         auto expectRejected = [&](ConsensusExtensions& ce,
1852                                   std::shared_ptr<SHAMap> const& map,
1853                                   uint256 const& expectedTxHash,
1854                                   PublicKey const& expectedSigner) {
1855             publishAndFetchSidecarSet(
1856                 env.app(),
1857                 ce,
1858                 map,
1859                 ConsensusExtensions::SidecarKind::exportSig);
1860             BEAST_EXPECT(!ce.exportSigCollector().hasVerifiedSignature(
1861                 expectedTxHash, expectedSigner));
1862             BEAST_EXPECT(
1863                 ce.exportSigCollector().signatureCount(expectedTxHash) == 0);
1864         };
1865
1866         {
1867             auto const [inactivePK, _] = randomKeyPair(KeyType::secp256k1);
1868             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1869             ce.setExportEnabledThisRound(true);
1870             ce.cacheUNLReport(ledger);
1871             ce.cacheConsensusTxSet(txSet);
1872
1873             expectRejected(
1874                 ce,
1875                 makeSidecarSet(
1876                     env.app(),
1877                     {makeExportSigSidecar(
1878                         txHash,
1879                         inactivePK,
1880                         Slice(validSig.data(), validSig.size()))}),
1881                 txHash,
1882                 inactivePK);
1883         }
1884
1885         {
1886             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1887             ce.setExportEnabledThisRound(true);
1888             ce.cacheUNLReport(ledger);
1889             ce.cacheConsensusTxSet(makeRCLTxSet(env.app(), {}));
1890
1891             expectRejected(
1892                 ce,
1893                 makeSidecarSet(
1894                     env.app(),
1895                     {makeExportSigSidecar(
1896                         txHash,
1897                         valPK,
1898                         Slice(validSig.data(), validSig.size()))}),
1899                 txHash,
1900                 valPK);
1901         }
1902
1903         {
1904             ConsensusExtensions ce{env.app(), activeNoopJournal()};
1905             ce.setExportEnabledThisRound(true);
1906             ce.cacheUNLReport(ledger);
1907             ce.cacheConsensusTxSet(txSet);
1908
1909             expectRejected(
1910                 ce,
1911                 makeSidecarSet(
1912                     env.app(),
1913                     {makeExportSigSidecar(
1914                         txHash,
1915                         valPK,
1916                         Slice(invalidSig.data(), invalidSig.size()))}),
1917                 txHash,
1918                 valPK);
1919         }
1920     }
```

Export agreed-signature selection is pinned by mutating the live collector after
the sidecar hash converges. The agreed snapshot must still return the original
signature bytes.

📍 [`src/test/consensus/ConsensusExtensions_test.cpp:1922-1985`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/consensus/ConsensusExtensions_test.cpp#L1922-L1985)
```cpp
1922 void
1923     testExportAgreedSignaturesIgnoreLiveCollectorMutation()
1924     {
1925         testcase("Export apply uses agreed sidecar signatures");
1926
1927         using namespace jtx;
1928         Env env{
1929             *this, envconfig(validator, ""), supported_amendments(), nullptr};
1930         auto const ledger = env.app().getLedgerMaster().getClosedLedger();
1931         auto const& valKeys = env.app().getValidatorKeys();
1932         BEAST_EXPECT(valKeys.keys);
1933         if (!valKeys.keys)
1934             return;
1935
1936         auto const& valPK = valKeys.keys->publicKey;
1937         auto const& valSK = valKeys.keys->secretKey;
1938         auto const signerAccount = calcAccountID(valPK);
1939         auto const dst = calcAccountID(randomKeyPair(KeyType::secp256k1).first);
1940         auto const innerObj = makeExportedPayment(signerAccount, dst);
1941         auto const innerTx = makeSTTx(innerObj);
1942         auto const exportTx = makeExportTx(innerObj, signerAccount);
1943         auto const txHash = exportTx->getTransactionID();
1944         auto const txSet = makeRCLTxSet(env.app(), {exportTx});
1945         auto const seq = ledger->seq() + 1;
1946
1947         ConsensusExtensions ce{env.app(), activeNoopJournal()};
1948         ce.setExportEnabledThisRound(true);
1949         ce.cacheUNLReport(ledger);
1950         ce.cacheConsensusTxSet(txSet);
1951
1952         auto const sigData = buildMultiSigningData(innerTx, signerAccount);
1953         auto const sig = sign(valPK, valSK, sigData.slice());
1954         Buffer const originalSig(sig.data(), sig.size());
1955         ce.exportSigCollector().addVerifiedSignature(
1956             txHash, valPK, originalSig, seq);
1957         auto const exportSigSetHash = ce.buildExportSigSet(seq);
1958         BEAST_EXPECT(ce.isSidecarSet(exportSigSetHash));
1959
1960         // Simulate a late local collector mutation after the sidecar hash has
1961         // converged. The live collector now differs from the agreed sidecar
1962         // map.
1963         std::uint8_t const lateBytes[] = {9, 8, 7};
1964         Buffer const lateSig{lateBytes, sizeof(lateBytes)};
1965         ce.exportSigCollector().addVerifiedSignature(
1966             txHash, valPK, lateSig, seq);
1967
1968         auto const view = ce.activeValidatorView();
1969         auto const live = ce.exportSigCollector().checkQuorumAndSnapshot(
1970             txHash, 1, [&](PublicKey const& pk) {
1971                 return ce.isActiveValidator(pk, *view);
1972             });
1973         BEAST_EXPECT(live);
1974         if (live)
1975             BEAST_EXPECT(live->at(valPK) == lateSig);
1976
1977         auto const agreed =
1978             ce.agreedExportSignatures(*exportTx, txHash, *view, 1);
1979         BEAST_EXPECT(agreed);
1980         if (agreed)
1981         {
1982             BEAST_EXPECT(agreed->size() == 1);
1983             BEAST_EXPECT(agreed->at(valPK) == originalSig);
1984         }
1985     }
```

The app-level `Export::doApply` regression drives closed-ledger apply with the
same late live-collector mutation and checks the resulting shadow-ticket hash.
That pins the final ledger effect, not only the helper that reads agreed
signatures.

📍 [`src/test/app/Export_test.cpp:761-845`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/app/Export_test.cpp#L761-L845)
```cpp
 761 void
 762     testExportNetworkApplyUsesAgreedSidecar(FeatureBitset features)
 763     {
 764         testcase("ttEXPORT network apply uses agreed export sidecar");
 765
 766         using namespace jtx;
 767
 768         Env env{*this, exportTestConfig(), features};
 769
 770         Account const alice{"alice"};
 771         Account const carol{"carol"};
 772
 773         env.fund(XRP(10000), alice, carol);
 774         env.close();
 775
 776         auto const& valKeys = env.app().getValidatorKeys();
 777         BEAST_EXPECT(valKeys.keys);
 778         if (!valKeys.keys)
 779             return;
 780
 781         auto const& valPK = valKeys.keys->publicKey;
 782         auto const& valSK = valKeys.keys->secretKey;
 783         seedUNLReportLedger(env, {valPK});
 784         forceNonStandalone(env.app());
 785         BEAST_EXPECT(!env.app().config().standalone());
 786
 787         auto const seq = env.current()->seq();
 788         auto const ticketSeq = std::uint32_t{1};
 789         auto const lls = seq + 5;
 790         auto innerObj = buildExportedPayment(
 791             alice.id(), carol.id(), seq + 1, lls, ticketSeq);
 792         auto const innerTx = makeSTTx(innerObj);
 793         auto jt = makeExportJTx(env, alice, innerObj, lls);
 794         auto const exportTx = jt.stx;
 795         BEAST_EXPECT(exportTx);
 796         if (!exportTx)
 797             return;
 798         auto const txHash = exportTx->getTransactionID();
 799
 800         auto& ce = env.app().getConsensusExtensions();
 801         ce.setExportEnabledThisRound(true);
 802         ce.cacheUNLReport(env.app().getLedgerMaster().getClosedLedger());
 803         auto const view = ce.activeValidatorView();
 804         BEAST_EXPECT(view->fromUNLReport);
 805         ce.cacheConsensusTxSet(makeRCLTxSet(env.app(), {exportTx}));
 806
 807         auto const originalSig =
 808             ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
 809         auto const applySeq = env.closed()->seq() + 1;
 810         ce.exportSigCollector().addVerifiedSignature(
 811             txHash, valPK, originalSig, applySeq);
 812         auto const agreedHash = ce.buildExportSigSet(applySeq);
 813         BEAST_EXPECT(ce.isSidecarSet(agreedHash));
 814
 815         // Simulate an asynchronous collector mutation after sidecar agreement.
 816         // A bad revert to the live collector at apply would assemble this late
 817         // signature and write a different shadow-ticket hash.
 818         std::uint8_t const lateBytes[] = {9, 8, 7};
 819         Buffer const lateSig{lateBytes, sizeof(lateBytes)};
 820         ce.exportSigCollector().addVerifiedSignature(
 821             txHash, valPK, lateSig, applySeq);
 822
 823         ExportResultBuilder::SignatureSnapshot expectedSigs;
 824         expectedSigs.emplace(valPK, originalSig);
 825         auto const expectedSignedTxHash =
 826             ExportResultBuilder::assemble(
 827                 innerTx, expectedSigs, applySeq, txHash)
 828                 .signedTxHash;
 829
 830         auto const parent = env.app().getLedgerMaster().getClosedLedger();
 831         auto next = std::make_shared<Ledger>(
 832             *parent, env.app().timeKeeper().closeTime());
 833         OpenView accum(&*next);
 834         auto const result =
 835             ripple::apply(env.app(), accum, *exportTx, tapNONE, env.journal);
 836         BEAST_EXPECT(result.ter == tesSUCCESS);
 837         BEAST_EXPECT(result.applied);
 838         accum.apply(*next);
 839
 840         auto const st = next->read(keylet::shadowTicket(alice.id(), ticketSeq));
 841         BEAST_EXPECT(st);
 842         if (st)
 843             BEAST_EXPECT(
 844                 st->getFieldH256(sfTransactionHash) == expectedSignedTxHash);
 845     }
```

The companion `Export::doApply` test covers network mode without a UNLReport:
even with verified signatures and a built sidecar, export retries rather than
assembling from a node-local trusted-config view.

📍 [`src/test/app/Export_test.cpp:847-909`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/app/Export_test.cpp#L847-L909)
```cpp
 847 void
 848     testExportNetworkRetryWithoutUNLReport(FeatureBitset features)
 849     {
 850         testcase("ttEXPORT network mode retries without UNLReport view");
 851
 852         using namespace jtx;
 853
 854         Env env{*this, exportTestConfig(), features};
 855
 856         Account const alice{"alice"};
 857         Account const carol{"carol"};
 858
 859         env.fund(XRP(10000), alice, carol);
 860         env.close();
 861         forceNonStandalone(env.app());
 862         BEAST_EXPECT(!env.app().config().standalone());
 863
 864         auto const& valKeys = env.app().getValidatorKeys();
 865         BEAST_EXPECT(valKeys.keys);
 866         if (!valKeys.keys)
 867             return;
 868
 869         auto const& valPK = valKeys.keys->publicKey;
 870         auto const& valSK = valKeys.keys->secretKey;
 871         auto const seq = env.current()->seq();
 872         auto const ticketSeq = std::uint32_t{1};
 873         auto const lls = seq + 5;
 874         auto innerObj = buildExportedPayment(
 875             alice.id(), carol.id(), seq + 1, lls, ticketSeq);
 876         auto const innerTx = makeSTTx(innerObj);
 877         auto jt = makeExportJTx(env, alice, innerObj, lls);
 878         auto const exportTx = jt.stx;
 879         BEAST_EXPECT(exportTx);
 880         if (!exportTx)
 881             return;
 882         auto const txHash = exportTx->getTransactionID();
 883
 884         auto& ce = env.app().getConsensusExtensions();
 885         ce.setExportEnabledThisRound(true);
 886         ce.cacheUNLReport(env.app().getLedgerMaster().getClosedLedger());
 887         auto const view = ce.activeValidatorView();
 888         BEAST_EXPECT(!view->fromUNLReport);
 889         ce.cacheConsensusTxSet(makeRCLTxSet(env.app(), {exportTx}));
 890
 891         auto const sig =
 892             ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
 893         auto const applySeq = env.closed()->seq() + 1;
 894         ce.exportSigCollector().addVerifiedSignature(
 895             txHash, valPK, sig, applySeq);
 896         auto const agreedHash = ce.buildExportSigSet(applySeq);
 897         BEAST_EXPECT(ce.isSidecarSet(agreedHash));
 898
 899         auto const parent = env.app().getLedgerMaster().getClosedLedger();
 900         auto next = std::make_shared<Ledger>(
 901             *parent, env.app().timeKeeper().closeTime());
 902         OpenView accum(&*next);
 903         auto const result =
 904             ripple::apply(env.app(), accum, *exportTx, tapNONE, env.journal);
 905
 906         BEAST_EXPECT(result.ter == terRETRY_EXPORT);
 907         BEAST_EXPECT(!result.applied);
 908         BEAST_EXPECT(!next->read(keylet::shadowTicket(alice.id(), ticketSeq)));
 909     }
```

The LLS boundary test checks the exact closed-ledger edge documented in
`Export::doApply`: quorum material at `ledger == LastLedgerSequence` still
succeeds, while missing quorum at that same ledger returns
`tecEXPORT_EXPIRED`.

📍 [`src/test/app/Export_test.cpp:911-1001`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/src/test/app/Export_test.cpp#L911-L1001)
```cpp
 911 void
 912     testExportNetworkLastLedgerSequenceBoundary(FeatureBitset features)
 913     {
 914         testcase("ttEXPORT network mode LastLedgerSequence boundary");
 915
 916         using namespace jtx;
 917
 918         auto applyAtLastLedger = [&](bool withQuorum) {
 919             Env env{*this, exportTestConfig(), features};
 920
 921             Account const alice{"alice"};
 922             Account const carol{"carol"};
 923
 924             env.fund(XRP(10000), alice, carol);
 925             env.close();
 926
 927             auto const& valKeys = env.app().getValidatorKeys();
 928             BEAST_EXPECT(valKeys.keys);
 929             if (!valKeys.keys)
 930                 return;
 931
 932             auto const& valPK = valKeys.keys->publicKey;
 933             auto const& valSK = valKeys.keys->secretKey;
 934             seedUNLReportLedger(env, {valPK});
 935             forceNonStandalone(env.app());
 936             BEAST_EXPECT(!env.app().config().standalone());
 937
 938             auto const parent = env.app().getLedgerMaster().getClosedLedger();
 939             auto const applySeq = parent->seq() + 1;
 940             auto const ticketSeq =
 941                 withQuorum ? std::uint32_t{1} : std::uint32_t{2};
 942             auto innerObj = buildExportedPayment(
 943                 alice.id(), carol.id(), applySeq, applySeq, ticketSeq);
 944             auto const innerTx = makeSTTx(innerObj);
 945             auto jt = makeExportJTx(env, alice, innerObj, applySeq);
 946             auto const exportTx = jt.stx;
 947             BEAST_EXPECT(exportTx);
 948             if (!exportTx)
 949                 return;
 950             auto const txHash = exportTx->getTransactionID();
 951
 952             auto& ce = env.app().getConsensusExtensions();
 953             ce.setExportEnabledThisRound(true);
 954             ce.cacheUNLReport(parent);
 955             auto const view = ce.activeValidatorView();
 956             BEAST_EXPECT(view->fromUNLReport);
 957             ce.cacheConsensusTxSet(makeRCLTxSet(env.app(), {exportTx}));
 958
 959             if (withQuorum)
 960             {
 961                 auto const sig =
 962                     ExportResultBuilder::signExportedTxn(innerTx, valPK, valSK);
 963                 ce.exportSigCollector().addVerifiedSignature(
 964                     txHash, valPK, sig, applySeq);
 965                 auto const agreedHash = ce.buildExportSigSet(applySeq);
 966                 BEAST_EXPECT(ce.isSidecarSet(agreedHash));
 967             }
 968
 969             auto next = std::make_shared<Ledger>(
 970                 *parent, env.app().timeKeeper().closeTime());
 971             BEAST_EXPECT(next->seq() == applySeq);
 972             OpenView accum(&*next);
 973             auto const result = ripple::apply(
 974                 env.app(), accum, *exportTx, tapNONE, env.journal);
 975
 976             if (withQuorum)
 977             {
 978                 BEAST_EXPECT(result.ter == tesSUCCESS);
 979                 BEAST_EXPECT(result.applied);
 980                 accum.apply(*next);
 981                 BEAST_EXPECT(
 982                     next->read(keylet::shadowTicket(alice.id(), ticketSeq)));
 983             }
 984             else
 985             {
 986                 BEAST_EXPECT(result.ter == tecEXPORT_EXPIRED);
 987                 // tecEXPORT_EXPIRED is a terminal tec result: it applies to
 988                 // consume the sequence/fee, but must not create export state.
 989                 BEAST_EXPECT(result.applied);
 990                 accum.apply(*next);
 991                 BEAST_EXPECT(
 992                     !next->read(keylet::shadowTicket(alice.id(), ticketSeq)));
 993             }
 994         };
 995
 996         // Quorum at ledger == LastLedgerSequence still succeeds; no quorum at
 997         // the same boundary expires cleanly instead of falling through to a
 998         // later tefMAX_LEDGER preclaim.
 999         applyAtLastLedger(true);
1000         applyAtLastLedger(false);
1001     }
```

The live testnet Tier 2 smoke covers the operational behavior the unit tests
cannot show by themselves: in a 6-node network, `4/6` is below validation quorum
but inside the participant-aligned band, so the test inspects closed provisional
ledgers directly and requires `EntropyTier=2` with `EntropyCount=4`.

📍 [`.testnet/scenarios/entropy/participant_aligned_smoke.py:96-144`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/entropy/participant_aligned_smoke.py#L96-L144)
```python
  96     # --- 4/6: participant_aligned (Tier 2) degraded window ---
  97     ctx.stop_node(4)
  98     await ctx.wait_for_nodes_down(nodes=[4], timeout=30)
  99
 100     # ~12s window: confirm tier-2 INJECTION from the cohort's logs, and that the
 101     # round is NOT the impossible/fallback path (which is what distinguishes the
 102     # tier-2 band from the tier-1 fallback regime).
 103     op = await ctx.sleep(12, name="tier2_window")
 104     selected_t2 = ctx.search_logs(
 105         r"RNG: entropy selected seq=\d+ tier=2 count=4",
 106         within=op.window,
 107         nodes=[0, 1, 2, 3],
 108     )
 109     log(f"4/6: 'entropy selected tier=2 count=4' logs: {selected_t2.count}")
 110     if selected_t2.count == 0:
 111         raise AssertionError(
 112             "4/6 window injected no participant_aligned (tier 2) entropy: no "
 113             "'RNG: entropy selected ... tier=2 count=4' on the surviving cohort"
 114         )
 115     ctx.assert_not_log(
 116         r"reason=impossible-entropy-gate", within=op.window, nodes=[0, 1, 2, 3]
 117     )
 118
 119     # Verify the on-ledger EntropyTier=2 DIRECTLY: validation is stalled (4 < 5),
 120     # so sample the surviving cohort's CLOSED ledger (its LCL — built but not yet
 121     # validated). At least one must be participant_aligned with EntropyCount=4.
 122     tier2_on_ledger = 0
 123     last_seq = None
 124     for _ in range(5):
 125         seq, ce = _closed_entropy(
 126             ctx.ledger("closed", transactions=True, node_id=0)
 127         )
 128         if ce is not None and seq is not None and seq != last_seq:
 129             last_seq = seq
 130             tier = ce.get("EntropyTier")
 131             count = ce.get("EntropyCount", -1)
 132             log(f"  closed ledger {seq}: tier={tier} count={count}")
 133             if tier == 2:
 134                 assert_participant_aligned(ce, seq, expected_count=4)
 135                 tier2_on_ledger += 1
 136         await ctx.sleep(3)
 137
 138     if tier2_on_ledger == 0:
 139         raise AssertionError(
 140             "no closed participant_aligned (tier 2) ledger observed during the "
 141             "4/6 window (tier 2 was injected per logs, but not seen on a closed "
 142             "ledger)"
 143         )
 144     log(f"4/6: {tier2_on_ledger} participant_aligned closed ledger(s) verified")
```

The no-UNLReport testnet scenario covers the fail-closed rule outside the unit
harness: a healthy non-standalone network without a seeded UNLReport must keep
minting `consensus_fallback`, even after the commit/reveal pipeline has warmed
up.

📍 [`.testnet/scenarios/entropy/fallback_without_unl_report.py:8-28`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/entropy/fallback_without_unl_report.py#L8-L28)
```python
   8 async def scenario(ctx, log):
   9     await require_entropy(ctx, log)
  10
  11     # Non-standalone nodes require a ledger-anchored UNLReport before assigning
  12     # validator_quorum / participant_aligned labels. Without it, the RNG pipeline
  13     # may still collect commits/reveals, but injection must remain Tier 1.
  14     await ctx.wait_for_ledgers(3, node_id=0, timeout=60)
  15     log("Pipeline warmed up without UNLReport")
  16
  17     start_seq = ctx.validated_ledger_index(0)
  18     await ctx.wait_for_ledgers(5, node_id=0, timeout=90)
  19     end_seq = ctx.validated_ledger_index(0)
  20     log(f"Inspecting ledgers {start_seq + 1} -> {end_seq}")
  21
  22     for seq in range(start_seq + 1, end_seq + 1):
  23         ce, _ = get_entropy_tx(ctx, seq)
  24         digest, count = assert_consensus_fallback(ce, seq)
  25         log(f"  Ledger {seq}: EntropyCount={count} Digest={digest[:16]}...")
  26
  27     log(f"Verified {end_seq - start_seq} ledgers: all consensus_fallback")
  28     log("PASS")
```

The export testnet quorum scenario covers the network-level success/failure
shape: enough active validators produce `tesSUCCESS` plus an `ExportResult`;
below active-view quorum, the export must not succeed and no shadow ticket
should be present.

📍 [`.testnet/scenarios/export/export_quorum.py:24-117`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/export/export_quorum.py#L24-L117)
```python
  24 async def scenario(ctx, log, expect_success=True):
  25     await require_export(ctx, log)
  26
  27     # --- Setup ---
  28     await ctx.fund_accounts({"alice": 10000, "bob": 1000})
  29     log("Accounts funded")
  30
  31     alice = ctx.account("alice")
  32     bob = ctx.account("bob")
  33     current_seq = ctx.validated_ledger_index(0)
  34
  35     log(f"Current ledger: {current_seq}")
  36     outcome = "success" if expect_success else "failure (below quorum)"
  37     log(f"Expecting export {outcome}")
  38
  39     # --- Submit ttEXPORT ---
  40     result = await ctx.submit_and_wait(
  41         {
  42             "TransactionType": "Export",
  43             "LastLedgerSequence": current_seq + 10,
  44             "Fee": "1000000",
  45             "ExportedTxn": {
  46                 "TransactionType": "Payment",
  47                 "Account": alice.address,
  48                 "Destination": bob.address,
  49                 "Amount": "1000000",
  50                 "Fee": "10",
  51                 "Sequence": 0,
  52                 "TicketSequence": 1,
  53                 "FirstLedgerSequence": current_seq + 1,
  54                 "LastLedgerSequence": current_seq + 8,
  55                 "Flags": 2147483648,
  56                 "SigningPubKey": "",
  57             },
  58         },
  59         alice.wallet,
  60         timeout=60,
  61     )
  62
  63     final_seq = ctx.validated_ledger_index(0)
  64     engine_result = result.get("engine_result", "")
  65     meta = result.get("meta", {})
  66
  67     log(f"Export at ledger {final_seq}, result: {engine_result}")
  68
  69     if expect_success:
  70         if engine_result != "tesSUCCESS":
  71             raise AssertionError(
  72                 f"Expected tesSUCCESS, got {engine_result}"
  73             )
  74
  75         # Assert ExportResult is well-formed with signers
  76         assert_export_result(meta, log, require_signers=True)
  77
  78         # Assert shadow ticket was created
  79         assert_shadow_ticket(ctx, alice.address, log, expect_exists=True)
  80
  81         log("Export succeeded as expected (active-view quorum reached)")
  82     else:
  83         if engine_result == "tesSUCCESS":
  84             raise AssertionError(
  85                 "Export should NOT have succeeded below active-view quorum"
  86             )
  87         if engine_result != "tecEXPORT_EXPIRED":
  88             raise AssertionError(
  89                 "Expected tecEXPORT_EXPIRED below active-view quorum, "
  90                 f"got {engine_result}"
  91             )
  92         log(f"Export failed as expected ({engine_result})")
  93
  94         # No shadow ticket should exist
  95         assert_shadow_ticket(ctx, alice.address, log, expect_exists=False)
  96
  97     # --- Verify subsequent payment works ---
  98     log("Submitting payment from alice to bob...")
  99     pay_result = await ctx.submit_and_wait(
 100         {
 101             "TransactionType": "Payment",
 102             "Destination": bob.address,
 103             "Amount": "1000000",
 104             "Fee": "12",
 105         },
 106         alice.wallet,
 107         timeout=30,
 108     )
 109
 110     pay_engine = pay_result.get("engine_result", "")
 111     log(f"Payment result: {pay_engine}")
 112
 113     if pay_engine != "tesSUCCESS":
 114         raise AssertionError(f"Payment failed: {pay_engine}")
 115
 116     log("Payment succeeded -- account not blocked")
 117     log("PASS")
```

The export degradation scenario covers the no-fallback failure mode with logs:
with only `3/5` validators signing, export repeatedly returns
`terRETRY_EXPORT`, expires at `LastLedgerSequence`, and creates no shadow
ticket.

📍 [`.testnet/scenarios/export/export_degradation.py:36-97`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/export/export_degradation.py#L36-L97)
```python
  36     # --- Submit ttEXPORT (should retry then expire -- only 3/5 sigs) ---
  37     export_start = ctx.mark("export-degradation-submit-start")
  38     result = await ctx.submit_and_wait(
  39         {
  40             "TransactionType": "Export",
  41             "LastLedgerSequence": current_seq + 8,
  42             "Fee": "1000000",
  43             "ExportedTxn": {
  44                 "TransactionType": "Payment",
  45                 "Account": alice.address,
  46                 "Destination": bob.address,
  47                 "Amount": "1000000",
  48                 "Fee": "10",
  49                 "Sequence": 0,
  50                 "TicketSequence": 1,
  51                 "FirstLedgerSequence": current_seq + 1,
  52                 "LastLedgerSequence": current_seq + 6,
  53                 "Flags": 2147483648,
  54                 "SigningPubKey": "",
  55             },
  56         },
  57         alice.wallet,
  58         timeout=60,
  59     )
  60     export_end = ctx.mark("export-degradation-submit-end")
  61
  62     final_seq = ctx.validated_ledger_index(0)
  63     engine_result = result.get("engine_result", "")
  64     log(f"Export completed at ledger {final_seq}, result: {engine_result}")
  65
  66     # With only 3/5 sigs and 80% quorum (4 required), export MUST fail
  67     if engine_result == "tesSUCCESS":
  68         raise AssertionError(
  69             "Export should NOT have succeeded with only 3/5 sigs "
  70             "(need 4 for 80% quorum) -- check runtime_config no_export_sig"
  71         )
  72
  73     # Should be tecEXPORT_EXPIRED (LLS reached without quorum). Be exact here:
  74     # any other non-success means the retry/expiry boundary regressed.
  75     if engine_result != "tecEXPORT_EXPIRED":
  76         raise AssertionError(
  77             f"Expected tecEXPORT_EXPIRED below quorum, got {engine_result}"
  78         )
  79
  80     log(f"Export failed as expected ({engine_result})")
  81
  82     retry_logs = ctx.assert_log(
  83         r"Export: insufficient signatures .*result=terRETRY_EXPORT",
  84         since=export_start,
  85         until=export_end,
  86     )
  87     log(f"Export insufficient-signature retries: {retry_logs.count}")
  88
  89     expired_logs = ctx.assert_log(
  90         r"Export: last ledger expired .*result=tecEXPORT_EXPIRED",
  91         since=export_start,
  92         until=export_end,
  93     )
  94     log(f"Export LLS expiry logs: {expired_logs.count}")
  95
  96     # No shadow ticket should exist (export never reached quorum)
  97     assert_shadow_ticket(ctx, alice.address, log, expect_exists=False)
```

The export-without-UNLReport scenario covers the same fail-closed rule in a
multi-node testnet: even with normal signing available, export retries/expires
and logs that it refused to assemble from a non-ledger-anchored validator view.

📍 [`.testnet/scenarios/export/export_without_unl_report.py:14-92`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/export/export_without_unl_report.py#L14-L92)
```python
  14 async def scenario(ctx, log):
  15     await require_export(ctx, log, require_unl_report=False)
  16
  17     await ctx.fund_accounts({"alice": 10000, "bob": 1000})
  18     log("Accounts funded")
  19
  20     alice = ctx.account("alice")
  21     bob = ctx.account("bob")
  22     current_seq = ctx.validated_ledger_index(0)
  23
  24     log(f"Current ledger: {current_seq}")
  25     log("UNLReport intentionally absent; export must not use local config view")
  26
  27     export_start = ctx.mark("export-without-unlreport-submit-start")
  28     result = await ctx.submit_and_wait(
  29         {
  30             "TransactionType": "Export",
  31             "LastLedgerSequence": current_seq + 8,
  32             "Fee": "1000000",
  33             "ExportedTxn": {
  34                 "TransactionType": "Payment",
  35                 "Account": alice.address,
  36                 "Destination": bob.address,
  37                 "Amount": "1000000",
  38                 "Fee": "10",
  39                 "Sequence": 0,
  40                 "TicketSequence": 1,
  41                 "FirstLedgerSequence": current_seq + 1,
  42                 "LastLedgerSequence": current_seq + 6,
  43                 "Flags": 2147483648,
  44                 "SigningPubKey": "",
  45             },
  46         },
  47         alice.wallet,
  48         timeout=60,
  49     )
  50     export_end = ctx.mark("export-without-unlreport-submit-end")
  51
  52     final_seq = ctx.validated_ledger_index(0)
  53     engine_result = result.get("engine_result", "")
  54     log(f"Export completed at ledger {final_seq}, result: {engine_result}")
  55
  56     if engine_result == "tesSUCCESS":
  57         raise AssertionError(
  58             "Export should not succeed without a ledger-anchored UNLReport view"
  59         )
  60
  61     # Be exact: without a UNLReport view the export should retry until LLS and
  62     # expire, not fail by some unrelated terminal code.
  63     if engine_result != "tecEXPORT_EXPIRED":
  64         raise AssertionError(
  65             "Expected tecEXPORT_EXPIRED without UNLReport view, "
  66             f"got {engine_result}"
  67         )
  68
  69     warning_logs = ctx.assert_log(
  70         r"Export: retrying without ledger-anchored validator view",
  71         since=export_start,
  72         until=export_end,
  73     )
  74     log(f"Export no-UNLReport retry warnings: {warning_logs.count}")
  75
  76     retry_logs = ctx.assert_log(
  77         r"Export: insufficient signatures .*result=terRETRY_EXPORT",
  78         since=export_start,
  79         until=export_end,
  80     )
  81     log(f"Export retry logs: {retry_logs.count}")
  82
  83     expired_logs = ctx.assert_log(
  84         r"Export: last ledger expired .*result=tecEXPORT_EXPIRED",
  85         since=export_start,
  86         until=export_end,
  87     )
  88     log(f"Export expiry logs: {expired_logs.count}")
  89
  90     assert_shadow_ticket(ctx, alice.address, log, expect_exists=False)
  91
  92     log("PASS")
```

The export no-veto testnet scenario covers the live missing-observation
schedule: one active validator withholds `exportSigSetHash` while still
attaching export signatures, the other `4/5` active validators align on quorum
sidecar material, export succeeds, and the shadow ticket is created. The test
also requires the runtime logs proving both halves of the schedule:
`Export: withholding exportSigSetHash` and
`Export: missing exportSigSetHash observation ignored`.

📍 [`.testnet/scenarios/export/export_no_veto_missing_observation.py:19-87`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/export/export_no_veto_missing_observation.py#L19-L87)
```python
  19 async def scenario(ctx, log):
  20     await require_export(ctx, log)
  21
  22     await ctx.fund_accounts({"alice": 10000, "bob": 1000})
  23     log("Accounts funded")
  24
  25     alice = ctx.account("alice")
  26     bob = ctx.account("bob")
  27     current_seq = ctx.validated_ledger_index(0)
  28
  29     log(f"Current ledger: {current_seq}")
  30     log("Node 4 withholds exportSigSetHash but still attaches export signatures")
  31
  32     export_start = ctx.mark("export-no-veto-submit-start")
  33     result = await ctx.submit_and_wait(
  34         {
  35             "TransactionType": "Export",
  36             "LastLedgerSequence": current_seq + 10,
  37             "Fee": "1000000",
  38             "ExportedTxn": {
  39                 "TransactionType": "Payment",
  40                 "Account": alice.address,
  41                 "Destination": bob.address,
  42                 "Amount": "1000000",
  43                 "Fee": "10",
  44                 "Sequence": 0,
  45                 "TicketSequence": 1,
  46                 "FirstLedgerSequence": current_seq + 1,
  47                 "LastLedgerSequence": current_seq + 8,
  48                 "Flags": 2147483648,
  49                 "SigningPubKey": "",
  50             },
  51         },
  52         alice.wallet,
  53         timeout=60,
  54     )
  55     export_end = ctx.mark("export-no-veto-submit-end")
  56
  57     final_seq = ctx.validated_ledger_index(0)
  58     engine_result = result.get("engine_result", "")
  59     meta = result.get("meta", {})
  60
  61     log(f"Export completed at ledger {final_seq}, result: {engine_result}")
  62     if engine_result != "tesSUCCESS":
  63         raise AssertionError(f"Expected tesSUCCESS, got {engine_result}")
  64
  65     export_result = assert_export_result(meta, log, require_signers=True)
  66     signers = export_result.get("ExportedTxn", {}).get("Signers", [])
  67     if len(signers) < 4:
  68         raise AssertionError(f"Expected at least 4 signers, got {len(signers)}")
  69     log(f"Export signer count: {len(signers)}")
  70
  71     no_veto_logs = ctx.assert_log(
  72         r"Export: missing exportSigSetHash observation ignored",
  73         since=export_start,
  74         until=export_end,
  75     )
  76     log(f"Export no-veto missing-observation logs: {no_veto_logs.count}")
  77
  78     withhold_logs = ctx.assert_log(
  79         r"Export: withholding exportSigSetHash",
  80         since=export_start,
  81         until=export_end,
  82     )
  83     log(f"Export sidecar hash withholding logs: {withhold_logs.count}")
  84
  85     assert_shadow_ticket(ctx, alice.address, log, expect_exists=True)
  86
  87     log("PASS")
```

The latency probe is observational rather than a formal safety proof. It
preflights runtime-test support, logs the exact RuntimeConfig seen by each node,
and records retry/timeout/no-veto counters under directed delay/drop schedules.

📍 [`.testnet/scenarios/perf/ce_export_latency_probe.py:111-196`](https://github.com/Xahau/xahaud/blob/938d3055ce5a4c62ce7793b352d91b1371236077/.testnet/scenarios/perf/ce_export_latency_probe.py#L111-L196)
```python
 111 async def scenario(
 112     ctx,
 113     log,
 114     *,
 115     warmup_ledgers=3,
 116     ledgers=8,
 117     submit_export=False,
 118     export_timeout=90,
 119 ):
 120     await ctx.wait_for_ledger_close(timeout=120)
 121     await _require_runtime_config(ctx, log)
 122     _log_runtime_config(ctx, log)
 123     await _require_consensus_entropy(ctx, log)
 124
 125     if submit_export:
 126         # require_export also asserts the UNLReport precondition for successful
 127         # network-mode Export. Keep that explicit in perf runs so a missing
 128         # report does not masquerade as a latency failure.
 129         await require_export(ctx, log, require_runtime_config=False)
 130
 131     await ctx.wait_for_ledgers(warmup_ledgers, node_id=0, timeout=120)
 132     warm_seq = ctx.validated_ledger_index(0)
 133     log(f"Warmup complete at validated ledger {warm_seq}")
 134
 135     export_window = None
 136     if submit_export:
 137         export_window = await _submit_direct_export(
 138             ctx, log, timeout=export_timeout
 139         )
 140
 141     started = ctx.mark("latency-probe-start")
 142     start_seq = ctx.validated_ledger_index(0)
 143     await ctx.wait_for_ledgers(ledgers, node_id=0, timeout=max(120, ledgers * 30))
 144     ended = ctx.mark("latency-probe-end")
 145     end_seq = ctx.validated_ledger_index(0)
 146
 147     if start_seq is None or end_seq is None:
 148         raise AssertionError("validated ledger index unavailable during probe")
 149
 150     elapsed = (ended.monotonic_ns - started.monotonic_ns) / 1_000_000_000
 151     closed = max(0, end_seq - start_seq)
 152     cadence = elapsed / closed if closed else 0.0
 153     log(
 154         f"Observed validated ledgers {start_seq + 1}..{end_seq} "
 155         f"closed={closed} elapsed={elapsed:.3f}s cadence={cadence:.3f}s/ledger"
 156     )
 157
 158     tiers: Counter[int] = Counter()
 159     counts: Counter[int] = Counter()
 160     missing_entropy = 0
 161     for seq in range(start_seq + 1, end_seq + 1):
 162         try:
 163             ce, user_txns = get_entropy_tx(ctx, seq)
 164         except AssertionError as exc:
 165             missing_entropy += 1
 166             log(f"  Ledger {seq}: no ConsensusEntropy tx ({exc})")
 167             continue
 168
 169         tier = ce.get("EntropyTier", -1)
 170         count = ce.get("EntropyCount", -1)
 171         tiers[tier] += 1
 172         counts[count] += 1
 173         log(
 174             f"  Ledger {seq}: tier={tier} count={count} "
 175             f"user_txns={len(user_txns)} digest={ce.get('Digest', '')[:16]}..."
 176         )
 177
 178     log(
 179         "SUMMARY "
 180         f"closed={closed} elapsed_s={elapsed:.3f} cadence_s={cadence:.3f} "
 181         f"tiers={dict(sorted(tiers.items()))} "
 182         f"counts={dict(sorted(counts.items()))} "
 183         f"missing_entropy={missing_entropy}"
 184     )
 185
 186     _summarize_logs(ctx, log, label="probe", started=started, ended=ended)
 187     if export_window is not None:
 188         _summarize_logs(
 189             ctx,
 190             log,
 191             label="export",
 192             started=export_window[0],
 193             ended=export_window[1],
 194         )
 195
 196     log("PASS")
```

Known remaining gaps are explicit rather than hidden:

- the optional Lean/C++ drift test covers selected scalar formulas and helper
  predicates, but broader source-to-model correspondence still relies on code
  review;
- there is no live `.testnet`/transport-level Byzantine signer that produces
  distinct valid proposal signatures per recipient; the split-brain schedule is
  covered in CSF, and the threshold property is covered by Lean plus the direct
  C++ two-cohort regression above.

## 7. Optional Lean/C++ Drift Cross-Checks

The in-tree Lean package now exposes a small scalar C ABI for optional drift
tests. This checks whether selected production formulas and helper predicates
still match the Lean model, but it does not prove implementation equivalence.
The test path is compile-gated by `formal_verification=ON`; normal builds do
not link Lean.

📍 `formal/lean/xahau_consensus/XahauConsensus/FFI.lean:20-185`
```text
  20 @[export xahau_byzantine_bound]
  21 def xahauByzantineBound (count : UInt64) : UInt64 :=
  22   (byzantineBound count.toNat).toUInt64
  23
  24 @[export xahau_participant_threshold]
  25 def xahauParticipantThreshold (count : UInt64) : UInt64 :=
  26   (participantThreshold count.toNat).toUInt64
  27
  28 @[export xahau_quorum_threshold]
  29 def xahauQuorumThreshold (count : UInt64) : UInt64 :=
  30   (quorumThreshold count.toNat).toUInt64
  31
  32 @[export xahau_safe_quorum_threshold]
  33 def xahauSafeQuorumThreshold (count : UInt64) : UInt64 :=
  34   (safeQuorumThreshold count.toNat).toUInt64
  35
  36 @[export xahau_safe_participant_threshold]
  37 def xahauSafeParticipantThreshold (count : UInt64) : UInt64 :=
  38   (safeParticipantThreshold count.toNat).toUInt64
  39
  40 @[export xahau_entropy_gate_threshold_for_view]
  41 def xahauEntropyGateThresholdForView
  42     (effectiveView originalView : UInt64) : UInt64 :=
  43   (entropyGateThresholdForView effectiveView.toNat originalView.toNat).toUInt64
  44
  45 def entropyTierCode : EntropyTier → UInt8
  46   | EntropyTier.consensusFallback => 1
  47   | EntropyTier.participantAligned => 2
  48   | EntropyTier.validatorQuorum => 3
  49
  50 @[export xahau_select_entropy_tier]
  51 def xahauSelectEntropyTier
  52     (fromUNLReport participantCount effectiveView originalView : UInt64) : UInt8 :=
  53   entropyTierCode <|
  54     selectEntropyTier
  55       (fromUNLReport != 0)
  56       participantCount.toNat
  57       effectiveView.toNat
  58       originalView.toNat
  59
  60 @[export xahau_aligned_participants]
  61 def xahauAlignedParticipants
  62     (aligned localIsMember localPublished : UInt64) : UInt64 :=
  63   (alignedParticipants
  64       aligned.toNat
  65       (localIsMember != 0)
  66       (localPublished != 0)).toUInt64
  67
  68 @[export xahau_quorum_aligned]
  69 def xahauQuorumAligned
  70     (threshold aligned localIsMember localPublished : UInt64) : UInt8 :=
  71   if quorumAligned
  72       threshold.toNat
  73       aligned.toNat
  74       (localIsMember != 0)
  75       (localPublished != 0) then
  76     1
  77   else
  78     0
  79
  80 @[export xahau_full_observation]
  81 def xahauFullObservation (peersSeen txConverged : UInt64) : UInt8 :=
  82   if fullObservation peersSeen.toNat txConverged.toNat then 1 else 0
  83
  84 @[export xahau_export_gate_proceed]
  85 def xahauExportGateProceed
  86     (alignedParticipants quorumThreshold fullObservation : UInt64) : UInt8 :=
  87   if (ExportGate.mk
  88         alignedParticipants.toNat
  89         quorumThreshold.toNat
  90         (fullObservation != 0)).proceed then
  91     1
  92   else
  93     0
  94
  95
  96 @[export xahau_strict_intersection_safe]
  97 def xahauStrictIntersectionSafe
  98     (activeView byzantineUniverse threshold : UInt64) : UInt8 :=
  99   if activeView.toNat + byzantineBound byzantineUniverse.toNat <
 100       2 * threshold.toNat then
 101     1
 102   else
 103     0
 104
 105 @[export xahau_nonvacuous_strict_intersection_safe]
 106 def xahauNonvacuousStrictIntersectionSafe
 107     (activeView byzantineUniverse threshold : UInt64) : UInt8 :=
 108   if threshold.toNat <= activeView.toNat ∧
 109       activeView.toNat + byzantineBound byzantineUniverse.toNat <
 110         2 * threshold.toNat then
 111     1
 112   else
 113     0
 114
 115 @[export xahau_participant_band_nonempty]
 116 def xahauParticipantBandNonempty
 117     (effectiveView originalView : UInt64) : UInt8 :=
 118   if participantThreshold originalView.toNat < quorumThreshold effectiveView.toNat then
 119     1
 120   else
 121     0
 122
 123 @[export xahau_export_quorum_overlap_lower_bound]
 124 def xahauExportQuorumOverlapLowerBound (activeView : UInt64) : UInt64 :=
 125   (2 * quorumThreshold activeView.toNat - activeView.toNat).toUInt64
 126
 127 @[export xahau_export_quorum_safe_under_nunl_cap]
 128 def xahauExportQuorumSafeUnderNunlCap
 129     (originalView effectiveView disabled : UInt64) : UInt8 :=
 130   if effectiveView.toNat = originalView.toNat - disabled.toNat ∧
 131       disabled.toNat <= disabledCap originalView.toNat ∧
 132       0 < effectiveView.toNat ∧
 133       effectiveView.toNat + byzantineBound originalView.toNat <
 134         2 * quorumThreshold effectiveView.toNat then
 135     1
 136   else
 137     0
 138
 139 private def maskBit (mask : UInt64) (peer : Nat) : Bool :=
 140   ((mask.toNat / (2 ^ peer)) % 2) == 1
 141
 142 @[export xahau_active_aligned_count_mask]
 143 def xahauActiveAlignedCountMask
 144     (count activeMask alignedMask : UInt64) : UInt64 :=
 145   (activeAlignedCount
 146       (maskBit activeMask)
 147       (maskBit alignedMask)
 148       count.toNat).toUInt64
 149
 150 @[export xahau_quorum_aligned_mask]
 151 def xahauQuorumAlignedMask
 152     (threshold count activeMask alignedMask localIsMember localPublished : UInt64) : UInt8 :=
 153   let aligned :=
 154     activeAlignedCount
 155       (maskBit activeMask)
 156       (maskBit alignedMask)
 157       count.toNat
 158   if quorumAligned
 159       threshold.toNat
 160       aligned
 161       (localIsMember != 0)
 162       (localPublished != 0) then
 163     1
 164   else
 165     0
 166
 167 @[export xahau_naive_sixty_percent_threshold]
 168 def xahauNaiveSixtyPercentThreshold (count : UInt64) : UInt64 :=
 169   (naiveSixtyPercentThreshold count.toNat).toUInt64
 170
 171 @[export xahau_naive_sixty_percent_is_safe]
 172 def xahauNaiveSixtyPercentIsSafe (count : UInt64) : UInt8 :=
 173   if count.toNat + byzantineBound count.toNat <
 174       2 * naiveSixtyPercentThreshold count.toNat then
 175     1
 176   else
 177     0
 178
 179 @[export xahau_disabled_cap]
 180 def xahauDisabledCap (originalView : UInt64) : UInt64 :=
 181   (disabledCap originalView.toNat).toUInt64
 182
 183 @[export xahau_effective_view]
 184 def xahauEffectiveView (originalView disabled : UInt64) : UInt64 :=
 185   (effectiveView originalView.toNat disabled.toNat).toUInt64
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:36-144`
```cpp
  36 void
  37 lean_initialize_runtime_module();
  38
  39 lean_object*
  40 initialize_xahau__consensus_XahauConsensus_FFI(
  41     uint8_t builtin,
  42     lean_object* world);
  43
  44 std::uint64_t
  45 xahau_byzantine_bound(std::uint64_t count);
  46
  47 std::uint64_t
  48 xahau_participant_threshold(std::uint64_t count);
  49
  50 std::uint64_t
  51 xahau_quorum_threshold(std::uint64_t count);
  52
  53 std::uint64_t
  54 xahau_safe_quorum_threshold(std::uint64_t count);
  55
  56 std::uint64_t
  57 xahau_safe_participant_threshold(std::uint64_t count);
  58
  59 std::uint64_t
  60 xahau_entropy_gate_threshold_for_view(
  61     std::uint64_t effectiveView,
  62     std::uint64_t originalView);
  63
  64 std::uint8_t
  65 xahau_select_entropy_tier(
  66     std::uint64_t fromUNLReport,
  67     std::uint64_t participantCount,
  68     std::uint64_t effectiveView,
  69     std::uint64_t originalView);
  70
  71 std::uint64_t
  72 xahau_aligned_participants(
  73     std::uint64_t aligned,
  74     std::uint64_t localIsMember,
  75     std::uint64_t localPublished);
  76
  77 std::uint8_t
  78 xahau_quorum_aligned(
  79     std::uint64_t threshold,
  80     std::uint64_t aligned,
  81     std::uint64_t localIsMember,
  82     std::uint64_t localPublished);
  83
  84 std::uint8_t
  85 xahau_full_observation(std::uint64_t peersSeen, std::uint64_t txConverged);
  86
  87 std::uint8_t
  88 xahau_export_gate_proceed(
  89     std::uint64_t alignedParticipants,
  90     std::uint64_t quorumThreshold,
  91     std::uint64_t fullObservation);
  92
  93 std::uint64_t
  94 xahau_disabled_cap(std::uint64_t originalView);
  95
  96 std::uint64_t
  97 xahau_effective_view(std::uint64_t originalView, std::uint64_t disabled);
  98
  99 std::uint8_t
 100 xahau_strict_intersection_safe(
 101     std::uint64_t activeView,
 102     std::uint64_t byzantineUniverse,
 103     std::uint64_t threshold);
 104
 105 std::uint8_t
 106 xahau_nonvacuous_strict_intersection_safe(
 107     std::uint64_t activeView,
 108     std::uint64_t byzantineUniverse,
 109     std::uint64_t threshold);
 110
 111 std::uint8_t
 112 xahau_participant_band_nonempty(
 113     std::uint64_t effectiveView,
 114     std::uint64_t originalView);
 115
 116 std::uint64_t
 117 xahau_export_quorum_overlap_lower_bound(std::uint64_t activeView);
 118
 119 std::uint8_t
 120 xahau_export_quorum_safe_under_nunl_cap(
 121     std::uint64_t originalView,
 122     std::uint64_t effectiveView,
 123     std::uint64_t disabled);
 124
 125 std::uint64_t
 126 xahau_active_aligned_count_mask(
 127     std::uint64_t count,
 128     std::uint64_t activeMask,
 129     std::uint64_t alignedMask);
 130
 131 std::uint8_t
 132 xahau_quorum_aligned_mask(
 133     std::uint64_t threshold,
 134     std::uint64_t count,
 135     std::uint64_t activeMask,
 136     std::uint64_t alignedMask,
 137     std::uint64_t localIsMember,
 138     std::uint64_t localPublished);
 139
 140 std::uint64_t
 141 xahau_naive_sixty_percent_threshold(std::uint64_t count);
 142
 143 std::uint8_t
 144 xahau_naive_sixty_percent_is_safe(std::uint64_t count);
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:192-208`
```cpp
 192 void
 193 initializeLean()
 194 {
 195     static std::once_flag once;
 196     std::call_once(once, [] {
 197         lean_initialize_runtime_module();
 198
 199         auto* result = initialize_xahau__consensus_XahauConsensus_FFI(
 200             1, lean_io_mk_world());
 201         if (lean_io_result_is_error(result))
 202         {
 203             lean_dec_ref(result);
 204             throw std::runtime_error{"failed to initialize XahauConsensus.FFI"};
 205         }
 206         lean_dec_ref(result);
 207     });
 208 }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:215-254`
```cpp
 215 void
 216     testThresholdFormulaDrift()
 217     {
 218         testcase("Lean/C++ consensus threshold formula drift");
 219
 220         initializeLean();
 221
 222         for (std::uint64_t count = 0; count <= 1024; ++count)
 223         {
 224             BEAST_EXPECT(xahau_byzantine_bound(count) == count / 5);
 225             BEAST_EXPECT(
 226                 xahau_participant_threshold(count) ==
 227                 calculateParticipantThreshold(count));
 228             BEAST_EXPECT(
 229                 xahau_quorum_threshold(count) ==
 230                 calculateQuorumThreshold(count));
 231         }
 232
 233         auto const max = std::numeric_limits<std::uint64_t>::max();
 234         for (std::uint64_t count :
 235              {max / 100 - 1,
 236               max / 100,
 237               max / 80 - 1,
 238               max / 80,
 239               max / 5 - 1,
 240               max / 5,
 241               max / 2 - 1,
 242               max / 2,
 243               max - 1,
 244               max})
 245         {
 246             BEAST_EXPECT(xahau_byzantine_bound(count) == count / 5);
 247             BEAST_EXPECT(
 248                 xahau_participant_threshold(count) ==
 249                 calculateParticipantThreshold(count));
 250             BEAST_EXPECT(
 251                 xahau_quorum_threshold(count) ==
 252                 calculateQuorumThreshold(count));
 253         }
 254     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:256-306`
```cpp
 256 void
 257     testSelectorAndGateDrift()
 258     {
 259         testcase("Lean/C++ entropy selector and gate drift");
 260
 261         initializeLean();
 262
 263         for (std::uint64_t effectiveView = 0; effectiveView <= 40;
 264              ++effectiveView)
 265         {
 266             auto const expectedQuorum = safeQuorumThreshold(effectiveView);
 267             BEAST_EXPECT(
 268                 xahau_safe_quorum_threshold(effectiveView) == expectedQuorum);
 269
 270             for (std::uint64_t originalView = 0; originalView <= 40;
 271                  ++originalView)
 272             {
 273                 auto const expectedParticipant =
 274                     safeParticipantThreshold(originalView);
 275
 276                 BEAST_EXPECT(
 277                     xahau_safe_participant_threshold(originalView) ==
 278                     expectedParticipant);
 279                 BEAST_EXPECT(
 280                     xahau_entropy_gate_threshold_for_view(
 281                         effectiveView, originalView) ==
 282                     ConsensusExtensions::entropyGateThresholdForView(
 283                         effectiveView, originalView));
 284
 285                 for (std::uint64_t participantCount = 0; participantCount <= 45;
 286                      ++participantCount)
 287                 {
 288                     for (bool fromUNLReport : {false, true})
 289                     {
 290                         BEAST_EXPECT(
 291                             xahau_select_entropy_tier(
 292                                 fromUNLReport ? 1 : 0,
 293                                 participantCount,
 294                                 effectiveView,
 295                                 originalView) ==
 296                             static_cast<std::uint8_t>(
 297                                 ConsensusExtensions::selectEntropyTierForView(
 298                                     fromUNLReport,
 299                                     participantCount,
 300                                     effectiveView,
 301                                     originalView)));
 302                     }
 303                 }
 304             }
 305         }
 306     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:308-371`
```cpp
 308 void
 309     testSidecarAndExportGateDrift()
 310     {
 311         testcase("Lean/C++ sidecar alignment and export gate drift");
 312
 313         initializeLean();
 314
 315         for (std::uint64_t aligned = 0; aligned <= 12; ++aligned)
 316         {
 317             for (std::uint64_t threshold = 0; threshold <= 12; ++threshold)
 318             {
 319                 for (bool localIsMember : {false, true})
 320                 {
 321                     for (bool localPublished : {false, true})
 322                     {
 323                         auto const expectedAligned =
 324                             detail::sidecarAlignedParticipants(
 325                                 aligned, localIsMember, localPublished);
 326
 327                         BEAST_EXPECT(
 328                             xahau_aligned_participants(
 329                                 aligned,
 330                                 localIsMember ? 1 : 0,
 331                                 localPublished ? 1 : 0) == expectedAligned);
 332                         BEAST_EXPECT(
 333                             (xahau_quorum_aligned(
 334                                  threshold,
 335                                  aligned,
 336                                  localIsMember ? 1 : 0,
 337                                  localPublished ? 1 : 0) != 0) ==
 338                             detail::sidecarQuorumAligned(
 339                                 aligned,
 340                                 localIsMember,
 341                                 localPublished,
 342                                 threshold));
 343                     }
 344                 }
 345
 346                 for (bool fullObservation : {false, true})
 347                 {
 348                     BEAST_EXPECT(
 349                         (xahau_export_gate_proceed(
 350                              aligned, threshold, fullObservation ? 1 : 0) !=
 351                          0) ==
 352                         detail::exportSigSetQuorumAligned(aligned, threshold));
 353                 }
 354             }
 355         }
 356
 357         for (std::uint64_t peersSeen = 0; peersSeen <= 12; ++peersSeen)
 358         {
 359             for (std::uint64_t txConverged = 0; txConverged <= 12;
 360                  ++txConverged)
 361             {
 362                 detail::SidecarPeerAlignment state;
 363                 state.peersSeen = peersSeen;
 364                 state.txConverged = txConverged;
 365
 366                 BEAST_EXPECT(
 367                     (xahau_full_observation(peersSeen, txConverged) != 0) ==
 368                     detail::sidecarFullObservation(peersSeen, txConverged));
 369             }
 370         }
 371     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:373-405`
```cpp
 373 void
 374     testNunlCapDrift()
 375     {
 376         testcase("Lean/C++ NegativeUNL cap arithmetic drift");
 377
 378         initializeLean();
 379
 380         for (std::uint64_t originalView = 0; originalView <= 1024;
 381              ++originalView)
 382         {
 383             auto const expectedCap =
 384                 NegativeUNLVote::maxNegativeUNLListed(originalView);
 385             BEAST_EXPECT(xahau_disabled_cap(originalView) == expectedCap);
 386
 387             for (std::uint64_t disabled = 0; disabled <= 16; ++disabled)
 388             {
 389                 auto const expectedEffective =
 390                     disabled > originalView ? 0 : originalView - disabled;
 391                 BEAST_EXPECT(
 392                     xahau_effective_view(originalView, disabled) ==
 393                     expectedEffective);
 394             }
 395         }
 396
 397         auto const max = std::numeric_limits<std::uint64_t>::max();
 398         for (std::uint64_t originalView :
 399              {max / 4 - 1, max / 4, max / 2, max - 1, max})
 400         {
 401             BEAST_EXPECT(
 402                 xahau_disabled_cap(originalView) ==
 403                 NegativeUNLVote::maxNegativeUNLListed(originalView));
 404         }
 405     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:407-439`
```cpp
 407 void
 408     testViewUniverseDrift()
 409     {
 410         testcase("Lean/C++ view-universe safety predicate drift");
 411
 412         initializeLean();
 413
 414         for (std::uint64_t effectiveView = 0; effectiveView <= 40;
 415              ++effectiveView)
 416         {
 417             for (std::uint64_t originalView = 0; originalView <= 40;
 418                  ++originalView)
 419             {
 420                 auto const threshold =
 421                     calculateParticipantThreshold(originalView);
 422                 BEAST_EXPECT(
 423                     (xahau_strict_intersection_safe(
 424                          effectiveView, originalView, threshold) != 0) ==
 425                     strictIntersectionSafeCpp(
 426                         effectiveView, originalView, threshold));
 427                 BEAST_EXPECT(
 428                     (xahau_nonvacuous_strict_intersection_safe(
 429                          effectiveView, originalView, threshold) != 0) ==
 430                     (threshold <= effectiveView &&
 431                      strictIntersectionSafeCpp(
 432                          effectiveView, originalView, threshold)));
 433                 BEAST_EXPECT(
 434                     (xahau_participant_band_nonempty(
 435                          effectiveView, originalView) != 0) ==
 436                     participantBandNonemptyCpp(effectiveView, originalView));
 437             }
 438         }
 439     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:441-497`
```cpp
 441 void
 442     testExportQuorumDrift()
 443     {
 444         testcase("Lean/C++ export quorum safety predicate drift");
 445
 446         initializeLean();
 447
 448         for (std::uint64_t activeView = 0; activeView <= 64; ++activeView)
 449         {
 450             auto const quorum = calculateQuorumThreshold(activeView);
 451             auto const expectedOverlap =
 452                 2 * quorum > activeView ? 2 * quorum - activeView : 0;
 453             BEAST_EXPECT(
 454                 xahau_export_quorum_overlap_lower_bound(activeView) ==
 455                 expectedOverlap);
 456         }
 457
 458         for (std::uint64_t originalView = 0; originalView <= 64; ++originalView)
 459         {
 460             auto const cap =
 461                 NegativeUNLVote::maxNegativeUNLListed(originalView);
 462             for (std::uint64_t disabled = 0; disabled <= cap + 2; ++disabled)
 463             {
 464                 auto const effectiveView =
 465                     disabled > originalView ? 0 : originalView - disabled;
 466                 auto const expected =
 467                     disabled <= cap && effectiveView > 0 &&
 468                     strictIntersectionSafeCpp(
 469                         effectiveView,
 470                         originalView,
 471                         calculateQuorumThreshold(effectiveView));
 472                 BEAST_EXPECT(
 473                     (xahau_export_quorum_safe_under_nunl_cap(
 474                          originalView, effectiveView, disabled) != 0) ==
 475                     expected);
 476             }
 477         }
 478
 479         struct NunlPremiseCase
 480         {
 481             std::uint64_t originalView;
 482             std::uint64_t effectiveView;
 483             std::uint64_t disabled;
 484         };
 485
 486         for (auto const& c : {
 487                  NunlPremiseCase{20, 15, 4},  // effective view should be 16
 488                  NunlPremiseCase{20, 17, 4},  // effective view should be 16
 489                  NunlPremiseCase{20, 14, 6},  // disabled is above cap
 490                  NunlPremiseCase{10, 6, 4},   // disabled is above cap
 491              })
 492         {
 493             BEAST_EXPECT(
 494                 xahau_export_quorum_safe_under_nunl_cap(
 495                     c.originalView, c.effectiveView, c.disabled) == 0);
 496         }
 497     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:499-571`
```cpp
 499 void
 500     testSidecarMaskDrift()
 501     {
 502         testcase("Lean/C++ active-view sidecar mask drift");
 503
 504         initializeLean();
 505
 506         struct MaskCase
 507         {
 508             std::uint64_t count;
 509             std::uint64_t activeMask;
 510             std::uint64_t alignedMask;
 511         };
 512
 513         auto const checkMaskCase = [&](MaskCase const& c) {
 514             auto const aligned =
 515                 activeAlignedCountMaskCpp(c.count, c.activeMask, c.alignedMask);
 516             BEAST_EXPECT(
 517                 xahau_active_aligned_count_mask(
 518                     c.count, c.activeMask, c.alignedMask) == aligned);
 519
 520             for (std::uint64_t threshold = 0; threshold <= 12; ++threshold)
 521             {
 522                 for (bool localIsMember : {false, true})
 523                 {
 524                     for (bool localPublished : {false, true})
 525                     {
 526                         BEAST_EXPECT(
 527                             (xahau_quorum_aligned_mask(
 528                                  threshold,
 529                                  c.count,
 530                                  c.activeMask,
 531                                  c.alignedMask,
 532                                  localIsMember ? 1 : 0,
 533                                  localPublished ? 1 : 0) != 0) ==
 534                             detail::sidecarQuorumAligned(
 535                                 aligned,
 536                                 localIsMember,
 537                                 localPublished,
 538                                 threshold));
 539                     }
 540                 }
 541             }
 542         };
 543
 544         for (std::uint64_t count = 0; count <= 6; ++count)
 545         {
 546             auto const maxMask = std::uint64_t{1} << count;
 547             for (std::uint64_t activeMask = 0; activeMask < maxMask;
 548                  ++activeMask)
 549             {
 550                 for (std::uint64_t alignedMask = 0; alignedMask < maxMask;
 551                      ++alignedMask)
 552                 {
 553                     checkMaskCase(MaskCase{count, activeMask, alignedMask});
 554                 }
 555             }
 556         }
 557
 558         for (auto const& c : {
 559                  MaskCase{8, 0b10110110, 0b11111111},
 560                  MaskCase{12, 0b101010101010, 0b111100001111},
 561                  MaskCase{63, std::uint64_t{1} << 62, std::uint64_t{1} << 62},
 562                  MaskCase{64, std::uint64_t{1} << 63, std::uint64_t{1} << 63},
 563                  MaskCase{
 564                      64,
 565                      (std::uint64_t{1} << 63) | 0b1011,
 566                      (std::uint64_t{1} << 63) | 0b0110},
 567              })
 568         {
 569             checkMaskCase(c);
 570         }
 571     }
```

📍 `formal/xahaud-feature-export-rng-lean/src/test/formal_verification/LeanConsensus_test.cpp:573-595`
```cpp
 573 void
 574     testNaiveSixtyPercentRegression()
 575     {
 576         testcase("Lean/C++ naive 60 percent threshold regression anchors");
 577
 578         initializeLean();
 579
 580         for (std::uint64_t count = 0; count <= 1024; ++count)
 581         {
 582             auto const naive = (count * 60 + 99) / 100;
 583             BEAST_EXPECT(xahau_naive_sixty_percent_threshold(count) == naive);
 584             BEAST_EXPECT(
 585                 (xahau_naive_sixty_percent_is_safe(count) != 0) ==
 586                 strictIntersectionSafeCpp(count, count, naive));
 587
 588             if (count > 0 && count % 5 == 0)
 589             {
 590                 BEAST_EXPECT(!strictIntersectionSafeCpp(count, count, naive));
 591                 BEAST_EXPECT(strictIntersectionSafeCpp(
 592                     count, count, calculateParticipantThreshold(count)));
 593             }
 594         }
 595     }
```

## 8. What The Proofs Do Not Claim

These proofs are useful because they are narrow:

- They do not verify C++ memory safety, serialization, networking, or threading.
- They do not prove the whole consensus protocol.
- They do not prove runtime liveness under every scheduler.
- They do not prove safety for arbitrary heterogeneous or dynamic UNL
  topologies; the modeled sidecar intersections live inside one active-view
  universe.
- They do not prove the 20% base-consensus liveness/asynchrony bound.
- They do not prove the actual C++ parser accepts exactly the modeled inputs.
- They do not mechanically prove equivalence between the C++ and Lean; the
  anchors are review traceability.
- They do not model signature verification, proposal authentication, SHAMap
  acquisition/merge, amendment/config gating, or full ledger apply semantics.
- RuntimeConfig latency/drop probes are operational coverage, not formal model
  inputs.
- The entropy selector model is explicitly the network / non-failed / non-empty
  tier ladder.

What they do prove is the high-risk arithmetic and decision logic that review
kept returning to:

1. the Tier 2 floor is the minimal strict-intersection threshold;
2. original-view anchoring survives nUNL shrinkage;
3. sidecar alignment ignores nonmembers;
4. non-UNLReport entropy fails closed;
5. the gate and selector agree on the non-fallback boundary;
6. export sidecar-gate success is quorum-aligned, and below-quorum network
   apply retries or expires rather than fabricating fallback signatures.

Those are exactly the places where informal prose was easiest to overstate.
