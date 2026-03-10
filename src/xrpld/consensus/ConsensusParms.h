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
     *  Waiting the full 10s just delays the inevitable ZERO-entropy
     *  fallback and slows recovery for the restarting node (it can't
     *  catch up until the survivors close a ledger).
     *
     *  3s is long enough for commits to propagate on any reasonable
     *  network, but short enough that a missing-node scenario recovers
     *  quickly via the ZERO-entropy fallback path.
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

    /** Seed value for prevRoundTime_ on first round when bootstrap fast start
     *  is active.  Instead of the full ledgerIDLE_INTERVAL (15s), this much
     *  shorter seed lets the close-speed governor (openTime >= prevRoundTime/2)
     *  allow closing within ~1.5s, cutting ~12s per early round.
     */
    std::chrono::milliseconds bootstrapRoundTimeSeed = std::chrono::seconds{3};

    /** Number of consecutive rounds with quorum participation required before
     *  bootstrap fast start auto-disables.
     */
    std::size_t bootstrapStableRoundsRequired = 3;
};

/** Calculate the 80% quorum threshold (rounded up) for a given count.

    This is the standard quorum used for consensus validation, matching
    the formula in ValidatorList::calculateQuorum (std::ceil(n * 0.8f)).
    Uses integer arithmetic: (count * 80 + 99) / 100 == ceil(count * 0.8).

    @param count The number of validators or proposers
    @return The minimum number needed for quorum (80%, rounded up)
*/
inline std::size_t
calculateQuorumThreshold(std::size_t count)
{
    return (count * 80 + 99) / 100;
}

}  // namespace ripple
#endif
