#ifndef RIPPLE_CONSENSUS_CONSENSUSEXTENSIONSTICK_H_INCLUDED
#define RIPPLE_CONSENSUS_CONSENSUSEXTENSIONSTICK_H_INCLUDED

#include <xrpld/consensus/ConsensusTypes.h>
#include <xrpl/basics/Log.h>

namespace ripple {

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
    //@@start rng-phase-establish-substates
    // --- RNG Sub-state Checkpoints ---
    // These sub-states use union convergence (not avalanche).
    // Commits and reveals arrive piggybacked on proposals, so by the time
    // we reach these checkpoints most data is already collected. The
    // SHAMap fetch/diff/merge in onAcquiredSidecarSet is a safety net
    // for stragglers, not a voting mechanism.
    //
    // Why 80% for commits but 100% for reveals?
    //
    // COMMITS: quorum is based on the active UNL, but we don't know
    // which UNL members are actually online until they propose — and
    // commitments ride on those same proposals.  Chicken-and-egg: we
    // learn who's active by receiving their commits.  80% of the UNL
    // says "we've heard from enough validators, let's go."  The
    // impossible-quorum early-exit handles the case where too few
    // participants exist to ever reach 80%.
    //
    // REVEALS: the commit set is now locked and we know *exactly* who
    // committed.  Every committer broadcasts their reveal immediately.
    // So we wait for ALL of them, with rngREVEAL_TIMEOUT (measured
    // from ConvergingReveal entry) as the safety valve for nodes that
    // crash between commit and reveal.

    bool const isRngEnabled = ext.rngEnabled();

    JLOG(ext.j_.trace()) << "RNGGATE: phaseEstablish prevSeq="
                         << (static_cast<std::uint32_t>(ctx.buildSeq) - 1)
                         << " ext.rngEnabled=" << (isRngEnabled ? "yes" : "no")
                         << " estState=" << static_cast<int>(ext.estState_)
                         << " phase=establish"
                         << " mode=" << to_string(ctx.mode)
                         << " roundMs=" << ctx.roundTime.count();

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
                << " phase=establish"
                << " mode=" << to_string(ctx.mode)
                << " roundMs=" << ctx.roundTime.count()
                << " convergePct=" << ctx.convergePercent
                << " participants=" << participants
                << " peerPositions=" << ctx.peerPositions.size()
                << " prevProposers=" << ctx.prevProposers
                << " explicitFinalSent="
                << (ext.explicitFinalProposalSent_ ? "yes" : "no")
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
        };
        auto publishEntropySet = [&]() {
            auto entropySetHash = ext.buildEntropySet(buildSeq);
            auto newPos = ctx.getPosition();
            if (newPos.entropySetHash &&
                *newPos.entropySetHash == entropySetHash)
            {
                JLOG(ext.j_.debug())
                    << "RNG: entropySet already published hash="
                    << entropySetHash;
                return;
            }

            newPos.entropySetHash = entropySetHash;

            ctx.updatePosition(newPos);

            // Publish entropySetHash before accepting so lagging peers
            // can fetch/merge reveal sets in ConvergingReveal.
            //
            // This can look redundant in healthy rounds because txSetHash
            // may be unchanged versus the prior proposal (for example,
            // seq=2 and seq=3 showing the same tx summary in monitors). We
            // still publish to create an additional delivery window for
            // entropySetHash and to trigger fetch/merge on peers that
            // missed earlier packets.
            if (ctx.mode == ConsensusMode::proposing)
                ctx.propose();

            JLOG(ext.j_.debug()) << "RNG: built entropySet";
        };

        JLOG(ext.j_.trace()) << "RNG: phaseEstablish estState="
                             << static_cast<int>(ext.estState_);

        // Bootstrap fast-path: if the previous round didn't have
        // enough proposers for RNG to have succeeded, the network
        // is still converging.  Skip the entire commit/reveal
        // pipeline — it can only produce zero entropy anyway, but
        // each substate transition and timeout (PIPELINE_TIMEOUT,
        // REVEAL_TIMEOUT, conflict-wait) adds seconds of latency
        // per round that compound across staggered startup.
        //
        // Once prevProposers reaches quorum the pipeline engages
        // normally with all its coordination delays intact.
        bool rngBootstrapSkip = false;
        {
            auto const threshold = ext.quorumThreshold();
            // prevProposers is peer-only. Include our own proposer slot when
            // we are actively proposing, otherwise a 4/5 honest quorum appears
            // as only three previous proposers after one validator diverges.
            auto const previousParticipants = ctx.prevProposers +
                (ctx.mode == ConsensusMode::proposing ? 1 : 0);
            if (previousParticipants < threshold)
            {
                JLOG(ext.j_.debug())
                    << "RNG: bootstrap skip (previousParticipants="
                    << previousParticipants << " < threshold=" << threshold
                    << ", prevProposers=" << ctx.prevProposers << ")";
                rngBootstrapSkip = true;
            }
        }

        if (!rngBootstrapSkip && ext.estState_ == EstablishState::ConvergingTx)
        {
            // Commit quorum is fixed to 80% of the active UNL snapshot for
            // the round. We move immediately once that floor is met;
            // recent-proposer tracking is only for deciding whether more
            // waiting is worthwhile.
            if (ext.hasQuorumOfCommits())
            {
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
                                     << " commitSet=" << commitSetHash;
                return {};  // Wait for next tick
            }

            // Don't let the round close while waiting for commit quorum.
            // Without this gate, execution falls through to the normal
            // consensus close logic and nodes inject partial/zero entropy
            // while others are still collecting — causing ledger
            // mismatches.
            //
            // However, if we've already converged on the txSet (which we
            // have — haveConsensus() passed above) and there aren't enough
            // currently participating validators to ever reach the fixed
            // UNL quorum, skip immediately. With 3 active UNL validators
            // and quorum=3, losing one node means 2/3 commits forever —
            // waiting 3s per round just delays recovery.
            //
            // NOTE: Late-joining nodes (e.g. restarting after a crash)
            // cannot help here.  They enter the round as proposing=false
            // and onClose() skips commitment generation for non-proposers.
            // It takes at least one full round of observing before
            // consensus promotes them to proposing.
            {
                // participants = peers + ourselves
                auto const participants = ctx.peerPositions.size() + 1;
                auto const threshold = ext.quorumThreshold();
                bool const impossible = participants < threshold;

                if (impossible)
                {
                    JLOG(ext.j_.debug())
                        << "RNG: skipping commit wait (participants="
                        << participants << " < threshold=" << threshold << ")";
                    logRngDiag("rng-commit-wait-impossible-quorum");
                    // Fall through to close with zero entropy
                }
                else
                {
                    bool timeout =
                        ctx.roundTime > ctx.parms.rngPIPELINE_TIMEOUT;
                    if (!timeout)
                    {
                        logRngDiag("rng-commit-wait");
                        return {};  // Wait for more commits
                    }

                    // Timeout waiting for additional likely participants.
                    // If we already have the fixed UNL quorum, proceed
                    // with what we have — the SHAMap merge handles any
                    // remaining straggler fuzz for this transition round.
                    auto const commits = ext.pendingCommitCount();
                    auto const quorum = ext.quorumThreshold();
                    if (commits >= quorum)
                    {
                        JLOG(ext.j_.info())
                            << "RNG: commit timeout but have quorum ("
                            << commits << "/" << quorum
                            << "), proceeding with partial set";
                        // Jump to the same path as ext.hasQuorumOfCommits
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
                            << " commitSet=" << commitSetHash
                            << " (timeout fallback)";
                        return {};
                    }
                    logRngDiag("rng-commit-timeout-below-quorum");
                    // Truly below quorum: fall through to zero entropy
                }
            }
        }
        else if (
            !rngBootstrapSkip &&
            ext.estState_ == EstablishState::ConvergingCommit)
        {
            // If commit hashes diverge, we may not receive any additional
            // tx-converged proposals in this state (peers can move to the
            // next ledger quickly, causing prevLedger rejects). In that
            // case, hashes observed during ConvergingTx would never be
            // fetched because fetch is intentionally deferred there.
            //
            // Sweep currently tx-converged peer positions each tick so
            // deferred commitSet hashes still get fetched/merged even
            // without new accepted proposals in ConvergingCommit.
            {
                auto const ourPos = ctx.getPosition();
                for (auto const& [nodeId, peerPos] : ctx.peerPositions)
                {
                    auto const& peerPosition = peerPos.proposal().position();
                    if (!(peerPosition == ourPos))
                        continue;
                    ext.fetchRngSetIfNeeded(
                        peerPosition.commitSetHash, Ext::SidecarKind::commit);
                }
            }

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
                    if (!(peerPosition == ourPos))
                        continue;
                    if (note(peerPosition))
                        return true;
                }
                return false;
            };

            if (hasConflictingCommitSetHashes())
            {
                // Fetch/merge may have added missing commits since we last
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
                        << "RNG: refreshed commitSetHash after merge to "
                        << refreshedHash;
                }

                // Re-check after refreshing our own hash.
                if (hasConflictingCommitSetHashes())
                {
                    auto const nowSteady = ctx.nowSteady;
                    if (ext.commitHashConflictStart_ ==
                        std::chrono::steady_clock::time_point{})
                    {
                        // First observed conflict: start a bounded grace
                        // window so benign ordering/fetch races can settle.
                        ext.commitHashConflictStart_ = nowSteady;
                        JLOG(ext.j_.warn())
                            << "RNG: conflicting commitSetHash detected; "
                               "waiting briefly for convergence/fetch";
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
                            << "RNG: commitSetHash still conflicting after "
                            << std::chrono::duration_cast<
                                   std::chrono::milliseconds>(conflictElapsed)
                                   .count()
                            << "ms; staying in ConvergingCommit";
                        logRngDiag("rng-commit-conflict-wait");
                        return {};
                    }

                    // If conflict persists past a bounded wait, force
                    // deterministic fallback for this round.
                    ext.setEntropyFailed();
                    ext.estState_ = EstablishState::ConvergingReveal;
                    // Backdate ext.revealPhaseStart_ so the ConvergingReveal
                    // timeout path fires immediately next tick.
                    ext.revealPhaseStart_ = nowSteady -
                        ctx.parms.rngREVEAL_TIMEOUT -
                        std::chrono::milliseconds{1};
                    ext.commitHashConflictStart_ = {};
                    JLOG(ext.j_.warn())
                        << "RNG: commitSetHash conflict persisted; forcing "
                           "zero-entropy fallback";
                    logRngDiag("rng-commit-conflict-timeout-fallback");
                    return {};
                }
            }

            ext.commitHashConflictStart_ = {};

            //@@start rng-reveal-transition
            auto newPos = ctx.getPosition();
            newPos.myReveal = ext.getEntropySecret();

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
                                 << " reveal=" << ext.getEntropySecret();

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
                JLOG(ext.j_.debug())
                    << "RNG: fast-path published entropySet, waiting for "
                       "peer observation";
                logRngDiag("rng-reveal-fast-path-entropy-published-wait");
                return {};
            }
            else
            {
                logRngDiag("rng-reveal-wait-after-transition");
                return {};  // Wait for next tick
            }
        }
        else if (
            !rngBootstrapSkip &&
            ext.estState_ == EstablishState::ConvergingReveal)
        {
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
                    << " timeout=" << (timeout ? "yes" : "no") << " elapsedMs="
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                           elapsed)
                           .count();
                if (timeout && !ext.hasAnyReveals())
                {
                    ext.setEntropyFailed();
                    JLOG(ext.j_.warn()) << "RNG: entropy failed (no reveals)";
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
                    << " timeout=" << (timeout ? "yes" : "no") << " elapsedMs="
                    << std::chrono::duration_cast<std::chrono::milliseconds>(
                           elapsed)
                           .count();
                logRngDiag("rng-reveal-wait");
                return {};
            }

            // --- EntropySetHash convergence gate ---
            //
            // After publishing our entropySet, ensure peers have had
            // at least one observation window to see our hash (and us
            // theirs) before accepting.  Without this, a node can
            // publish + accept in the same tick, never seeing a peer's
            // different hash — causing asymmetric zero/non-zero entropy
            // and a ledger fork.
            //
            // The gate works in two phases:
            //   1. First tick after publishing: always wait (return {})
            //      to give proposals time to propagate.
            //   2. Subsequent ticks: check for conflict, fetch/merge/
            //      rebuild if needed, bounded by deadline.
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
                            << "RNG: entropySet first published, waiting "
                               "for peer observation";
                        logRngDiag("rng-entropy-hash-first-publish-wait");
                        return {};
                    }

                    struct EntropyPeerState
                    {
                        bool conflict = false;
                        std::size_t aligned = 0;
                        std::size_t peersSeen = 0;
                        std::size_t txConverged = 0;
                    };

                    // Phase 2: check peer agreement.  Extension hashes do not
                    // participate in tx-set equality, so a different
                    // entropySetHash is an RNG-side disagreement to resolve or
                    // zero out, not something that should block ordinary
                    // tx-set consensus indefinitely.
                    auto inspectEntropyPeers =
                        [&](auto const& pos,
                            bool fetchMismatches) -> EntropyPeerState {
                        EntropyPeerState state;
                        for (auto const& [_, peerPos] : ctx.peerPositions)
                        {
                            auto const& pp = peerPos.proposal().position();
                            if (!(pp == pos))
                                continue;  // not tx-converged
                            ++state.txConverged;
                            if (!pp.entropySetHash)
                                continue;  // peer hasn't published yet
                            ++state.peersSeen;
                            if (*pp.entropySetHash == *pos.entropySetHash)
                            {
                                ++state.aligned;
                                continue;
                            }

                            state.conflict = true;
                            if (fetchMismatches)
                                ext.fetchRngSetIfNeeded(
                                    pp.entropySetHash,
                                    Ext::SidecarKind::reveal);
                        }
                        return state;
                    };

                    auto entropyState = inspectEntropyPeers(ourPos, true);
                    auto const entropyQuorum = ext.quorumThreshold();
                    auto quorumAligned = [&] {
                        return entropyState.aligned + 1 >= entropyQuorum;
                    };
                    auto fullObservation = [&] {
                        // Local quorum alignment is not enough if some
                        // tx-converged peers have not advertised their
                        // entropy sidecar hash yet. Otherwise one node can
                        // accept non-zero from an asymmetric local view while
                        // the rest of the network times out to zero.
                        return entropyState.peersSeen ==
                            entropyState.txConverged;
                    };
                    auto clearEntropyHash = [&] {
                        auto failedPos = ctx.getPosition();
                        if (!failedPos.entropySetHash)
                            return;
                        failedPos.entropySetHash.reset();
                        ctx.updatePosition(failedPos);
                        if (ctx.mode == ConsensusMode::proposing)
                            ctx.propose();
                    };

                    if (entropyState.conflict && !quorumAligned())
                    {
                        // Rebuild our entropy set after any merges.
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
                                << "RNG: refreshed entropySetHash after "
                                   "merge to "
                                << refreshedHash;
                        }

                        // Re-check against the current local hash.  Any peer
                        // that still advertises a different entropySetHash is
                        // unresolved until it converges or the bounded RNG
                        // window expires and forces zero entropy.
                        entropyState =
                            inspectEntropyPeers(ctx.getPosition(), true);
                    }

                    if (entropyState.conflict && quorumAligned() &&
                        fullObservation())
                    {
                        JLOG(ext.j_.debug())
                            << "RNG: entropySetHash conflict ignored after "
                               "quorum alignment"
                            << " alignedParticipants="
                            << (entropyState.aligned + 1)
                            << " quorum=" << entropyQuorum
                            << " peersSeen=" << entropyState.peersSeen
                            << " txConverged=" << entropyState.txConverged;
                    }
                    else if (entropyState.conflict)
                    {
                        // Bounded grace window for unresolved entropy-side
                        // conflicts.
                        auto const entropyElapsed =
                            ctx.nowSteady - ext.entropyPublishStart_;
                        auto const entropyDeadline =
                            ctx.parms.rngREVEAL_TIMEOUT * 2;
                        if (entropyElapsed <= entropyDeadline)
                        {
                            JLOG(ext.j_.debug())
                                << "RNG: entropySetHash conflict, waiting "
                                << std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       entropyElapsed)
                                       .count()
                                << "ms / "
                                << std::chrono::duration_cast<
                                       std::chrono::milliseconds>(
                                       entropyDeadline)
                                       .count()
                                << "ms";
                            logRngDiag("rng-entropy-hash-conflict-wait");
                            return {};
                        }

                        // Deadline exceeded — fall back to zero.
                        ext.setEntropyFailed();
                        clearEntropyHash();
                        JLOG(ext.j_.warn())
                            << "RNG: entropySetHash conflict persisted "
                               "past deadline, falling back to zero "
                               "entropy";
                        logRngDiag("rng-entropy-hash-conflict-timeout");
                    }

                    // Positive alignment check: require at least one
                    // tx-converged quorum with a matching entropySetHash
                    // before accepting non-zero entropy, and require every
                    // tx-converged peer we are counting to have advertised
                    // some entropySetHash.  Without the full-observation
                    // part, asymmetric proposal delivery lets a node accept
                    // non-zero while peers that are still missing sidecar
                    // hashes hit the deadline and deterministically zero.
                    if (!entropyState.conflict &&
                        (!quorumAligned() || !fullObservation()))
                    {
                        auto const entropyElapsed =
                            ctx.nowSteady - ext.entropyPublishStart_;
                        auto const entropyDeadline =
                            ctx.parms.rngREVEAL_TIMEOUT * 2;
                        if (entropyElapsed <= entropyDeadline)
                        {
                            JLOG(ext.j_.debug())
                                << "RNG: waiting for entropySetHash quorum "
                                   "alignment"
                                << " alignedParticipants="
                                << (entropyState.aligned + 1)
                                << " quorum=" << entropyQuorum
                                << " peersSeen=" << entropyState.peersSeen
                                << " txConverged=" << entropyState.txConverged;
                            logRngDiag("rng-entropy-hash-quorum-wait");
                            return {};
                        }
                        ext.setEntropyFailed();
                        clearEntropyHash();
                        JLOG(ext.j_.warn())
                            << "RNG: entropySetHash quorum alignment missing "
                               "within deadline, falling back to zero"
                            << " alignedParticipants="
                            << (entropyState.aligned + 1)
                            << " quorum=" << entropyQuorum
                            << " peersSeen=" << entropyState.peersSeen
                            << " txConverged=" << entropyState.txConverged;
                        logRngDiag("rng-entropy-hash-quorum-timeout");
                    }

                    JLOG(ext.j_.debug())
                        << "RNG: entropy gate — aligned="
                        << entropyState.aligned
                        << " alignedParticipants=" << (entropyState.aligned + 1)
                        << " quorum=" << entropyQuorum
                        << " peersSeen=" << entropyState.peersSeen
                        << " txConverged=" << entropyState.txConverged
                        << " conflict="
                        << (entropyState.conflict ? "yes" : "no");
                }
            }

            // Optional explicit final proposal (seq=4 style):
            // publish a synthetic tx-set hash that includes the
            // consensus-entropy pseudo-tx just before accept.
            //
            // IMPORTANT DESIGN NOTE (read before editing this block):
            //
            // This path is intentionally OPTIONAL and default-off. It
            // exists for diagnostics/perf experiments (for example, making
            // monitor visibility of the final pseudo-tx set more direct),
            // NOT as a required step for consensus correctness.
            //
            // Why so conservative?
            // - The main consensus engine still keys agreement on tx-set
            // hash.
            // - Updating our tx-set hash here creates a "late identity
            //   change" in establish.
            // - Under lossy/reordered networks, peers can be slightly out
            // of
            //   phase: some nodes may have switched to the synthetic hash
            //   while others are still on the base hash.
            // - That can fragment agreement during a critical window (two
            //   hashes in flight for one ledger), increase proposal
            //   chatter, and trigger sync churn.
            //
            // Therefore this logic must remain best-effort only:
            // - Never required for liveness/safety.
            // - No extra wait tick is introduced.
            // - If gates are not met, we skip and continue to accept via
            // the
            //   normal implicit path (accept-time pseudo-tx injection).
            //
            // TBD (2026-03-03): We did not find a robust timing model that
            // folds this into a guaranteed-safe explicit final proposal
            // across lossy/reordered links without increasing churn. Keep
            // this path as opt-in for future evaluation.
            {
                bool fullParticipantCoverage = false;
                bool entropyAligned = false;
                {
                    // Guard against "early switch" churn:
                    // require at least as many participants as the previous
                    // round before attempting the explicit-final mutation.
                    //
                    // This is a heuristic to reduce risk, not a proof of
                    // safety. We still keep the feature
                    // optional/default-off.
                    auto const participants = ctx.peerPositions.size() + 1;
                    auto const expectedParticipants = ctx.prevProposers + 1;
                    fullParticipantCoverage =
                        participants >= expectedParticipants;
                    // Require a majority aligned on entropySetHash before
                    // mutating tx-set hash. If this threshold is loosened,
                    // the probability of hash fragmentation rises quickly.
                    auto const requiredEntropyAligned =
                        (expectedParticipants / 2) + 1;
                    auto const ourPos = ctx.getPosition();
                    if (ourPos.entropySetHash)
                    {
                        auto const expectedEntropy = *ourPos.entropySetHash;
                        std::size_t alignedPeers = 0;
                        bool conflict = false;
                        for (auto const& [_, peerPos] : ctx.peerPositions)
                        {
                            auto const& peerPosition =
                                peerPos.proposal().position();
                            if (!peerPosition.entropySetHash)
                                continue;
                            if (*peerPosition.entropySetHash == expectedEntropy)
                            {
                                ++alignedPeers;
                                continue;
                            }
                            conflict = true;
                            break;
                        }

                        auto const alignedParticipants = alignedPeers + 1;
                        entropyAligned = !conflict &&
                            alignedParticipants >= requiredEntropyAligned;
                        if (!entropyAligned)
                        {
                            JLOG(ext.j_.debug())
                                << "RNG: explicit-final entropy alignment "
                                   "insufficient"
                                << " alignedParticipants="
                                << alignedParticipants
                                << " required=" << requiredEntropyAligned
                                << " conflict=" << (conflict ? "yes" : "no");
                        }
                    }
                    else
                    {
                        JLOG(ext.j_.debug())
                            << "RNG: explicit-final waiting on local "
                               "entropySetHash";
                    }
                }

                if (ctx.mode == ConsensusMode::proposing &&
                    !ext.explicitFinalProposalSent_ &&
                    ext.hasQuorumOfCommits() && revealConsensus &&
                    fullParticipantCoverage && entropyAligned &&
                    ext.shouldSendExplicitFinalProposal())
                {
                    // One-shot per round. This avoids repeated mutations/
                    // broadcasts from timer ticks, which can amplify
                    // network chatter in the exact conditions
                    // (loss/reordering) where this path is already fragile.
                    auto const synthSet = ext.buildExplicitFinalProposalTxSet(
                        ctx.getTxns(), buildSeq);
                    ext.explicitFinalProposalSent_ = true;

                    if (synthSet)
                    {
                        auto const synthHash = synthSet->id();
                        auto currentPos = ctx.getPosition();
                        auto newPos = currentPos;
                        newPos.updateTxSet(synthHash);

                        if (!(newPos == currentPos))
                        {
                            // WARNING:
                            // This changes proposal tx-set identity late in
                            // establish. Keep this path tightly gated and
                            // optional. The canonical ledger path remains
                            // the implicit accept-time injection logic.

                            // Maintain the invariant that our active
                            // position's tx-set hash is present in
                            // acquired_, otherwise gotTxSet can assert if
                            // this set arrives back from the network.
                            ctx.cacheAndShareTxSet(*synthSet);
                            JLOG(ext.j_.debug())
                                << "RNG: cached explicit-final txSet="
                                << synthHash;
                            ctx.updatePosition(newPos);
                            ctx.propose();
                            JLOG(ext.j_.debug())
                                << "RNG: explicit final proposal txSet="
                                << synthHash;
                            logRngDiag("rng-explicit-final-proposed");
                        }
                    }
                }
                else
                {
                    char const* reason = "disabled";
                    if (ctx.mode != ConsensusMode::proposing)
                        reason = "not-proposing";
                    else if (ext.explicitFinalProposalSent_)
                        reason = "already-sent";
                    else if (!ext.hasQuorumOfCommits())
                        reason = "no-commit-quorum";
                    else if (!revealConsensus)
                        reason = "reveal-timeout";
                    else if (!fullParticipantCoverage)
                        reason = "participant-gap";
                    else if (!entropyAligned)
                        reason = "entropy-not-aligned";
                    JLOG(ext.j_.debug())
                        << "STALLDIAG: rng-explicit-final-skipped"
                        << " reason=" << reason
                        << " mode=" << to_string(ctx.mode) << " sent="
                        << (ext.explicitFinalProposalSent_ ? "yes" : "no");
                }
            }
        }
    }
    else
    {
        JLOG(ext.j_.debug())
            << "RNGGATE: skipping RNG substates"
            << " prevSeq=" << (static_cast<std::uint32_t>(ctx.buildSeq) - 1)
            << " phase=establish"
            << " mode=" << to_string(ctx.mode);
    }
    //@@end rng-phase-establish-substates

    //@@start export-sig-convergence-gate
    // Export sig convergence gate: runs after RNG sub-states when Export has
    // verified signatures to converge, or when a tx-converged peer advertises
    // an exportSigSetHash we may need to fetch. This is a bounded safety
    // coordination window, not a wait-for-Export-success mechanism.
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

        auto fetchPeerExportSigSets = [&](auto const& pos) {
            std::size_t peerSets = 0;
            for (auto const& [_, peerPos] : ctx.peerPositions)
            {
                auto const& pp = peerPos.proposal().position();
                if (!(pp == pos))
                    continue;  // not tx-converged
                if (!pp.exportSigSetHash)
                    continue;

                ++peerSets;
                ext.fetchRngSetIfNeeded(
                    pp.exportSigSetHash, Ext::SidecarKind::exportSig);
            }
            return peerSets;
        };

        bool hasLocalExportSigs = ext.hasPendingExportSigs();
        if (!hasLocalExportSigs)
        {
            auto const peerSets = fetchPeerExportSigSets(ctx.getPosition());
            if (peerSets > 0)
            {
                startExportSigGate();
                hasLocalExportSigs = ext.hasPendingExportSigs();
                if (!hasLocalExportSigs)
                {
                    auto const elapsed =
                        ctx.nowSteady - ext.exportSigGateStart_;
                    auto const deadline = ctx.parms.rngREVEAL_TIMEOUT * 2;
                    if (elapsed <= deadline)
                    {
                        JLOG(ext.j_.debug())
                            << "Export: bounded wait for advertised "
                               "exportSigSet "
                               "fetch/merge"
                            << " peerSets=" << peerSets;
                        return {};
                    }

                    ext.setExportSigConvergenceFailed();
                    JLOG(ext.j_.warn())
                        << "Export: advertised exportSigSet did not converge "
                           "locally within bounded safety window; exports "
                           "will retry or expire"
                        << " peerSets=" << peerSets;
                }
            }
            else if (ext.hasConsensusExportTxns())
            {
                // A candidate ttEXPORT with no local sig material gets one
                // short observation window so an already-reachable peer
                // exportSigSetHash can be fetched before apply. If nothing
                // appears in time, apply takes the retry/expire path.
                startExportSigGate();
                auto const elapsed = ctx.nowSteady - ext.exportSigGateStart_;
                auto const deadline = ctx.parms.rngREVEAL_TIMEOUT * 2;
                if (elapsed <= deadline)
                {
                    JLOG(ext.j_.debug())
                        << "Export: bounded wait for exportSigSet "
                           "advertisement";
                    return {};
                }

                ext.setExportSigConvergenceFailed();
                JLOG(ext.j_.warn())
                    << "Export: no exportSigSet advertisement within bounded "
                       "safety window; exports will retry or expire";
            }
        }

        if (hasLocalExportSigs)
        {
            //@@start export-publish-sigset-hash
            auto const buildSeqExport = ctx.buildSeq;
            auto const exportHash = ext.buildExportSigSet(buildSeqExport);

            auto currentPos = ctx.getPosition();
            bool const publishedNewHash = !currentPos.exportSigSetHash ||
                *currentPos.exportSigSetHash != exportHash;
            if (publishedNewHash)
            {
                currentPos.exportSigSetHash = exportHash;
                ctx.updatePosition(currentPos);

                if (ctx.mode == ConsensusMode::proposing)
                    ctx.propose();

                JLOG(ext.j_.debug())
                    << "Export: published exportSigSetHash=" << exportHash;
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
                    JLOG(ext.j_.debug())
                        << "Export: exportSigSet published, waiting for peer "
                           "observation";
                    return {};
                }

                struct ExportPeerState
                {
                    bool conflict = false;
                    std::size_t aligned = 0;
                    std::size_t peersSeen = 0;
                    std::size_t txConverged = 0;
                };

                auto inspectExportPeers =
                    [&](auto const& pos,
                        bool fetchMismatches) -> ExportPeerState {
                    ExportPeerState state;
                    if (!pos.exportSigSetHash)
                        return state;

                    for (auto const& [_, peerPos] : ctx.peerPositions)
                    {
                        auto const& pp = peerPos.proposal().position();
                        if (!(pp == pos))
                            continue;  // not tx-converged
                        ++state.txConverged;
                        if (!pp.exportSigSetHash)
                            continue;  // peer hasn't published yet
                        ++state.peersSeen;
                        if (*pp.exportSigSetHash == *pos.exportSigSetHash)
                        {
                            ++state.aligned;
                            continue;
                        }

                        state.conflict = true;
                        if (fetchMismatches)
                            ext.fetchRngSetIfNeeded(
                                pp.exportSigSetHash,
                                Ext::SidecarKind::exportSig);
                    }
                    return state;
                };

                auto exportState = inspectExportPeers(ctx.getPosition(), true);
                auto const exportQuorum = ext.exportSigQuorumThreshold();
                auto quorumAligned = [&] {
                    return exportState.aligned + 1 >= exportQuorum;
                };
                auto fullObservation = [&] {
                    // Export success changes ledger effects too. Require a
                    // full view of tx-converged peers before treating a local
                    // quorum as safe enough to succeed in this ledger.
                    return exportState.peersSeen == exportState.txConverged;
                };

                if (exportState.conflict && !quorumAligned())
                {
                    auto const refreshedHash =
                        ext.buildExportSigSet(buildSeqExport);
                    auto current = ctx.getPosition();
                    if (!current.exportSigSetHash ||
                        *current.exportSigSetHash != refreshedHash)
                    {
                        current.exportSigSetHash = refreshedHash;
                        ctx.updatePosition(current);
                        if (ctx.mode == ConsensusMode::proposing)
                            ctx.propose();
                        JLOG(ext.j_.debug())
                            << "Export: refreshed exportSigSetHash after merge "
                               "to "
                            << refreshedHash;
                    }

                    exportState = inspectExportPeers(ctx.getPosition(), true);
                }

                if (exportState.conflict && quorumAligned() &&
                    fullObservation())
                {
                    JLOG(ext.j_.info())
                        << "Export: exportSigSetHash conflict ignored after "
                           "quorum alignment"
                        << " alignedParticipants=" << (exportState.aligned + 1)
                        << " quorum=" << exportQuorum
                        << " peersSeen=" << exportState.peersSeen
                        << " txConverged=" << exportState.txConverged;
                }
                else if (
                    exportState.conflict || !quorumAligned() ||
                    !fullObservation())
                {
                    auto const elapsed =
                        ctx.nowSteady - ext.exportSigGateStart_;
                    auto const deadline = ctx.parms.rngREVEAL_TIMEOUT * 2;
                    if (elapsed <= deadline)
                    {
                        JLOG(ext.j_.debug())
                            << "Export: waiting for exportSigSet quorum "
                               "alignment"
                            << " alignedParticipants="
                            << (exportState.aligned + 1)
                            << " quorum=" << exportQuorum
                            << " peersSeen=" << exportState.peersSeen
                            << " txConverged=" << exportState.txConverged
                            << " conflict="
                            << (exportState.conflict ? "yes" : "no");
                        return {};
                    }

                    ext.setExportSigConvergenceFailed();
                    JLOG(ext.j_.warn())
                        << "Export: exportSigSet quorum alignment missing "
                           "within deadline; exports will retry or expire"
                        << " alignedParticipants=" << (exportState.aligned + 1)
                        << " quorum=" << exportQuorum
                        << " peersSeen=" << exportState.peersSeen
                        << " txConverged=" << exportState.txConverged
                        << " conflict="
                        << (exportState.conflict ? "yes" : "no");
                }
            }
            //@@end export-sigset-conflict-wait
        }
    }
    //@@end export-sig-convergence-gate

    return {.readyForAccept = true};
}

}  // namespace ripple

#endif
