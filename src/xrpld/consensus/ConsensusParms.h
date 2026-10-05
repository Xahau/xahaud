//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2017 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#ifndef RIPPLE_CONSENSUS_CONSENSUS_PARMS_H_INCLUDED
#define RIPPLE_CONSENSUS_CONSENSUS_PARMS_H_INCLUDED

#include <chrono>
#include <cstddef>

namespace ripple {

/** Consensus algorithm parameters

    Parameters which control the consensus algorithm.  This are not
    meant to be changed arbitrarily.
*/
struct ConsensusParms
{
    explicit ConsensusParms() = default;

    //-------------------------------------------------------------------------
    // Validation and proposal durations are relative to NetClock times, so use
    // second resolution
    /** The duration a validation remains current after its ledger's
       close time.

        This is a safety to protect against very old validations and the time
        it takes to adjust the close time accuracy window.
    */
    std::chrono::seconds validationVALID_WALL = std::chrono::minutes{5};

    /** Duration a validation remains current after first observed.

       The duration a validation remains current after the time we
       first saw it. This provides faster recovery in very rare cases where the
       number of validations produced by the network is lower than normal
    */
    std::chrono::seconds validationVALID_LOCAL = std::chrono::minutes{3};

    /**  Duration pre-close in which validations are acceptable.

        The number of seconds before a close time that we consider a validation
        acceptable. This protects against extreme clock errors
    */
    std::chrono::seconds validationVALID_EARLY = std::chrono::minutes{3};

    //! How long we consider a proposal fresh
    std::chrono::seconds proposeFRESHNESS = std::chrono::seconds{20};

    //! How often we force generating a new proposal to keep ours fresh
    std::chrono::seconds proposeINTERVAL = std::chrono::seconds{12};

    //-------------------------------------------------------------------------
    // Consensus durations are relative to the internal Consensus clock and use
    // millisecond resolution.

    //! The percentage threshold above which we can declare consensus.
    std::size_t minCONSENSUS_PCT = 80;

    //! The duration a ledger may remain idle before closing
    std::chrono::milliseconds ledgerIDLE_INTERVAL = std::chrono::seconds{15};

    //! The number of seconds we wait minimum to ensure participation
    std::chrono::milliseconds ledgerMIN_CONSENSUS =
        std::chrono::milliseconds{1950};

    /** The maximum amount of time to spend pausing for laggards.
     *
     *  This should be sufficiently less than validationFRESHNESS so that
     *  validators don't appear to be offline that are merely waiting for
     *  laggards.
     */
    std::chrono::milliseconds ledgerMAX_CONSENSUS = std::chrono::seconds{15};

    /** Maximum time to wait for RNG commit/reveal quorum before giving up.
     *
     *  This is intentionally shorter than ledgerMAX_CONSENSUS because
     *  waiting longer won't help: a node that missed the start of the
     *  round (e.g. restarting after a crash) enters as proposing=false
     *  and cannot generate commitments until consensus promotes it to
     *  proposing — which takes at least one full round of observing.
     *  Waiting the full 10s just delays the inevitable non-zero Tier-1
     *  fallback and slows recovery for the restarting node (it can't
     *  catch up until the survivors close a ledger).
     *
     *  3s is long enough for commits to propagate on any reasonable
     *  network, but short enough that a missing-node scenario recovers
     *  quickly via the Tier-1 fallback path.
     */
    std::chrono::milliseconds rngPIPELINE_TIMEOUT = std::chrono::seconds{3};

    /** Reveal-phase timeout — maximum time to wait for reveals after
     *  entering ConvergingReveal.  Measured from the moment we broadcast
     *  our own reveal, NOT from round start.  This is the defense against
     *  a validator that commits but never reveals (crash or malice).
     *  1.5s is generous for propagation on any network.
     */
    std::chrono::milliseconds rngREVEAL_TIMEOUT =
        std::chrono::milliseconds{1500};

    //! Minimum number of seconds to wait to ensure others have computed the LCL
    std::chrono::milliseconds ledgerMIN_CLOSE = std::chrono::seconds{2};

    //! How often we check state or change positions
    std::chrono::milliseconds ledgerGRANULARITY = std::chrono::seconds{1};

    /** The minimum amount of time to consider the previous round
        to have taken.

        The minimum amount of time to consider the previous round
        to have taken. This ensures that there is an opportunity
        for a round at each avalanche threshold even if the
        previous consensus was very fast. This should be at least
        twice the interval between proposals (0.7s) divided by
        the interval between mid and late consensus ([85-50]/100).
    */
    std::chrono::milliseconds avMIN_CONSENSUS_TIME = std::chrono::seconds{5};

    //------------------------------------------------------------------------------
    // Avalanche tuning
    // As a function of the percent this round's duration is of the prior round,
    // we increase the threshold for yes votes to add a transaction to our
    // position.

    //! Percentage of nodes on our UNL that must vote yes
    std::size_t avINIT_CONSENSUS_PCT = 50;

    //! Percentage of previous round duration before we advance
    std::size_t avMID_CONSENSUS_TIME = 50;

    //! Percentage of nodes that most vote yes after advancing
    std::size_t avMID_CONSENSUS_PCT = 65;

    //! Percentage of previous round duration before we advance
    std::size_t avLATE_CONSENSUS_TIME = 85;

    //! Percentage of nodes that most vote yes after advancing
    std::size_t avLATE_CONSENSUS_PCT = 70;

    //! Percentage of previous round duration before we are stuck
    std::size_t avSTUCK_CONSENSUS_TIME = 200;

    //! Percentage of nodes that must vote yes after we are stuck
    std::size_t avSTUCK_CONSENSUS_PCT = 95;

    //! Percentage of nodes required to reach agreement on ledger close time
    std::size_t avCT_CONSENSUS_PCT = 75;
};

/** Calculate the 80% quorum threshold (rounded up) for a given count.

    This is the standard quorum used for consensus validation, matching
    the formula in ValidatorList::calculateQuorum (std::ceil(n * 0.8f)).
    Uses integer arithmetic equivalent to ceil(count * 0.8), split into
    quotient/remainder terms so very large test inputs cannot overflow.

    @param count The number of validators or proposers
    @return The minimum number needed for quorum (80%, rounded up)
*/
inline std::size_t
calculateQuorumThreshold(std::size_t count)
{
    auto const quotient = count / 100;
    auto const remainder = count % 100;
    return quotient * 80 + (remainder * 80 + 99) / 100;
}

/** Safe quorum helper for consensus-extension gates.

    The raw quorum formula returns zero for an empty view. Consensus-extension
    sidecar gates use a fail-closed floor of one participant instead: an empty
    local view must not make a sidecar quorum vacuously true.
*/
inline std::size_t
safeQuorumThreshold(std::size_t count)
{
    return count == 0 ? 1 : calculateQuorumThreshold(count);
}

namespace detail {

// floor((lhs + rhs) / 2), without forming the potentially overflowing sum.
inline std::size_t
floorHalfSum(std::size_t lhs, std::size_t rhs)
{
    return lhs / 2 + rhs / 2 + (lhs % 2 + rhs % 2) / 2;
}

}  // namespace detail

/** Calculate the Tier 2 (participant_aligned) alignment floor.

    Tier 2 sub-quorum entropy aligns a cohort at this lower bar. The floor is
    DERIVED, not a fixed fraction: it is the smallest cohort size t whose
    pairwise intersection within the view strictly exceeds the tolerated
    Byzantine count f = floor(count / 5) (~20%). Two t-cohorts in a count-sized
    view overlap in at least 2t - count validators; requiring 2t - count > f
    guarantees an HONEST validator in every such overlap. Since an honest
    validator advertises only one entropy-set hash, that shared honest node
    stops a single equivocator (or up to f colluding Byzantine nodes) from
    minting two distinct aligned digests for the same round -> no fork.

    Solving 2t - count > f for the smallest integer t gives
    t = floor((count + f) / 2) + 1. This is ~0.6 * count and equals
    ceil(0.6 * count) at every count EXCEPT multiples of 5, where the plain 0.6
    bar leaves the overlap exactly equal to f (not greater) and is therefore
    forkable -- there this is one higher. (E.g. count=10: f=2, this yields 7,
    whereas ceil(0.6*10)=6 leaves overlap 12-10=2 == f.)

    Anchored to the ORIGINAL (pre-nUNL) view size, NOT the effective (post-nUNL)
    one: the Byzantine bound is over the original UNL, and nUNL can shrink the
    effective view while leaving faulty nodes in it. See
    ActiveValidatorView::originalViewSize.

    @param count The original (pre-nUNL) number of active validators
    @return The minimum cohort size for safe participant alignment
*/
inline std::size_t
calculateParticipantThreshold(std::size_t count)
{
    // f = floor(0.2 * count) tolerated Byzantine validators; the smallest t
    // with 2t - count > f is floor((count + f) / 2) + 1.
    auto const byzantine = count / 5;
    return detail::floorHalfSum(count, byzantine) + 1;
}

/** Safe Tier-2 helper for consensus-extension gates.

    Like safeQuorumThreshold(), this floors empty-view sidecar gates at one
    participant so they fail closed instead of succeeding vacuously.
*/
inline std::size_t
safeParticipantThreshold(std::size_t count)
{
    return count == 0 ? 1 : calculateParticipantThreshold(count);
}

}  // namespace ripple
#endif
