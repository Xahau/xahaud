#ifndef RIPPLE_CONSENSUS_CONSENSUSEXTENSIONSTICK_H_INCLUDED
#define RIPPLE_CONSENSUS_CONSENSUSEXTENSIONSTICK_H_INCLUDED

#include <xrpld/consensus/ConsensusTypes.h>
#include <xrpl/basics/Log.h>
#include <chrono>
#include <cstddef>

namespace ripple {

namespace detail {

inline std::size_t
sidecarLocalContribution(bool localCounts)
{
    return localCounts ? 1 : 0;
}

inline std::size_t
sidecarLocalContribution(bool localIsMember, bool localPublished)
{
    return sidecarLocalContribution(localIsMember && localPublished);
}

inline std::size_t
sidecarAlignedParticipants(std::size_t aligned, bool localCounts)
{
    return aligned + sidecarLocalContribution(localCounts);
}

inline std::size_t
sidecarAlignedParticipants(
    std::size_t aligned,
    bool localIsMember,
    bool localPublished)
{
    return aligned + sidecarLocalContribution(localIsMember, localPublished);
}

inline bool
sidecarQuorumAligned(
    std::size_t aligned,
    bool localCounts,
    std::size_t threshold)
{
    return sidecarAlignedParticipants(aligned, localCounts) >= threshold;
}

inline bool
sidecarQuorumAligned(
    std::size_t aligned,
    bool localIsMember,
    bool localPublished,
    std::size_t threshold)
{
    return sidecarAlignedParticipants(aligned, localIsMember, localPublished) >=
        threshold;
}

inline bool
sidecarFullObservation(std::size_t peersSeen, std::size_t txConverged)
{
    return peersSeen == txConverged;
}

template <class Parms>
auto
sidecarConvergenceTimeout(Parms const& parms)
{
    return parms.rngREVEAL_TIMEOUT * 2;
}

struct SidecarPeerAlignment
{
    bool localCounts = false;
    bool conflict = false;
    std::size_t aligned = 0;
    std::size_t peersSeen = 0;
    std::size_t txConverged = 0;

    std::size_t
    alignedParticipants() const
    {
        return sidecarAlignedParticipants(aligned, localCounts);
    }

    bool
    quorumAligned(std::size_t quorum) const
    {
        return sidecarQuorumAligned(aligned, localCounts, quorum);
    }

    bool
    fullObservation() const
    {
        return sidecarFullObservation(peersSeen, txConverged);
    }
};

template <
    class PeerPositions,
    class Position,
    class GetHash,
    class IsMember,
    class OnMismatch>
SidecarPeerAlignment
inspectTxConvergedSidecarPeers(
    PeerPositions const& peerPositions,
    Position const& pos,
    bool localIsMember,
    GetHash getHash,
    IsMember isMember,
    OnMismatch onMismatch)
{
    SidecarPeerAlignment state;
    auto const localHash = getHash(pos);
    if (!localHash)
        return state;

    // The alignment-counting universe must be the active validator view, the
    // same denominator the entropy/export thresholds use (quorumThreshold /
    // tier2Threshold are computed over that view). A trusted-but-non-active
    // proposer can tx-converge and advertise a sidecar hash, but it must NOT
    // pad alignedParticipants(): counting outside the active view inflates the
    // universe N above originalViewSize and erodes the Tier-2 intersection
    // margin (2t - N) below the Byzantine floor f, breaking equivocation
    // uniqueness. Mirror buildEntropySet/hasQuorumOfCommits' containsNode
    // filter, and only count our own +1 when this node is itself active.
    state.localCounts = localIsMember;
    for (auto const& [nodeId, peerPos] : peerPositions)
    {
        if (!isMember(nodeId))
            continue;  // outside the active view -> not in the counting
                       // universe
        auto const& pp = peerPos.proposal().position();
        if (positionTxSetID(pp) != positionTxSetID(pos))
            continue;  // not tx-converged
        ++state.txConverged;

        auto const peerHash = getHash(pp);
        if (!peerHash)
            continue;  // peer hasn't published this sidecar hash yet
        ++state.peersSeen;

        if (*peerHash == *localHash)
        {
            ++state.aligned;
            continue;
        }

        state.conflict = true;
        onMismatch(peerHash);
    }

    return state;
}

}  // namespace detail

/// Shared RNG sub-state machine and export sig convergence gate.
///
/// Templated so both production (ConsensusExtensions) and test (CSF Peer)
/// run the same logic with different leaf method implementations.
///
/// @param ext  Object providing RNG methods (hasQuorumOfCommits, etc.)
///             and state members (estState_, revealPhaseStart_, etc.)
/// @param ctx  ConsensusTick with callbacks into the consensus engine
template <class Ext, class Ctx>
ExtensionTickResult
extensionsTick(Ext& ext, Ctx const& ctx)
{
    // --- RNG Sub-state Checkpoints ---
    // These sub-states use union convergence (not avalanche).
    // Commits and reveals arrive piggybacked on proposals, so by the time
    // we reach these checkpoints most data is already collected. Local
    // SHAMap snapshots materialize accepted roots, but peers do not fetch,
    // serve, or merge sidecar roots during the round.
    //
    // Why an 80% fast path for commits but 100% for reveals?
    //
    // COMMITS: the immediate transition still uses the 80%
    // validator_quorum threshold. If that fast path is not reached, bounded
    // timeout/impossible-participant logic may still proceed at the lower
    // entropyGateThreshold() so Tier 2 participant_aligned rounds can close.
    //
    // REVEALS: the commit set is now locked and we know *exactly* who
    // committed.  Every committer broadcasts their reveal immediately.
    // So we wait for ALL of them, with rngREVEAL_TIMEOUT (measured
    // from ConvergingReveal entry) as the safety valve for nodes that
    // crash between commit and reveal.

    bool const isRngEnabled = ext.rngEnabled();
    bool const isExportEnabled = ext.exportEnabled();
    auto const toMs = [](auto duration) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration)
            .count();
    };

    JLOG(ext.j_.trace()) << "RNGGATE: phaseEstablish"
                         << " buildSeq=" << ctx.buildSeq << " prevSeq="
                         << (static_cast<std::uint32_t>(ctx.buildSeq) - 1)
                         << " rngEnabled=" << (isRngEnabled ? "yes" : "no")
                         << " exportEnabled="
                         << (isExportEnabled ? "yes" : "no")
                         << " estState=" << static_cast<int>(ext.estState_)
                         << " mode=" << to_string(ctx.mode)
                         << " roundMs=" << ctx.roundTime.count();

    if (isRngEnabled || isExportEnabled)
    {
        if constexpr (requires {
                          ext.recordParticipantDiagnostics(
                              ctx.mode, ctx.peerPositions);
                      })
        {
            // Diagnostic only: this records the active-UNL participants visible
            // to this node so proposals can carry a signed hash for debugging
            // timing/degraded-network cases. It is not a quorum denominator.
            ext.recordParticipantDiagnostics(ctx.mode, ctx.peerPositions);
        }
    }

    if (isRngEnabled)
    {
        auto const buildSeq = ctx.buildSeq;
        auto const estStateName = [&]() -> char const* {
            switch (ext.estState_)
            {
                case EstablishState::ConvergingTx:
                    return "ConvergingTx";
                case EstablishState::ConvergingCommit:
                    return "ConvergingCommit";
                case EstablishState::ConvergingReveal:
                    return "ConvergingReveal";
            }
            return "Unknown";
        };
        auto logRngDiag = [&](char const* reason) {
            auto const ourPos = ctx.getPosition();
            auto const participants = ctx.peerPositions.size() + 1;
            JLOG(ext.j_.debug())
                << "STALLDIAG: " << reason << " state=" << estStateName()
                << " buildSeq=" << buildSeq << " phase=establish"
                << " mode=" << to_string(ctx.mode)
                << " roundMs=" << ctx.roundTime.count()
                << " convergePct=" << ctx.convergePercent
                << " participants=" << participants
                << " peerPositions=" << ctx.peerPositions.size()
                << " prevProposers=" << ctx.prevProposers
                << " closeTimeConsensus="
                << (ctx.haveCloseTimeConsensus ? "yes" : "no")
                << " txSet=" << ourPos;

            JLOG(ext.j_.debug())
                << "STALLDIAG: sidecar"
                << " commitSetHash="
                << (ourPos.commitSetHash ? to_string(*ourPos.commitSetHash)
                                         : std::string{"none"})
                << " entropySetHash="
                << (ourPos.entropySetHash ? to_string(*ourPos.entropySetHash)
                                          : std::string{"none"})
                << " exportSigSetHash="
                << (ourPos.exportSigSetHash
                        ? to_string(*ourPos.exportSigSetHash)
                        : std::string{"none"})
                << " myCommitment=" << (ourPos.myCommitment ? "yes" : "no")
                << " myReveal=" << (ourPos.myReveal ? "yes" : "no");

            auto const commits = ext.pendingCommitCount();
            auto const quorum = ext.quorumThreshold();
            auto const commitQuorum = ext.hasQuorumOfCommits();
            auto const minReveals = ext.hasMinimumReveals();
            auto const anyReveals = ext.hasAnyReveals();
            auto const reveals = ext.pendingRevealCount();
            auto const likelyParticipants = ext.expectedProposerCount();

            JLOG(ext.j_.debug())
                << "STALLDIAG: rng-counters"
                << " commits=" << commits << " quorum=" << quorum
                << " commitQuorum=" << (commitQuorum ? "yes" : "no")
                << " reveals=" << std::to_string(reveals)
                << " minReveals=" << (minReveals ? "yes" : "no")
                << " anyReveals=" << (anyReveals ? "yes" : "no")
                << " likelyParticipants=" << std::to_string(likelyParticipants);

            if constexpr (requires {
                              ext.observedParticipantCount();
                              ext.observedParticipantsHash();
                              ext.observedParticipantsBitmapBin();
                          })
            {
                auto const observedHash = ext.observedParticipantsHash();
                JLOG(ext.j_.debug())
                    << "STALLDIAG: participant-diagnostics"
                    << " observedActiveParticipants="
                    << ext.observedParticipantCount()
                    << " observedParticipantsHash="
                    << (observedHash ? to_string(*observedHash)
                                     : std::string{"none"})
                    << " bitmapBin=" << ext.observedParticipantsBitmapBin();
            }
        };
        auto publishEntropySet = [&]() {
            auto entropySetHash = ext.buildEntropySet(buildSeq);
            auto newPos = ctx.getPosition();
            if constexpr (requires { ext.testSuppressEntropySetHash(); })
            {
                if (ext.testSuppressEntropySetHash())
                {
                    if (newPos.entropySetHash)
                    {
                        newPos.entropySetHash.reset();
                        ctx.updatePosition(newPos);

                        if (ctx.mode == ConsensusMode::proposing)
                            ctx.propose();
                    }

                    JLOG(ext.j_.debug())
                        << "RNG: withholding entropySetHash"
                        << " reason=test-suppress-entropy-set-hash"
                        << " buildSeq=" << buildSeq
                        << " hash=" << entropySetHash;
                    return;
                }
            }

            if (newPos.entropySetHash &&
                *newPos.entropySetHash == entropySetHash)
            {
                JLOG(ext.j_.debug())
                    << "RNG: entropySet already published"
                    << " buildSeq=" << buildSeq << " hash=" << entropySetHash;
                return;
            }

            newPos.entropySetHash = entropySetHash;

            ctx.updatePosition(newPos);

            // Publish entropySetHash before accepting so tx-converged peers can
            // observe the proposed local snapshot root before the gate decides.
            //
            // This can look redundant in healthy rounds because txSetHash
            // may be unchanged versus the prior proposal (for example,
            // seq=2 and seq=3 showing the same tx summary in monitors). We
            // still publish to create an additional delivery window for
            // entropySetHash observation.
            if (ctx.mode == ConsensusMode::proposing)
                ctx.propose();

            JLOG(ext.j_.debug())
                << "RNG: published entropySet"
                << " buildSeq=" << buildSeq << " hash=" << entropySetHash
                << " proposing="
                << (ctx.mode == ConsensusMode::proposing ? "yes" : "no");
        };

        JLOG(ext.j_.trace())
            << "RNG: phaseEstablish"
            << " buildSeq=" << buildSeq << " estState=" << estStateName()
            << " roundMs=" << ctx.roundTime.count()
            << " mode=" << to_string(ctx.mode);

        if (ext.estState_ == EstablishState::ConvergingTx)
        {
            // Commit quorum is fixed to the active UNL snapshot for the round.
            // We move immediately once that floor is met; recent-proposer and
            // peer-visibility counts are diagnostics only.
            if (ext.hasQuorumOfCommits())
            {
                //@@start rng-commit-quorum-transition
                auto commitSetHash = ext.buildCommitSet(buildSeq);

                // Keep the same entropy secret from onClose() — do NOT
                // regenerate.  The commitment in the commitSet was built
                // from that original secret; regenerating would make the
                // later reveal fail verification.
                auto newPos = ctx.getPosition();
                newPos.commitSetHash = commitSetHash;

                ctx.updatePosition(newPos);

                if (ctx.mode == ConsensusMode::proposing)
                    ctx.propose();

                ext.estState_ = EstablishState::ConvergingCommit;
                ext.commitHashConflictStart_ = {};
                JLOG(ext.j_.debug()) << "RNG: transitioned to ConvergingCommit"
                                     << " buildSeq=" << buildSeq
                                     << " commitSetHash=" << commitSetHash
                                     << " commits=" << ext.pendingCommitCount()
                                     << " quorum=" << ext.quorumThreshold();
                //@@end rng-commit-quorum-transition
                return {};  // Wait for next tick
            }

            // Don't let the round close while waiting for commit quorum.
            // Without this gate, execution falls through to the normal
            // consensus close logic and nodes inject different entropy tiers
            // while others are still collecting — causing ledger
            // mismatches.
            //
            // Local proposer counts are not stable consensus inputs.  They may
            // tell us whether to keep waiting, but they must not make this node
            // close with fallback while another node has quorum sidecar
            // material for the same parent/base transaction set.
            //@@start rng-commit-timeout-degrade
            bool timeout = ctx.roundTime > ctx.parms.rngPIPELINE_TIMEOUT;
            if (!timeout)
            {
                logRngDiag("rng-commit-wait");
                return {};  // Wait for more commits
            }

            // Timeout waiting for additional likely participants.  If the
            // proofed commit set already meets the entropy gate threshold
            // (the lowest accepted tier's bar — min(quorum, tier2)), proceed
            // with what we have; otherwise this round degrades to fallback.
            auto const commits = ext.proofedCommitCount();
            auto const quorum = ext.entropyGateThreshold();
            if (commits >= quorum)
            {
                JLOG(ext.j_.info())
                    << "RNG: commit timeout with entropy gate threshold"
                    << " buildSeq=" << buildSeq << " commits=" << commits
                    << " entropyGateThreshold=" << quorum
                    << " roundMs=" << ctx.roundTime.count()
                    << " timeoutMs=" << ctx.parms.rngPIPELINE_TIMEOUT.count();
                // Jump to the same path as ext.hasQuorumOfCommits.
                auto commitSetHash = ext.buildCommitSet(buildSeq);
                auto newPos = ctx.getPosition();
                newPos.commitSetHash = commitSetHash;
                ctx.updatePosition(newPos);
                if (ctx.mode == ConsensusMode::proposing)
                    ctx.propose();
                ext.estState_ = EstablishState::ConvergingCommit;
                ext.commitHashConflictStart_ = {};
                JLOG(ext.j_.debug())
                    << "RNG: transitioned to ConvergingCommit"
                    << " reason=timeout-with-quorum" << " buildSeq=" << buildSeq
                    << " commitSetHash=" << commitSetHash
                    << " commits=" << commits << " quorum=" << quorum;
                return {};
            }
            logRngDiag("rng-commit-timeout-below-quorum");
            // Truly below the entropy gate: fall through to
            // consensus_fallback entropy.
            //@@end rng-commit-timeout-degrade
        }
        else if (ext.estState_ == EstablishState::ConvergingCommit)
        {
            // Fast path: if no commit-set conflicts are observed, do
            // exactly what we did before (immediate reveal transition).
            //
            // Safety path: haveConsensus() only compares tx-set hash, not
            // RNG sidecar fields. So commitSetHash disagreements can exist
            // transiently even while tx consensus is true. We only add
            // delay when we *actually* observe conflicting non-empty
            // commitSetHash values among tx-converged positions.

            // --- hasConflictingCommitSetHashes logic (inlined) ---
            auto hasConflictingCommitSetHashes = [&]() -> bool {
                auto const ourPos = ctx.getPosition();
                std::optional<uint256> observed;

                auto note = [&](auto const& pos) -> bool {
                    if (!pos.commitSetHash)
                        return false;
                    if (!observed)
                    {
                        observed = *pos.commitSetHash;
                        return false;
                    }
                    return *observed != *pos.commitSetHash;
                };

                if (note(ourPos))
                    return true;

                for (auto const& [nodeId, peerPos] : ctx.peerPositions)
                {
                    auto const& peerPosition = peerPos.proposal().position();
                    if (peerPosition.txSetHash != ourPos.txSetHash)
                        continue;
                    if (note(peerPosition))
                        return true;
                }
                return false;
            };

            if (hasConflictingCommitSetHashes())
            {
                // Proposal harvest may have added commits since we last
                // published our commitSetHash. Rebuild and re-publish so
                // peers can converge on one deterministic hash instead of
                // timing out.
                auto pos = ctx.getPosition();
                auto const previousHash = pos.commitSetHash;
                auto const refreshedHash = ext.buildCommitSet(buildSeq);
                if (!previousHash || *previousHash != refreshedHash)
                {
                    pos.commitSetHash = refreshedHash;
                    ctx.updatePosition(pos);

                    if (ctx.mode == ConsensusMode::proposing)
                        ctx.propose();

                    JLOG(ext.j_.debug())
                        << "RNG: refreshed commitSetHash"
                        << " reason=proposal-harvest"
                        << " buildSeq=" << buildSeq << " oldHash="
                        << (previousHash ? to_string(*previousHash)
                                         : std::string{"none"})
                        << " newHash=" << refreshedHash;
                }

                // Re-check after refreshing our own hash.
                if (hasConflictingCommitSetHashes())
                {
                    auto const nowSteady = ctx.nowSteady;
                    if (ext.commitHashConflictStart_ ==
                        std::chrono::steady_clock::time_point{})
                    {
                        // First observed conflict: start a bounded grace
                        // window so benign proposal ordering can settle.
                        ext.commitHashConflictStart_ = nowSteady;
                        JLOG(ext.j_.warn())
                            << "RNG: conflicting commitSetHash detected"
                            << " buildSeq=" << buildSeq << " deadlineMs="
                            << toMs(ctx.parms.rngREVEAL_TIMEOUT)
                            << " action=wait-for-proposal-alignment";
                        logRngDiag("rng-commit-conflict-start");
                        return {};
                    }

                    auto const conflictElapsed =
                        nowSteady - ext.commitHashConflictStart_;
                    if (conflictElapsed <= ctx.parms.rngREVEAL_TIMEOUT)
                    {
                        // We are still inside the grace window, so keep
                        // waiting. This preserves the fast path when peers
                        // converge after a short delay.
                        JLOG(ext.j_.debug())
                            << "RNG: commitSetHash conflict wait"
                            << " buildSeq=" << buildSeq
                            << " elapsedMs=" << toMs(conflictElapsed)
                            << " deadlineMs="
                            << toMs(ctx.parms.rngREVEAL_TIMEOUT)
                            << " state=ConvergingCommit";
                        logRngDiag("rng-commit-conflict-wait");
                        return {};
                    }

                    // If conflict persists past a bounded wait, stop waiting
                    // in the commit sub-state. The reveal gate still decides
                    // the accepted root; selection falls back only if no
                    // accepted root materializes.
                    ext.setEntropyFailed();
                    ext.freezeRngCommitSet();
                    ext.estState_ = EstablishState::ConvergingReveal;
                    // Backdate ext.revealPhaseStart_ so the ConvergingReveal
                    // timeout path fires immediately next tick.
                    ext.revealPhaseStart_ = nowSteady -
                        ctx.parms.rngREVEAL_TIMEOUT -
                        std::chrono::milliseconds{1};
                    ext.commitHashConflictStart_ = {};
                    JLOG(ext.j_.warn())
                        << "RNG: commitSetHash conflict timeout"
                        << " buildSeq=" << buildSeq
                        << " elapsedMs=" << toMs(conflictElapsed)
                        << " deadlineMs=" << toMs(ctx.parms.rngREVEAL_TIMEOUT)
                        << " action=advance-to-reveal-gate";
                    logRngDiag("rng-commit-conflict-timeout-advance");
                    return {};
                }
            }

            ext.commitHashConflictStart_ = {};

            //@@start rng-reveal-transition
            auto newPos = ctx.getPosition();
            newPos.myReveal = ext.getEntropySecret();
            ext.freezeRngCommitSet();

            // Self-seed our own reveal into pendingReveals so it
            // counts toward reveal quorum and appears in the
            // entropy set.  harvestRngData only sees peer proposals,
            // not our own.
            ext.selfSeedReveal();

            ctx.updatePosition(newPos);

            if (ctx.mode == ConsensusMode::proposing)
                ctx.propose();

            ext.estState_ = EstablishState::ConvergingReveal;
            //@@end rng-reveal-transition
            ext.revealPhaseStart_ = ctx.nowSteady;
            JLOG(ext.j_.debug()) << "RNG: transitioned to ConvergingReveal"
                                 << " buildSeq=" << buildSeq
                                 << " reveal=" << ext.getEntropySecret()
                                 << " reveals=" << ext.pendingRevealCount();

            // Fast path:
            // If all required reveals are already present at transition
            // time, publish entropySet immediately and finish in this timer
            // pass. This is state-based (reveal completeness), not tied to
            // any particular proposal sequence number.
            if (ext.hasMinimumReveals())
            {
                publishEntropySet();
                ext.entropySetPublished_ = true;
                ext.entropyPublishStart_ = ctx.nowSteady;
                JLOG(ext.j_.debug()) << "RNG: fast-path published entropySet"
                                     << " buildSeq=" << buildSeq
                                     << " action=wait-for-peer-observation";
                logRngDiag("rng-reveal-fast-path-entropy-published-wait");
                return {};
            }
            else
            {
                logRngDiag("rng-reveal-wait-after-transition");
                return {};  // Wait for next tick
            }
        }
        else if (ext.estState_ == EstablishState::ConvergingReveal)
        {
            //@@start rng-reveal-publish-gate
            // Wait for ALL committers to reveal (not just 80%).
            // Timeout measured from ConvergingReveal entry, not round
            // start.
            auto const elapsed = ctx.nowSteady - ext.revealPhaseStart_;
            bool timeout = elapsed > ctx.parms.rngREVEAL_TIMEOUT;
            bool ready = false;
            bool const revealConsensus =
                ctx.haveConsensus() && ext.hasMinimumReveals();

            if (revealConsensus || timeout)
            {
                JLOG(ext.j_.debug())
                    << "STALLDIAG: rng-reveal-gate-open"
                    << " revealConsensus=" << (revealConsensus ? "yes" : "no")
                    << " timeout=" << (timeout ? "yes" : "no")
                    << " elapsedMs=" << toMs(elapsed)
                    << " deadlineMs=" << toMs(ctx.parms.rngREVEAL_TIMEOUT)
                    << " buildSeq=" << buildSeq;
                if (timeout && !ext.hasAnyReveals())
                {
                    ext.setEntropyFailed();
                    JLOG(ext.j_.warn())
                        << "RNG: entropy failed"
                        << " reason=no-reveals"
                        << " buildSeq=" << buildSeq
                        << " elapsedMs=" << toMs(elapsed)
                        << " deadlineMs=" << toMs(ctx.parms.rngREVEAL_TIMEOUT);
                    logRngDiag("rng-reveal-timeout-no-reveals");
                }
                else
                {
                    publishEntropySet();
                    logRngDiag("rng-reveal-published-entropy-set");
                }
                ready = true;
            }

            if (!ready)
            {
                JLOG(ext.j_.debug())
                    << "STALLDIAG: rng-reveal-gate-blocked"
                    << " revealConsensus=" << (revealConsensus ? "yes" : "no")
                    << " timeout=" << (timeout ? "yes" : "no")
                    << " elapsedMs=" << toMs(elapsed)
                    << " deadlineMs=" << toMs(ctx.parms.rngREVEAL_TIMEOUT)
                    << " buildSeq=" << buildSeq;
                logRngDiag("rng-reveal-wait");
                return {};
            }
            //@@end rng-reveal-publish-gate

            // --- EntropySetHash convergence gate ---
            //
            // After publishing our entropySet, ensure peers have had
            // at least one observation window to see our hash (and us
            // theirs) before accepting.  Without this, a node can
            // publish + accept in the same tick, never seeing a peer's
            // different hash — causing asymmetric validator/fallback entropy
            // and a ledger fork.
            //
            // The gate works in two phases:
            //   1. First tick after publishing: always wait (return {})
            //      to give proposals time to propagate.
            //   2. Subsequent ticks: check for conflict and rebuild if needed,
            //      bounded by deadline.
            //
            // Same pattern as commitSetHash conflict handling (line ~308)
            // and exportSigSetHash convergence gate (line ~674).
            {
                auto const ourPos = ctx.getPosition();
                if (ourPos.entropySetHash)
                {
                    // Phase 1: on the tick we first published, always
                    // wait one more tick for observation.
                    if (!ext.entropySetPublished_)
                    {
                        ext.entropySetPublished_ = true;
                        ext.entropyPublishStart_ = ctx.nowSteady;
                        JLOG(ext.j_.debug())
                            << "RNG: entropySet first published"
                            << " buildSeq=" << buildSeq
                            << " hash=" << *ourPos.entropySetHash
                            << " action=wait-for-peer-observation";
                        logRngDiag("rng-entropy-hash-first-publish-wait");
                        return {};
                    }

                    // Phase 2: check peer agreement.  Extension hashes do not
                    // participate in tx-set equality, so a different
                    // entropySetHash is an RNG-side disagreement to resolve or
                    // zero out, not something that should block ordinary
                    // tx-set consensus indefinitely.
                    //@@start rng-entropy-observation-state
                    auto inspectEntropyPeers = [&](auto const& pos) {
                        return detail::inspectTxConvergedSidecarPeers(
                            ctx.peerPositions,
                            pos,
                            ext.localIsActiveValidator(),
                            [](auto const& position) {
                                return position.entropySetHash;
                            },
                            [&ext](auto const& nodeId) {
                                return ext.isUNLReportMember(nodeId);
                            },
                            [](auto const&) {});
                    };

                    auto entropyState = inspectEntropyPeers(ourPos);
                    auto const entropyQuorum = ext.entropyGateThreshold();
                    auto quorumAligned = [&] {
                        return entropyState.quorumAligned(entropyQuorum);
                    };
                    auto fullObservation = [&] {
                        return entropyState.fullObservation();
                    };
                    auto clearEntropyHash = [&] {
                        auto failedPos = ctx.getPosition();
                        ext.clearAcceptedEntropySet();
                        if (!failedPos.entropySetHash)
                            return;
                        failedPos.entropySetHash.reset();
                        ctx.updatePosition(failedPos);
                        if (ctx.mode == ConsensusMode::proposing)
                            ctx.propose();
                    };
                    //@@end rng-entropy-observation-state

                    if (entropyState.conflict && !quorumAligned())
                    {
                        // Rebuild our entropy set after proposal harvest
                        // updates.
                        auto const refreshedHash =
                            ext.buildEntropySet(buildSeq);
                        if (refreshedHash != *ourPos.entropySetHash)
                        {
                            auto newPos = ctx.getPosition();
                            newPos.entropySetHash = refreshedHash;
                            ctx.updatePosition(newPos);
                            if (ctx.mode == ConsensusMode::proposing)
                                ctx.propose();
                            JLOG(ext.j_.debug())
                                << "RNG: refreshed entropySetHash"
                                << " reason=local-refresh"
                                << " buildSeq=" << buildSeq
                                << " oldHash=" << *ourPos.entropySetHash
                                << " newHash=" << refreshedHash;
                        }

                        // Re-check against the current local hash.  Any peer
                        // that still advertises a different entropySetHash is
                        // unresolved until it converges or the bounded RNG
                        // window expires and forces consensus_fallback.
                        entropyState = inspectEntropyPeers(ctx.getPosition());
                    }

                    //@@start rng-entropy-conflict-gate
                    if (entropyState.conflict && quorumAligned())
                    {
                        // Safety here is the fixed-denominator intersection
                        // invariant in entropyGateThreshold(): any two
                        // quorum-aligned entropy cohorts must share an honest
                        // validator. fullObservation is intentionally not the
                        // equivocation backstop; it would only reintroduce a
                        // liveness veto by nodes that withhold observations.
                        JLOG(ext.j_.debug())
                            << "RNG: entropySetHash conflict ignored"
                            << " reason=quorum-aligned"
                            << " buildSeq=" << buildSeq
                            << " alignedParticipants="
                            << entropyState.alignedParticipants()
                            << " quorum=" << entropyQuorum
                            << " peersSeen=" << entropyState.peersSeen
                            << " txConverged=" << entropyState.txConverged;
                    }
                    else if (entropyState.conflict)
                    {
                        // Bounded grace window for unresolved entropy-side
                        // conflicts that have not reached the quorum-aligned
                        // threshold.
                        auto const entropyElapsed =
                            ctx.nowSteady - ext.entropyPublishStart_;
                        auto const entropyDeadline =
                            detail::sidecarConvergenceTimeout(ctx.parms);
                        if (entropyElapsed <= entropyDeadline)
                        {
                            JLOG(ext.j_.debug())
                                << "RNG: entropySetHash conflict wait"
                                << " buildSeq=" << buildSeq
                                << " elapsedMs=" << toMs(entropyElapsed)
                                << " deadlineMs=" << toMs(entropyDeadline)
                                << " alignedParticipants="
                                << entropyState.alignedParticipants()
                                << " quorum=" << entropyQuorum
                                << " peersSeen=" << entropyState.peersSeen
                                << " txConverged=" << entropyState.txConverged;
                            logRngDiag("rng-entropy-hash-conflict-wait");
                            return {};
                        }

                        // Deadline exceeded — fall back to consensus_fallback.
                        ext.setEntropyFailed();
                        clearEntropyHash();
                        JLOG(ext.j_.warn())
                            << "RNG: entropySetHash conflict timeout"
                            << " buildSeq=" << buildSeq
                            << " elapsedMs=" << toMs(entropyElapsed)
                            << " deadlineMs=" << toMs(entropyDeadline)
                            << " action=consensus-fallback"
                            << " alignedParticipants="
                            << entropyState.alignedParticipants()
                            << " entropyGateThreshold=" << entropyQuorum
                            << " peersSeen=" << entropyState.peersSeen
                            << " txConverged=" << entropyState.txConverged;
                        logRngDiag("rng-entropy-hash-conflict-timeout");
                    }
                    //@@end rng-entropy-conflict-gate

                    // Positive alignment check: silence is not a conflicting
                    // entropy value. A quorum-aligned clean hash can proceed
                    // without waiting for every tx-converged validator to
                    // advertise; otherwise one silent validator gets a free
                    // RNG off-switch. Observed minority conflicts use the same
                    // quorum-aligned threshold: intersection math gives a
                    // single aligned winner, while validation resolves the
                    // bounded deadline edge.
                    //@@start rng-entropy-positive-alignment-gate
                    if (!entropyState.conflict && !quorumAligned())
                    {
                        auto const entropyElapsed =
                            ctx.nowSteady - ext.entropyPublishStart_;
                        auto const entropyDeadline =
                            detail::sidecarConvergenceTimeout(ctx.parms);
                        if (entropyElapsed <= entropyDeadline)
                        {
                            JLOG(ext.j_.debug())
                                << "RNG: waiting for entropySetHash entropy "
                                   "gate alignment"
                                << " buildSeq=" << buildSeq
                                << " alignedParticipants="
                                << entropyState.alignedParticipants()
                                << " entropyGateThreshold=" << entropyQuorum
                                << " peersSeen=" << entropyState.peersSeen
                                << " txConverged=" << entropyState.txConverged
                                << " elapsedMs=" << toMs(entropyElapsed)
                                << " deadlineMs=" << toMs(entropyDeadline);
                            logRngDiag("rng-entropy-hash-quorum-wait");
                            return {};
                        }
                        ext.setEntropyFailed();
                        clearEntropyHash();
                        JLOG(ext.j_.warn())
                            << "RNG: entropySetHash entropy gate alignment "
                               "timeout"
                            << " buildSeq=" << buildSeq
                            << " action=consensus-fallback"
                            << " alignedParticipants="
                            << entropyState.alignedParticipants()
                            << " entropyGateThreshold=" << entropyQuorum
                            << " peersSeen=" << entropyState.peersSeen
                            << " txConverged=" << entropyState.txConverged
                            << " elapsedMs=" << toMs(entropyElapsed)
                            << " deadlineMs=" << toMs(entropyDeadline);
                        logRngDiag("rng-entropy-hash-quorum-timeout");
                    }
                    else if (
                        !entropyState.conflict && quorumAligned() &&
                        !fullObservation())
                    {
                        JLOG(ext.j_.info())
                            << "RNG: missing entropySetHash observation "
                               "ignored"
                            << " reason=quorum-aligned"
                            << " buildSeq=" << buildSeq
                            << " alignedParticipants="
                            << entropyState.alignedParticipants()
                            << " quorum=" << entropyQuorum
                            << " peersSeen=" << entropyState.peersSeen
                            << " txConverged=" << entropyState.txConverged;
                    }
                    //@@end rng-entropy-positive-alignment-gate

                    JLOG(ext.j_.debug())
                        << "RNG: entropy gate"
                        << " buildSeq=" << buildSeq
                        << " aligned=" << entropyState.aligned
                        << " alignedParticipants="
                        << entropyState.alignedParticipants()
                        << " entropyGateThreshold=" << entropyQuorum
                        << " peersSeen=" << entropyState.peersSeen
                        << " txConverged=" << entropyState.txConverged
                        << " conflict="
                        << (entropyState.conflict ? "yes" : "no");

                    if (auto const accepted = ctx.getPosition().entropySetHash)
                        ext.acceptEntropySet(*accepted);
                }
            }
        }
    }
    else
    {
        JLOG(ext.j_.debug())
            << "RNGGATE: skipping RNG substates"
            << " buildSeq=" << ctx.buildSeq
            << " prevSeq=" << (static_cast<std::uint32_t>(ctx.buildSeq) - 1)
            << " mode=" << to_string(ctx.mode);
    }

    // Export sig convergence gate: runs after RNG sub-states when Export has
    // verified signatures to publish or when tx-converged peers advertise
    // exportSigSetHash roots we can locally materialize. This is a bounded
    // safety coordination window, not a wait-for-Export-success mechanism.
    if constexpr (requires { ctx.getPosition().exportSigSetHash; })
    {
        if (!ext.exportEnabled())
            return {.readyForAccept = true};

        auto startExportSigGate = [&]() -> bool {
            if (ext.exportSigGateStarted_)
                return false;
            ext.exportSigGateStarted_ = true;
            ext.exportSigGateStart_ = ctx.nowSteady;
            return true;
        };

        auto observedPeerExportSigSets = [&](auto const& pos) {
            std::size_t peerSets = 0;
            for (auto const& [_, peerPos] : ctx.peerPositions)
            {
                auto const& pp = peerPos.proposal().position();
                if (positionTxSetID(pp) != positionTxSetID(pos))
                    continue;  // not tx-converged
                if (!pp.exportSigSetHash)
                    continue;

                ++peerSets;
            }
            return peerSets;
        };

        bool hasLocalExportSigs = ext.hasPendingExportSigs();
        //@@start export-sigset-material-wait
        if (!hasLocalExportSigs && ext.hasEligiblePendingExports())
        {
            auto const peerSets = observedPeerExportSigSets(ctx.getPosition());
            if (peerSets > 0)
            {
                startExportSigGate();
                hasLocalExportSigs = ext.hasPendingExportSigs();
                if (!hasLocalExportSigs)
                {
                    auto const elapsed =
                        ctx.nowSteady - ext.exportSigGateStart_;
                    auto const deadline =
                        detail::sidecarConvergenceTimeout(ctx.parms);
                    if (elapsed <= deadline)
                    {
                        JLOG(ext.j_.debug())
                            << "Export: bounded wait for advertised "
                               "exportSigSet local material"
                            << " buildSeq=" << ctx.buildSeq
                            << " peerSets=" << peerSets
                            << " elapsedMs=" << toMs(elapsed)
                            << " deadlineMs=" << toMs(deadline);
                        return {};
                    }

                    ext.setExportSigConvergenceFailed();
                    ext.clearAcceptedExportSigSet();
                    JLOG(ext.j_.warn())
                        << "Export: advertised exportSigSet material timeout"
                        << " buildSeq=" << ctx.buildSeq
                        << " peerSets=" << peerSets
                        << " elapsedMs=" << toMs(elapsed)
                        << " deadlineMs=" << toMs(deadline)
                        << " action=wait-or-expire";
                }
            }
            else
            {
                // A pending Export latch with no local sig material gets one
                // short observation window so proposal-carried signatures can
                // arrive. If nothing appears in time, this ledger injects no
                // witness and the latch remains pending.
                startExportSigGate();
                auto const elapsed = ctx.nowSteady - ext.exportSigGateStart_;
                auto const deadline =
                    detail::sidecarConvergenceTimeout(ctx.parms);
                if (elapsed <= deadline)
                {
                    JLOG(ext.j_.debug())
                        << "Export: bounded wait for exportSigSet "
                           "advertisement"
                        << " buildSeq=" << ctx.buildSeq
                        << " elapsedMs=" << toMs(elapsed)
                        << " deadlineMs=" << toMs(deadline)
                        << " candidateExportTxns=yes";
                    return {};
                }

                ext.setExportSigConvergenceFailed();
                ext.clearAcceptedExportSigSet();
                JLOG(ext.j_.warn())
                    << "Export: exportSigSet advertisement timeout"
                    << " buildSeq=" << ctx.buildSeq
                    << " elapsedMs=" << toMs(elapsed)
                    << " deadlineMs=" << toMs(deadline)
                    << " action=wait-or-expire";
            }
        }
        //@@end export-sigset-material-wait

        if (hasLocalExportSigs)
        {
            //@@start export-publish-sigset-hash
            auto const buildSeqExport = ctx.buildSeq;
            auto const exportHash = ext.buildExportSigSet(buildSeqExport);

            auto currentPos = ctx.getPosition();
            bool publishedNewHash = false;
            if (ext.testSuppressExportSigSetHash())
            {
                if (currentPos.exportSigSetHash)
                {
                    currentPos.exportSigSetHash.reset();
                    ctx.updatePosition(currentPos);

                    if (ctx.mode == ConsensusMode::proposing)
                        ctx.propose();
                }

                JLOG(ext.j_.debug())
                    << "Export: withholding exportSigSetHash"
                    << " reason=runtime-config-noExportSigHash"
                    << " buildSeq=" << buildSeqExport << " hash=" << exportHash;
            }
            else
            {
                publishedNewHash = !currentPos.exportSigSetHash ||
                    *currentPos.exportSigSetHash != exportHash;
                if (publishedNewHash)
                {
                    currentPos.exportSigSetHash = exportHash;
                    ctx.updatePosition(currentPos);

                    if (ctx.mode == ConsensusMode::proposing)
                        ctx.propose();

                    JLOG(ext.j_.debug()) << "Export: published exportSigSetHash"
                                         << " buildSeq=" << buildSeqExport
                                         << " hash=" << exportHash;
                }
            }
            //@@end export-publish-sigset-hash

            //@@start export-sigset-conflict-wait
            // Check quorum agreement on exportSigSetHash. Like RNG entropy,
            // Export success is an accept-time derived effect outside tx-set
            // equality. A local-only quorum must not succeed unless enough
            // tx-converged peers advertise the same export sig sidecar hash.
            {
                if (startExportSigGate() || publishedNewHash)
                {
                    JLOG(ext.j_.debug()) << "Export: exportSigSet published"
                                         << " buildSeq=" << buildSeqExport
                                         << " hash=" << exportHash
                                         << " action=wait-for-peer-observation";
                    return {};
                }

                auto inspectExportPeers = [&](auto const& pos) {
                    return detail::inspectTxConvergedSidecarPeers(
                        ctx.peerPositions,
                        pos,
                        ext.localIsActiveValidator(),
                        [](auto const& position) {
                            return position.exportSigSetHash;
                        },
                        [&ext](auto const& nodeId) {
                            return ext.isUNLReportMember(nodeId);
                        },
                        [](auto const&) {});
                };

                auto exportState = inspectExportPeers(ctx.getPosition());
                auto const exportQuorum = ext.exportRootAlignmentThreshold();
                auto quorumAligned = [&] {
                    return exportState.quorumAligned(exportQuorum);
                };
                bool acceptedExportSigHash = false;
                //@@start export-sigset-alignment-check
                if (exportState.conflict && !quorumAligned())
                {
                    auto const refreshedHash =
                        ext.buildExportSigSet(buildSeqExport);
                    auto current = ctx.getPosition();
                    if (!current.exportSigSetHash ||
                        *current.exportSigSetHash != refreshedHash)
                    {
                        auto const oldHash = current.exportSigSetHash;
                        current.exportSigSetHash = refreshedHash;
                        ctx.updatePosition(current);
                        if (ctx.mode == ConsensusMode::proposing)
                            ctx.propose();
                        JLOG(ext.j_.debug())
                            << "Export: refreshed exportSigSetHash"
                            << " reason=local-refresh"
                            << " buildSeq=" << buildSeqExport << " oldHash="
                            << (oldHash ? to_string(*oldHash)
                                        : std::string{"none"})
                            << " newHash=" << refreshedHash;
                    }

                    exportState = inspectExportPeers(ctx.getPosition());
                }
                //@@end export-sigset-alignment-check

                //@@start export-no-veto-quorum-branches
                if (exportState.conflict && quorumAligned())
                {
                    // Export sidecar roots are signed through ExtendedPosition
                    // whenever featureExport is active. A quorum-aligned hash
                    // is therefore enough to proceed; requiring every
                    // tx-converged active peer to publish an exportSigSetHash
                    // would let a missing minority sidecar suppress witness
                    // injection despite an aligned quorum.
                    JLOG(ext.j_.info())
                        << "Export: exportSigSetHash conflict ignored"
                        << " reason=quorum-aligned"
                        << " buildSeq=" << buildSeqExport
                        << " alignedParticipants="
                        << exportState.alignedParticipants()
                        << " quorum=" << exportQuorum
                        << " peersSeen=" << exportState.peersSeen
                        << " txConverged=" << exportState.txConverged;
                    acceptedExportSigHash = true;
                }
                else if (quorumAligned() && !exportState.fullObservation())
                {
                    JLOG(ext.j_.info())
                        << "Export: missing exportSigSetHash observation "
                           "ignored"
                        << " reason=quorum-aligned"
                        << " buildSeq=" << buildSeqExport
                        << " alignedParticipants="
                        << exportState.alignedParticipants()
                        << " quorum=" << exportQuorum
                        << " peersSeen=" << exportState.peersSeen
                        << " txConverged=" << exportState.txConverged;
                    acceptedExportSigHash = true;
                }
                //@@end export-no-veto-quorum-branches
                else if (exportState.conflict || !quorumAligned())
                {
                    auto const elapsed =
                        ctx.nowSteady - ext.exportSigGateStart_;
                    auto const deadline =
                        detail::sidecarConvergenceTimeout(ctx.parms);
                    if (elapsed <= deadline)
                    {
                        JLOG(ext.j_.debug())
                            << "Export: waiting for exportSigSet quorum "
                               "alignment"
                            << " buildSeq=" << buildSeqExport
                            << " alignedParticipants="
                            << exportState.alignedParticipants()
                            << " quorum=" << exportQuorum
                            << " peersSeen=" << exportState.peersSeen
                            << " txConverged=" << exportState.txConverged
                            << " conflict="
                            << (exportState.conflict ? "yes" : "no")
                            << " elapsedMs=" << toMs(elapsed)
                            << " deadlineMs=" << toMs(deadline);
                        return {};
                    }

                    ext.setExportSigConvergenceFailed();
                    ext.clearAcceptedExportSigSet();
                    JLOG(ext.j_.warn())
                        << "Export: exportSigSet quorum alignment timeout"
                        << " buildSeq=" << buildSeqExport
                        << " action=wait-or-expire"
                        << " alignedParticipants="
                        << exportState.alignedParticipants()
                        << " quorum=" << exportQuorum
                        << " peersSeen=" << exportState.peersSeen
                        << " txConverged=" << exportState.txConverged
                        << " conflict=" << (exportState.conflict ? "yes" : "no")
                        << " elapsedMs=" << toMs(elapsed)
                        << " deadlineMs=" << toMs(deadline);
                }
                else
                {
                    acceptedExportSigHash = true;
                }

                if (acceptedExportSigHash)
                {
                    // Apply must consume exactly the sidecar root that passed
                    // the export gate. Local collector state may continue to
                    // grow after this point, but it is not part of the agreed
                    // closed-ledger export material.
                    if (auto const accepted =
                            ctx.getPosition().exportSigSetHash)
                        ext.acceptExportSigSet(*accepted);
                }
            }
            //@@end export-sigset-conflict-wait
        }
    }

    return {.readyForAccept = true};
}

}  // namespace ripple

#endif
