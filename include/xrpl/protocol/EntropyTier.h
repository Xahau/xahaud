#ifndef RIPPLE_PROTOCOL_ENTROPY_TIER_H_INCLUDED
#define RIPPLE_PROTOCOL_ENTROPY_TIER_H_INCLUDED

#include <cstdint>

namespace ripple {

/// Which gate the ledger's entropy passed. Stored in sfEntropyTier (UINT8)
/// on the ttCONSENSUS_ENTROPY pseudo-transaction and the ConsensusEntropy
/// ledger entry.
///
/// EntropyCount says how many validators contributed; EntropyTier says which
/// gate the result passed. Values are strength-ordered so consumers can gate
/// with a numeric comparison (tier >= required).
enum EntropyTier : std::uint8_t {
    /// No usable entropy (reserved; a fresh ConsensusEntropy entry should
    /// always carry one of the tiers below).
    entropyTierNone = 0,

    /// Consensus-bound deterministic fallback: derived from already-agreed
    /// round inputs (parent ledger hash, base tx set hash, sequence) under
    /// HashPrefix::entropyFallback when no agreed reveal set reaches either
    /// participant_aligned or validator_quorum. Unpredictable in practice but
    /// user-influenceable via transaction submission — never suitable for
    /// value-bearing outcomes.
    entropyTierConsensusFallback = 1,

    /// Participant-aligned sub-quorum entropy: the agreed reveal set aligned at
    /// the tier-2 participant threshold — below the 80% validator quorum but at
    /// or above the equivocation-intersection floor over the original
    /// (pre-nUNL)
    /// view. Weaker than validator_quorum; opt-in for hooks via min_tier.
    entropyTierParticipantAligned = 2,

    /// Validator commit/reveal entropy whose sidecar set passed the
    /// active-validator-view quorum alignment gate.
    entropyTierValidatorQuorum = 3,
};

}  // namespace ripple

#endif
