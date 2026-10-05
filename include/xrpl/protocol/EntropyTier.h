#ifndef RIPPLE_PROTOCOL_ENTROPY_TIER_H_INCLUDED
#define RIPPLE_PROTOCOL_ENTROPY_TIER_H_INCLUDED

#include <cstdint>

namespace ripple {

/// Quality label of the ledger's entropy. Stored in sfEntropyTier (UINT8)
/// on the ttCONSENSUS_ENTROPY pseudo-transaction.
///
/// EntropyCount says how many validators contributed; EntropyDenominator says
/// how many active validators were in the ledger-anchored view for that
/// non-fallback result; EntropyTier labels the result. Non-fallback labels are
/// derived from the leaf count of the accepted reveal set. The proceed gate
/// (an alignment count over the active view reaching entropyGateThreshold())
/// is a separate check: it decides whether a reveal root is accepted at all,
/// not which tier it earns. Fallback entropy carries count=0/denominator=0
/// because no validator-derived denominator was accepted. Tier values are
/// strength-ordered so consumers can gate with a numeric comparison
/// (tier >= required).
///
/// RESIDUAL BIAS — applies to fallback, participant_aligned, and
/// validator_quorum. This is a commit/reveal scheme: a validator can withhold
/// its reveal until after observing peers' reveals, choosing between two
/// outcomes (its contribution in vs. out) — up to one bit of influence per
/// withholder, and colluding withholders near a threshold can instead force a
/// downgrade to a lower tier. These tiers bound and *label* manipulation (it is
/// observable and limited); they are NOT bias-resistant against a colluding
/// validator minority. A hook that requires validator_full fails closed if any
/// active validator withholds, trading availability for no selective-
/// withholding slack.
enum EntropyTier : std::uint8_t {
    /// No usable entropy (reserved; a fresh ConsensusEntropy input should
    /// always carry one of the tiers below).
    entropyTierNone = 0,

    /// Consensus-bound deterministic fallback: derived from already-agreed
    /// round inputs (parent ledger hash, base tx set hash, sequence) under
    /// HashPrefix::entropyFallback when the round has no accepted, well-formed
    /// reveal set that earns a non-fallback tier (its leaf count meets neither
    /// the participant nor the quorum threshold), or its active view is not
    /// UNLReport-backed. Unpredictable in practice but
    /// user-influenceable via transaction submission — never suitable for
    /// value-bearing outcomes.
    entropyTierConsensusFallback = 1,

    /// Participant-aligned sub-quorum entropy: the accepted reveal set holds
    /// fewer leaves than the 80% validator quorum but at least the
    /// equivocation-intersection floor (tier2Threshold) over the original
    /// (pre-nUNL) view. Weaker than validator_quorum; opt-in for hooks via
    /// min_tier.
    entropyTierParticipantAligned = 2,

    /// Validator commit/reveal entropy whose accepted reveal set holds at
    /// least the 80% quorum (quorumThreshold) of the effective active view,
    /// but not every validator in it.
    entropyTierValidatorQuorum = 3,

    /// Validator commit/reveal entropy with reveals from every validator in
    /// the ledger-anchored active view. Any missing active validator downgrades
    /// the tier, so hooks can require this to fail closed on selective
    /// withholding.
    entropyTierValidatorFull = 4,
};

}  // namespace ripple

#endif
