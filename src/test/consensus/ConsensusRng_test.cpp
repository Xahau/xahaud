//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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
#include <test/csf.h>
#include <test/unit_test/SuiteJournal.h>
#include <xrpld/consensus/Consensus.h>
#include <xrpl/beast/unit_test.h>

namespace ripple {
namespace test {

class ConsensusRng_test : public beast::unit_test::suite
{
    SuiteJournal journal_;

public:
    ConsensusRng_test() : journal_("ConsensusRng_test", *this)
    {
    }
    void
    testRngCommitRevealConverges()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG commit/reveal converges");

        ConsensusParms const parms{};
        Sim sim;
        PeerGroup peers = sim.createGroup(5);

        peers.trustAndConnect(
            peers, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        // Runtime opt-in keeps CSF on a single Peer type, which minimizes
        // maintenance and upstream sync churn in Sim/PeerGroup infrastructure.
        for (Peer* peer : peers)
            peer->ce().enableRngConsensus_ = true;

        // Warmup: run 1 round so prevProposers_ is populated (bootstrap
        // skip bypasses the RNG pipeline when prevProposers < quorum).
        sim.run(1);
        BEAST_EXPECT(sim.synchronized());

        sim.run(3);

        if (BEAST_EXPECT(sim.synchronized()))
        {
            for (Peer const* peer : peers)
            {
                BEAST_EXPECT(!peer->ce().lastEntropyWasFallback_);
                BEAST_EXPECT(peer->ce().lastEntropyCount_ > 0);
                BEAST_EXPECT(peer->ce().lastEntropyDigest_ != uint256{});
            }
        }
    }

    void
    testRngCommitRevealConvergesWithTransactions()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG commit/reveal converges with non-empty tx set");

        ConsensusParms const parms{};
        Sim sim;
        PeerGroup peers = sim.createGroup(5);

        peers.trustAndConnect(
            peers, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        for (Peer* peer : peers)
            peer->ce().enableRngConsensus_ = true;

        // Warmup: run 1 round so prevProposers_ is populated (bootstrap
        // skip bypasses the RNG pipeline when prevProposers < quorum).
        sim.run(1);
        BEAST_EXPECT(sim.synchronized());

        // Submit transactions for the real test round.
        for (Peer* peer : peers)
            peer->submit(Tx(static_cast<std::uint32_t>(peer->id)));

        sim.run(1);

        if (BEAST_EXPECT(sim.synchronized()))
        {
            for (Peer const* peer : peers)
            {
                auto const& lcl = peer->lastClosedLedger;
                BEAST_EXPECT(!peer->ce().lastEntropyWasFallback_);
                BEAST_EXPECT(peer->ce().lastEntropyCount_ > 0);
                BEAST_EXPECT(peer->ce().lastEntropyDigest_ != uint256{});
                BEAST_EXPECT(lcl.txs().size() > 0);
            }
        }
    }

    void
    testRngImpossibleQuorumFallback()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG impossible quorum fallback");

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup majority = sim.createGroup(2);
        PeerGroup isolated = sim.createGroup(1);
        PeerGroup network = majority + isolated;

        for (Peer* peer : network)
            peer->ce().enableRngConsensus_ = true;

        // First run fully connected so expected proposers include all three.
        network.trust(network);
        network.connect(
            network, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));
        sim.run(1);

        // Then isolate one node so 80% quorum becomes impossible for majority.
        majority.disconnect(isolated);
        isolated.disconnect(majority);
        majority.connect(
            majority, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        sim.run(1);

        if (BEAST_EXPECT(sim.synchronized(majority)))
        {
            for (Peer const* peer : majority)
            {
                BEAST_EXPECT(peer->ce().lastEntropyWasFallback_);
                BEAST_EXPECT(peer->ce().lastEntropyDigest_ == uint256{});
                BEAST_EXPECT(peer->ce().lastEntropyCount_ == 0);
            }
        }
    }

    void
    testRngPersistentLossDoesNotShrinkQuorum()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG persistent loss does not shrink quorum");

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup majority = sim.createGroup(2);
        PeerGroup isolated = sim.createGroup(1);
        PeerGroup network = majority + isolated;

        for (Peer* peer : network)
            peer->ce().enableRngConsensus_ = true;

        network.trust(network);
        network.connect(
            network, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        // Seed recent-proposer hints from a fully connected round.
        sim.run(1);

        // Then isolate one validator and run multiple degraded rounds. Commit
        // quorum must remain fixed to the active trusted set (3 -> threshold 3)
        // rather than silently shrinking to the 2 surviving peers.
        majority.disconnect(isolated);
        isolated.disconnect(majority);
        majority.connect(
            majority, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        sim.run(2);

        if (BEAST_EXPECT(sim.synchronized(majority)))
        {
            for (Peer const* peer : majority)
            {
                BEAST_EXPECT(peer->ce().lastEntropyWasFallback_);
                BEAST_EXPECT(peer->ce().lastEntropyDigest_ == uint256{});
                BEAST_EXPECT(peer->ce().lastEntropyCount_ == 0);
            }
        }
    }

    void
    testRngTimeoutWithPartialQuorum()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG timeout with partial quorum keeps entropy");

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup majority = sim.createGroup(4);
        PeerGroup isolated = sim.createGroup(1);
        PeerGroup network = majority + isolated;

        for (Peer* peer : network)
            peer->ce().enableRngConsensus_ = true;

        network.trust(network);
        network.connect(
            network, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        // Seed expected proposers from a fully connected round.
        sim.run(1);

        // Isolate one expected proposer. Majority should still progress after
        // timeout using available commit quorum instead of zero-entropy
        // fallback.
        majority.disconnect(isolated);
        isolated.disconnect(majority);
        majority.connect(
            majority, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        sim.run(1);

        if (BEAST_EXPECT(sim.synchronized(majority)))
        {
            for (Peer const* peer : majority)
            {
                BEAST_EXPECT(!peer->ce().lastEntropyWasFallback_);
                BEAST_EXPECT(peer->ce().lastEntropyDigest_ != uint256{});
                BEAST_EXPECT(peer->ce().lastEntropyCount_ > 0);
            }
        }
    }

    void
    testRngCommitSetConflictForcesFallback()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG commitSet conflict forces fallback");

        ConsensusParms const parms{};
        Sim sim;
        PeerGroup peers = sim.createGroup(5);

        for (Peer* peer : peers)
            peer->ce().enableRngConsensus_ = true;

        peers.trustAndConnect(
            peers, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        // Keep tx-set convergence intact but force one peer to advertise a
        // different commitSetHash so we exercise the conflict-only guard.
        peers[0]->ce().forcedCommitSetHash_ =
            sha512Half(std::string("forced-csf"));

        sim.run(1);

        if (BEAST_EXPECT(sim.synchronized(peers)))
        {
            for (Peer const* peer : peers)
            {
                BEAST_EXPECT(peer->ce().lastEntropyWasFallback_);
                BEAST_EXPECT(peer->ce().lastEntropyDigest_ == uint256{});
            }
        }
    }

    void
    testRngObserverDoesNotExpectSelfCommit()
    {
        using namespace csf;

        testcase("RNG observer does not expect self commit");

        Sim sim;
        PeerGroup peers = sim.createGroup(2);
        Peer* validator = peers[0];
        Peer* observer = peers[1];
        PeerGroup validatorGroup{validator};
        PeerGroup observerGroup{observer};

        for (Peer* peer : peers)
            peer->ce().enableRngConsensus_ = true;
        observer->runAsValidator = false;

        validatorGroup.trust(validatorGroup);
        observerGroup.trust(validatorGroup);

        observer->ce().cacheUNLReport();
        BEAST_EXPECT(observer->ce().unlNodes_.count(observer->id) == 0);
        BEAST_EXPECT(observer->ce().unlNodes_.count(validator->id) == 1);

        hash_set<PeerID> proposers;
        proposers.insert(observer->id);
        proposers.insert(validator->id);
        observer->ce().setExpectedProposers(std::move(proposers));

        BEAST_EXPECT(
            observer->ce().likelyParticipants_.count(observer->id) == 0);
        BEAST_EXPECT(
            observer->ce().likelyParticipants_.count(validator->id) == 1);

        observer->ce().pendingCommits_[validator->id] = sha512Half(42u);
        BEAST_EXPECT(observer->ce().hasQuorumOfCommits());
    }

    void
    testRngIgnoresNonUNLData()
    {
        using namespace csf;

        testcase("RNG ignores non-UNL data");

        Sim sim;
        PeerGroup peers = sim.createGroup(1);
        Peer* peer = peers[0];
        peer->ce().enableRngConsensus_ = true;
        peer->ce().cacheUNLReport();

        ProposalPosition pos;
        pos.myCommitment = sha512Half(1u);
        pos.myReveal = sha512Half(2u);

        // NodeID 999 is not in this peer's UNL report (which contains only
        // self).
        peer->ce().harvestRngData(
            PeerID{999},
            PeerKey{PeerID{999}, 0},
            pos,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        BEAST_EXPECT(peer->ce().pendingCommits_.empty());
        BEAST_EXPECT(peer->ce().pendingReveals_.empty());
    }

    void
    testRngRejectsRevealWithoutCommit()
    {
        using namespace csf;

        testcase("RNG rejects reveal without commit");

        Sim sim;
        PeerGroup peers = sim.createGroup(1);
        Peer* peer = peers[0];
        peer->ce().enableRngConsensus_ = true;
        peer->ce().cacheUNLReport();

        ProposalPosition pos;
        pos.myReveal = sha512Half(3u);

        peer->ce().harvestRngData(
            peer->id,
            peer->key,
            pos,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        BEAST_EXPECT(peer->ce().pendingCommits_.empty());
        BEAST_EXPECT(peer->ce().pendingReveals_.empty());
    }

    void
    testRngRejectsInvalidReveal()
    {
        using namespace csf;

        testcase("RNG rejects invalid reveal");

        Sim sim;
        PeerGroup peers = sim.createGroup(1);
        Peer* peer = peers[0];
        peer->ce().enableRngConsensus_ = true;
        peer->ce().cacheUNLReport();

        auto const seq =
            static_cast<std::uint32_t>(peer->lastClosedLedger.seq()) + 1;
        auto const committedReveal = sha512Half(10u);
        auto const invalidReveal = sha512Half(11u);
        auto const commitment = sha512Half(
            committedReveal,
            static_cast<std::uint32_t>(peer->id),
            peer->key.second,
            seq);

        ProposalPosition commitPos;
        commitPos.myCommitment = commitment;
        peer->ce().harvestRngData(
            peer->id,
            peer->key,
            commitPos,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        ProposalPosition revealPos;
        revealPos.myReveal = invalidReveal;
        peer->ce().harvestRngData(
            peer->id,
            peer->key,
            revealPos,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        BEAST_EXPECT(peer->ce().pendingCommits_.size() == 1);
        BEAST_EXPECT(peer->ce().pendingReveals_.empty());
    }

    void
    testRngCommitChangeClearsStaleReveal()
    {
        using namespace csf;

        testcase("RNG commit change clears stale reveal");

        Sim sim;
        PeerGroup peers = sim.createGroup(1);
        Peer* peer = peers[0];
        peer->ce().enableRngConsensus_ = true;
        peer->ce().cacheUNLReport();

        auto const seq =
            static_cast<std::uint32_t>(peer->lastClosedLedger.seq()) + 1;
        auto const revealA = sha512Half(20u);
        auto const revealB = sha512Half(21u);
        auto const commitA = sha512Half(
            revealA,
            static_cast<std::uint32_t>(peer->id),
            peer->key.second,
            seq);
        auto const commitB = sha512Half(
            revealB,
            static_cast<std::uint32_t>(peer->id),
            peer->key.second,
            seq);

        ProposalPosition commitPosA;
        commitPosA.myCommitment = commitA;
        peer->ce().harvestRngData(
            peer->id,
            peer->key,
            commitPosA,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        ProposalPosition revealPosA;
        revealPosA.myReveal = revealA;
        peer->ce().harvestRngData(
            peer->id,
            peer->key,
            revealPosA,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        BEAST_EXPECT(peer->ce().pendingReveals_.size() == 1);

        // Commitment changes after reveal was accepted. The old reveal is now
        // cryptographically stale and must no longer count toward reveal
        // quorum.
        ProposalPosition commitPosB;
        commitPosB.myCommitment = commitB;
        peer->ce().harvestRngData(
            peer->id,
            peer->key,
            commitPosB,
            0,
            peer->now(),
            peer->lastClosedLedger.id(),
            0);

        BEAST_EXPECT(peer->ce().pendingCommits_.size() == 1);
        BEAST_EXPECT(peer->ce().pendingReveals_.empty());
    }

    void
    testRngRevealTimeoutAsymmetricDelays()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG reveal timeout under asymmetric delays");

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup groupA = sim.createGroup(3);
        PeerGroup groupB = sim.createGroup(3);
        PeerGroup network = groupA + groupB;

        for (Peer* peer : network)
            peer->ce().enableRngConsensus_ = true;

        network.trust(network);

        auto const fast = round<milliseconds>(0.2 * parms.ledgerGRANULARITY);
        groupA.connect(groupA, fast);
        groupB.connect(groupB, fast);

        // Cross-group links are intentionally slower than rngREVEAL_TIMEOUT.
        auto const slow = round<milliseconds>(2.0 * parms.ledgerGRANULARITY);
        groupA.connect(groupB, slow);
        groupB.connect(groupA, slow);

        sim.run(1);

        // If this ever forks/splits, reveal-timeout handling is allowing
        // non-deterministic entropy subsets to close.
        BEAST_EXPECT(sim.branches(network) == 1);
        BEAST_EXPECT(sim.synchronized(network));
    }

    void
    testRngEntropyConvergesWithPartialReveals()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG entropy converges with partial reveal subsets");

        // 6 peers in two groups. Group A drops reveals from peer 5,
        // group B drops reveals from peer 0.  Both groups have > 80%
        // quorum of reveals but DIFFERENT subsets.
        //
        // Without the entropySetHash convergence gate, these groups
        // compute different entropy -> different pseudo-tx -> fork.
        //
        // With the gate, they must either converge on the same reveal
        // set (via SHAMap fetch/merge) or both fall back to zero
        // entropy.  Either way: no fork.

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup groupA = sim.createGroup(3);
        PeerGroup groupB = sim.createGroup(3);
        PeerGroup network = groupA + groupB;

        for (Peer* peer : network)
            peer->ce().enableRngConsensus_ = true;

        network.trust(network);

        auto const fast = round<milliseconds>(0.2 * parms.ledgerGRANULARITY);
        network.connect(network, fast);

        // Group A never sees peer 5's reveal
        for (Peer* peer : groupA)
            peer->ce().dropRevealFrom_.insert(network[5]->id);

        // Group B never sees peer 0's reveal
        for (Peer* peer : groupB)
            peer->ce().dropRevealFrom_.insert(network[0]->id);

        sim.run(1);

        // Must not fork
        BEAST_EXPECT(sim.branches(network) == 1);
        BEAST_EXPECT(sim.synchronized(network));

        // All peers must agree on entropy (same digest or all zero)
        auto const& refDigest = network[0]->ce().lastEntropyDigest_;
        for (Peer const* peer : network)
        {
            BEAST_EXPECT(peer->ce().lastEntropyDigest_ == refDigest);
            BEAST_EXPECT(
                peer->ce().lastEntropyCount_ ==
                network[0]->ce().lastEntropyCount_);
        }
    }

    void
    testRngEntropyFallbackOnMajorRevealLoss()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG entropy falls back to zero on major reveal loss");

        // 5 peers.  Peer 0 drops reveals from peers 2, 3, 4
        // (only sees 2/5 reveals = 40%, below 80% quorum).
        // All other peers see all reveals.
        //
        // Peer 0 must fall back to zero entropy.
        // The network must still agree (either all use full entropy
        // from the converged set, or all fall back).

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup peers = sim.createGroup(5);
        for (Peer* peer : peers)
            peer->ce().enableRngConsensus_ = true;

        peers.trustAndConnect(
            peers, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        // Peer 0 drops most reveals
        peers[0]->ce().dropRevealFrom_.insert(peers[2]->id);
        peers[0]->ce().dropRevealFrom_.insert(peers[3]->id);
        peers[0]->ce().dropRevealFrom_.insert(peers[4]->id);

        sim.run(1);

        // Must not fork
        BEAST_EXPECT(sim.branches(peers) == 1);
        BEAST_EXPECT(sim.synchronized(peers));

        // All peers must agree on entropy
        auto const& refDigest = peers[0]->ce().lastEntropyDigest_;
        for (Peer const* peer : peers)
            BEAST_EXPECT(peer->ce().lastEntropyDigest_ == refDigest);
    }

    void
    testRngSingleByzantineCannotDenyEntropy()
    {
        using namespace csf;
        using namespace std::chrono;

        testcase("RNG single Byzantine validator cannot deny entropy");

        // 5 peers, all see all reveals. Peer 0 forces a different
        // entropy set hash (simulating a Byzantine node publishing
        // a garbage entropySetHash).
        //
        // The remaining 4/5 (80%) should still produce valid entropy.
        // The Byzantine node's hash should be outvoted by supermajority.

        ConsensusParms const parms{};
        Sim sim;

        PeerGroup peers = sim.createGroup(5);
        for (Peer* peer : peers)
            peer->ce().enableRngConsensus_ = true;

        peers.trustAndConnect(
            peers, round<milliseconds>(0.2 * parms.ledgerGRANULARITY));

        // TODO: add forcedEntropySetHash_ test knob to CsfExtensions
        // For now, this test documents the expected behavior.
        // When the convergence gate is implemented, uncomment:
        //
        // peers[0]->ce().forcedEntropySetHash_ =
        //     sha512Half(std::string("byzantine-entropy"));

        sim.run(1);

        BEAST_EXPECT(sim.branches(peers) == 1);
        BEAST_EXPECT(sim.synchronized(peers));

        // With 5 healthy peers, entropy should be non-zero
        for (Peer const* peer : peers)
        {
            BEAST_EXPECT(!peer->ce().lastEntropyWasFallback_);
            BEAST_EXPECT(peer->ce().lastEntropyDigest_ != uint256{});
        }
    }

    void
    run() override
    {
        // Set XAHAU_RNG_TEST=<name> to run a single test method.
        // e.g. XAHAU_RNG_TEST=SingleByzantine
        auto const* filter = std::getenv("XAHAU_RNG_TEST");
        std::string f = filter ? filter : "";

#define RUN(method)                                                         \
    do                                                                      \
    {                                                                       \
        if (f.empty() || std::string(#method).find(f) != std::string::npos) \
            method();                                                       \
    } while (false)

        RUN(testRngCommitRevealConverges);
        RUN(testRngCommitRevealConvergesWithTransactions);
        RUN(testRngImpossibleQuorumFallback);
        RUN(testRngPersistentLossDoesNotShrinkQuorum);
        RUN(testRngTimeoutWithPartialQuorum);
        RUN(testRngCommitSetConflictForcesFallback);
        RUN(testRngObserverDoesNotExpectSelfCommit);
        RUN(testRngIgnoresNonUNLData);
        RUN(testRngRejectsRevealWithoutCommit);
        RUN(testRngRejectsInvalidReveal);
        RUN(testRngCommitChangeClearsStaleReveal);
        RUN(testRngRevealTimeoutAsymmetricDelays);
        RUN(testRngEntropyConvergesWithPartialReveals);
        RUN(testRngEntropyFallbackOnMajorRevealLoss);
        RUN(testRngSingleByzantineCannotDenyEntropy);

#undef RUN
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusRng, consensus, ripple);
}  // namespace test
}  // namespace ripple
