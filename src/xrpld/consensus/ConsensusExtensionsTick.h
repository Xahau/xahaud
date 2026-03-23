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
    // SHAMap fetch/diff/merge in handleAcquiredRngSet is a safety net
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
            if (ctx.prevProposers < threshold)
            {
                JLOG(ext.j_.debug())
                    << "RNG: bootstrap skip (prevProposers="
                    << ctx.prevProposers << " < threshold=" << threshold << ")";
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
                    ext.fetchRngSetIfNeeded(peerPosition.commitSetHash);
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
                JLOG(ext.j_.debug())
                    << "RNG: fast-path published entropySet in same tick";
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
    // Export sig convergence gate: runs after RNG sub-states, only when
    // both CE and Export are enabled. Builds/publishes exportSigSetHash
    // and waits for peer agreement before accepting.
    if constexpr (requires { ctx.getPosition().exportSigSetHash; })
    {
        // Only run when CE is active (provides ExtendedPosition infra)
        // and there are export sigs to converge.
        if (isRngEnabled)
        {
            if (ext.hasPendingExportSigs())
            {
                //@@start export-publish-sigset-hash
                auto const buildSeqExport = ctx.buildSeq;
                auto const exportHash = ext.buildExportSigSet(buildSeqExport);

                auto currentPos = ctx.getPosition();
                if (!currentPos.exportSigSetHash ||
                    *currentPos.exportSigSetHash != exportHash)
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
                // Check peer agreement on exportSigSetHash.
                // If any tx-converged peer has a different non-empty hash,
                // wait briefly for fetch/merge to resolve it.
                {
                    bool conflict = false;
                    for (auto const& [_, peerPos] : ctx.peerPositions)
                    {
                        auto const& pp = peerPos.proposal().position();
                        if (!pp.exportSigSetHash)
                            continue;
                        if (*pp.exportSigSetHash != exportHash)
                        {
                            conflict = true;

                            // Trigger fetch for the differing set
                            ext.fetchRngSetIfNeeded(pp.exportSigSetHash);
                            break;
                        }
                    }

                    if (conflict)
                    {
                        // Don't block indefinitely — use the same pipeline
                        // timeout as RNG.
                        bool const timeout =
                            ctx.roundTime > ctx.parms.rngPIPELINE_TIMEOUT;
                        if (!timeout)
                        {
                            JLOG(ext.j_.debug())
                                << "Export: exportSigSetHash conflict, waiting";
                            return {};
                        }
                        JLOG(ext.j_.info())
                            << "Export: exportSigSetHash conflict timed out, "
                               "proceeding (exports will retry next round)";
                    }
                }
                //@@end export-sigset-conflict-wait
            }
        }
    }
    //@@end export-sig-convergence-gate

    return {.readyForAccept = true};
}

}  // namespace ripple

#endif
