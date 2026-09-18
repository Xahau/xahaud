//------------------------------------------------------------------------------
// SteppingByzantine — axis B: POSITION-ALTITUDE misbehavior, the byzantine
// validator. Where SteppingFaults is a hostile NETWORK (drop/delay/dup at the
// wire — an attacker WITHOUT keys), this is a hostile VALIDATOR: a node that
// signs WRONG content with its real keys. That is the one class of adversary
// wire faults structurally cannot express — a signed-but-false validation only
// a keyholder can produce.
//
// CONTAINED BY CONSTRUCTION: zero production change. SteppingNetwork::
// injectValidation builds the bad validation with the production STValidation
// ctor and pushes it through the production Overlay::broadcast(), exactly as
// RCLConsensus::Adaptor::validate does — the harness only supplies the keys and
// the lie. The byzantine node stays honest INTERNALLY (we never process the
// injected validation locally); only its peers receive it, so it equivocates:
// the node's real validate() still broadcasts the honest validation, and a peer
// sees two validations from one signer at one sequence.
//
// The oracle is the byzantine-fault-tolerance property itself: an honest quorum
// converges DESPITE a lying validator, no honest node ever validates a forked
// ledger, and — the harness's signature — the whole byzantine run replays
// bit-for-bit. quorum(5) = 4 = exactly the four honest nodes, so node 4's lies
// (or its honest vote) are never load-bearing; the network must not need them.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpld/app/misc/NetworkOPs.h>

#include <ripple.pb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace ripple::test {

class SteppingByzantine_test : public beast::unit_test::suite
{
    // A fabricated ledger hash that is a deterministic function of seq (stable
    // across replays) and cannot collide with any real ledger the network
    // builds (high bits set).
    static uint256
    fakeHash(std::uint32_t seq)
    {
        uint256 h;
        h.data()[0] = 0xBA;
        h.data()[1] = 0xD5;
        // The non-vacuity oracle records only the first 8 hash hex chars.
        // Put the test sequence there so distinct lies stay distinguishable.
        h.data()[2] = static_cast<std::uint8_t>((seq >> 8) & 0xff);
        h.data()[3] = static_cast<std::uint8_t>(seq & 0xff);
        h.data()[31] = static_cast<std::uint8_t>(seq);
        return h;
    }

    static std::string
    fakePrefix(uint256 const& hash)
    {
        return to_string(hash).substr(0, 8);
    }

    static bool
    sawValidationPrefix(SteppingNetwork& net, uint256 const& hash)
    {
        auto const prefix = fakePrefix(hash);
        return std::any_of(
            net.forensics().valEvents.begin(),
            net.forensics().valEvents.end(),
            [&prefix](std::string const& ev) { return ev.find(prefix) != std::string::npos; });
    }

    void
    testEquivocatingValidator()
    {
        testcase(
            "one validator signs a fabricated ledger at each seq (equivocating "
            "with its honest vote); the honest quorum(5)=4 converges without "
            "forking, and the byzantine run replays bit-for-bit");
        constexpr std::uint32_t n = 5;
        constexpr std::uint32_t byz = 4;
        expectReplays(*this, "equivocating validator", [&](SteppingNetwork& net) {
            using Payload = std::optional<std::vector<uint256>>;
            // canonicalJobs: an injected validation for a hash no node has
            // triggers a REAL acquire (GetConsL2 → InboundLedger → virtual
            // retries) — exercised, not modeled, so it must run as a job.
            net.canonicalJobs();
            net.recordForensics();
            net.validators(n).mesh();
            if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                return Payload{};
            net.runTo(3);
            if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                return Payload{};

            // For three ledgers, node 4 broadcasts a fabricated full
            // validation for the NEXT sequence just before it closes —
            // alongside the honest validation its real validate() still
            // sends. Peers receive both: equivocation from a trusted signer.
            std::vector<uint256> lies;
            for (int round = 0; round < 3; ++round)
            {
                auto const seq = net.minValidatedSeq() + 1;
                auto const fake = fakeHash(seq);
                lies.push_back(fake);
                net.lieValidation(byz, seq, fake);
                net.runTo(seq);
                // Safety holds at EVERY step, not just at the end.
                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return Payload{};
                BEAST_EXPECT(net.offThreadJobs() == 0);
                BEAST_EXPECT(net.failedJobs() == 0);
            }

            // The honest four must have fully validated a shared chain past
            // the byzantine window. Node 4 is allowed to hold whatever
            // twisted view its own lies produced — we ask only the honest set.
            auto const target = net.minValidatedSeq() + 2;
            net.runTo(target);
            if (!BEAST_EXPECT(net.validatedAgree({0, 1, 2, 3}, target)))
            {
                for (std::uint32_t i = 0; i < n; ++i)
                    log << "  n" << i << " valid=" << net.validSeq(i)
                        << " mode=" << static_cast<int>(net.mode(i)) << std::endl;
                return Payload{};
            }
            BEAST_EXPECT(net.validatedForkFree());

            // NON-VACUITY (the rung-2b discipline): the lies must actually
            // have REACHED an honest node's validation machinery — otherwise
            // the scenario proves nothing about byzantine tolerance, it just
            // proves a healthy network converges. Scan the recorded
            // validation lifecycle for a fabricated hash prefix.
            for (std::size_t i = 0; i < lies.size(); ++i)
                for (std::size_t j = i + 1; j < lies.size(); ++j)
                    if (!BEAST_EXPECT(fakePrefix(lies[i]) != fakePrefix(lies[j])))
                        return Payload{};
            std::uint32_t liesSeen = 0;
            for (auto const& fake : lies)
            {
                auto const prefix = fakePrefix(fake);
                for (auto const& ev : net.forensics().valEvents)
                    if (ev.find(prefix) != std::string::npos)
                    {
                        ++liesSeen;
                        break;
                    }
            }
            log << "  byzantine validations injected=" << lies.size()
                << " reached an honest node=" << liesSeen << std::endl;
            if (!BEAST_EXPECT(liesSeen == lies.size()))
                return Payload{};

            std::vector<uint256> chain;
            for (std::uint32_t seq = 2; seq <= target; ++seq)
                chain.push_back(net.ledgerHash(0, seq));
            return Payload{std::move(chain)};
        });
    }

    // The safety boundary when honest votes fall below quorum: two
    // validators of five WITHHOLD their validations (a wire drop of
    // mtVALIDATION on every outbound link — the silent/withholding
    // byzantine fault, distinct from the LYING byzantine of the other two
    // cases). Now the honest set is {0,1,2} = 3, below quorum(5)=4, so
    // consensus can CLOSE ledgers but cannot VALIDATE them: liveness is
    // lost. The property under test is that it is lost SAFELY — no honest
    // node validates a forked ledger while starved — and that clearing the
    // withholding recovers full validation. The below-quorum analog of the
    // loss envelope (SteppingFaults), composing a withholding fault with
    // the wire.
    //
    // (Codex round 16: an earlier form ALSO injected lies here, but the
    // outbound mtVALIDATION drop suppresses crafted lieValidation traffic
    // too — same message type, same wire — so the lies were a no-op. The
    // scenario proves the SUPPRESSION property; lying under below-quorum
    // is untestable via wire-drop since honest and crafted validations are
    // indistinguishable at the wire. Reframed to withholding-only.)
    void
    testWithholdingQuorumStarvation()
    {
        testcase(
            "two validators of five WITHHOLD their validations; the honest 3 "
            "< quorum(5)=4 stalls WITHOUT forking, and restoring the votes "
            "recovers full validation");
        constexpr std::uint32_t n = 5;
        std::vector<std::uint32_t> const byz{3, 4};
        expectReplays(*this, "withholding quorum starvation", [&](SteppingNetwork& net) {
            using Payload = std::optional<std::vector<uint256>>;
            net.canonicalJobs();
            net.validators(n).mesh();
            if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                return Payload{};
            net.runTo(3);
            if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                return Payload{};

            // Withholding window: each node drops its own validations to
            // every peer (suppressing its vote) so only the honest 3
            // remain — below quorum.
            for (auto const b : byz)
                for (std::uint32_t p = 0; p < n; ++p)
                    if (p != b)
                        net.faultLink(b, p, simfaults::dropType(protocol::mtVALIDATION));

            auto const honestValidated = [&net]() {
                return std::min({net.validSeq(0), net.validSeq(1), net.validSeq(2)});
            };
            auto const stalledAt = honestValidated();
            // Drive several beats under starvation and confirm the honest
            // three never advance their validated tip while safety holds
            // every beat.
            for (int round = 0; round < 4; ++round)
            {
                net.tick();
                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return Payload{};
                BEAST_EXPECT(net.offThreadJobs() == 0);
                BEAST_EXPECT(net.failedJobs() == 0);
            }
            // NON-VACUITY: liveness must actually have been lost — the
            // honest validated tip held while starved (a scenario where
            // the honest 3 kept validating would prove nothing about the
            // quorum boundary).
            if (!BEAST_EXPECT(honestValidated() == stalledAt))
            {
                log << "  honest tip advanced " << stalledAt << " -> " << honestValidated()
                    << " (not starved)" << std::endl;
                return Payload{};
            }
            // ...but the network kept CLOSING ledgers below quorum (it is
            // stalled at validation, not dead) — the below-quorum shape.
            BEAST_EXPECT(net.closedSeq(0) > net.validSeq(0));
            log << "  starved: honest validated held at " << stalledAt << ", closed advanced to "
                << net.closedSeq(0) << std::endl;

            // Heal: stop withholding. The two nodes rejoin the honest
            // majority and full validation resumes.
            for (auto const b : byz)
                for (std::uint32_t p = 0; p < n; ++p)
                    if (p != b)
                        net.faultLink(b, p, {});
            auto const target = net.minValidatedSeq() + 3;
            if (!BEAST_EXPECT(net.runUntil(
                    [&net, target]() {
                        return net.minValidatedSeq() >= target && net.ledgersAgree(target);
                    },
                    SteppingNetwork::RunBudget{/*heartbeats=*/25})))
            {
                for (std::uint32_t i = 0; i < n; ++i)
                    log << "  n" << i << " valid=" << net.validSeq(i) << std::endl;
                return Payload{};
            }
            BEAST_EXPECT(net.validatedForkFree());
            for (std::uint32_t i = 0; i < n; ++i)
                BEAST_EXPECT(net.mode(i) == OperatingMode::FULL);
            log << "  healed: validated " << net.minValidatedSeq() << ", all nodes FULL"
                << std::endl;

            std::vector<uint256> chain;
            for (std::uint32_t seq = 2; seq <= target; ++seq)
                chain.push_back(net.ledgerHash(0, seq));
            chain.push_back(uint256{stalledAt});
            return Payload{std::move(chain)};
        });
    }

    // The two-faced attack proper: a byzantine node tells DIFFERENT peers
    // DIFFERENT stories at the same sequence — the classic equivocation
    // designed to split a network into two camps validating different
    // ledgers. Node 4 sends honest camp {0,1} a validation for hash A and
    // honest camp {2,3} a validation for hash B (both fabricated, same
    // seq), via per-peer send (not broadcast). The honest four remain fully
    // interconnected, so the property under test is that equivocation
    // CANNOT split a connected honest quorum: they converge on the real
    // chain among themselves and no node validates a fork — even though the
    // one signer demonstrably delivered contradictory validations to
    // different peers.
    void
    testPerPeerEquivocation()
    {
        testcase(
            "a byzantine node sends two honest camps different fabricated "
            "ledgers at one seq (true per-peer equivocation); the connected "
            "honest quorum converges without forking, and it replays");
        constexpr std::uint32_t n = 5;
        constexpr std::uint32_t byz = 4;
        std::vector<std::uint32_t> const campA{0, 1};
        std::vector<std::uint32_t> const campB{2, 3};
        expectReplays(*this, "per-peer equivocation", [&](SteppingNetwork& net) {
            using Payload = std::optional<std::vector<uint256>>;
            net.canonicalJobs();
            net.recordForensics();
            net.validators(n).mesh();
            if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                return Payload{};
            net.runTo(3);
            if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                return Payload{};

            std::vector<std::pair<uint256, uint256>> lies;  // (A, B)
            for (int round = 0; round < 3; ++round)
            {
                auto const seq = net.minValidatedSeq() + 1;
                auto const hashA = fakeHash(seq);         // camp A's story
                auto const hashB = fakeHash(seq + 1000);  // camp B's story
                if (!BEAST_EXPECT(fakePrefix(hashA) != fakePrefix(hashB)))
                    return Payload{};
                lies.emplace_back(hashA, hashB);
                net.lieValidationTo(byz, campA, seq, hashA).lieValidationTo(byz, campB, seq, hashB);
                net.runTo(seq);
                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return Payload{};
                BEAST_EXPECT(net.offThreadJobs() == 0);
                BEAST_EXPECT(net.failedJobs() == 0);
            }

            auto const target = net.minValidatedSeq() + 2;
            net.runTo(target);
            if (!BEAST_EXPECT(net.validatedAgree({0, 1, 2, 3}, target)))
                return Payload{};
            BEAST_EXPECT(net.validatedForkFree());

            // NON-VACUITY: the equivocation must have LANDED asymmetric —
            // camp A saw hash A and camp B saw hash B from the same
            // signer. Scan the recorded validation lifecycle for each
            // camp's prefix appearing on a node OF THAT CAMP.
            auto sawOnCamp = [&](std::vector<std::uint32_t> const& camp, uint256 const& hash) {
                auto const prefix = fakePrefix(hash);
                for (auto const c : camp)
                {
                    auto const tag = "n" + std::to_string(c) + " ";
                    for (auto const& ev : net.forensics().valEvents)
                        if (ev.find(tag) != std::string::npos &&
                            ev.find(prefix) != std::string::npos)
                            return true;
                }
                return false;
            };
            std::uint32_t split = 0;
            for (auto const& [a, b] : lies)
                if (sawOnCamp(campA, a) && sawOnCamp(campB, b))
                    ++split;
            log << "  equivocation rounds where both camps saw their own "
                << "story = " << split << "/" << lies.size() << std::endl;
            if (!BEAST_EXPECT(split == lies.size()))
                return Payload{};

            std::vector<uint256> chain;
            for (std::uint32_t seq = 2; seq <= target; ++seq)
                chain.push_back(net.ledgerHash(0, seq));
            return Payload{std::move(chain)};
        });
    }

    // COMPOSITION REGRESSION: this deliberately combines three independently
    // useful harness axes in one history:
    //
    //   1. Byzantine input: a trusted validator signs a fabricated ledger.
    //   2. Topology churn: that validator is isolated, then reconnected.
    //   3. Modeled execution pressure: its scheduler events cost 20x while
    //      every node has its own finite K budget per heartbeat.
    //
    // The single-axis tests establish each mechanism in isolation. This test
    // covers their interaction: a lie must not poison the honest chain; the
    // four honest validators must remain live while the liar is absent and
    // slow; and the laggard must use the real acquire/rejoin path to recover
    // under the SAME pressure model after links return. A clean unprofiled
    // recovery after the profiled phase would not prove that final claim, so
    // there is intentionally no ordinary runTo/runUntil fallback below.
    //
    // Topology matters. With five validators, quorum is four, exactly the
    // honest cohort {0,1,2,3}. Node 4 is Byzantine only at the wire boundary:
    // lieValidation() broadcasts one signed false validation to its peers, but
    // node 4's own consensus state remains honest. Its normal validation for
    // the real ledger is still emitted, making the pair an equivocation. That
    // containment lets the test demand that node 4 eventually rejoin the real
    // chain; a node whose internal ledger state was corrupted would require a
    // different recovery contract.
    //
    // Every phase has a non-vacuity witness:
    //
    //   * the fabricated hash appears in validation forensics;
    //   * the honest cohort reaches a target that the isolated node does not;
    //   * weightedEvents/clampHits prove K pacing executed and saturated;
    //   * one continuous profiled heal preserves node-local lag until all five
    //     nodes reach and agree at the fixed healTarget;
    //   * expectReplays compares chain hashes AND pressure/lag observations,
    //     so a timing-shape change cannot hide behind an unchanged final tip.
    //
    //@@start composed-equivocation-partition-heal-per-node-k
    void
    testEquivocationPartitionHealUnderPerNodeK()
    {
        testcase(
            "a lying validator is then partitioned and K-lagged; the honest "
            "quorum advances under pressure, heal recovers, and no fork "
            "appears");
        constexpr std::uint32_t n = 5;
        constexpr std::uint32_t byz = 4;
        std::vector<std::uint32_t> const honest{0, 1, 2, 3};
        expectReplays(
            *this, "equivocation + partition/heal + per-node K", [&](SteppingNetwork& net) {
                using Payload = std::optional<std::vector<uint256>>;
                using namespace std::chrono;

                // Phase 0: establish a healthy, fully connected baseline. Job
                // execution is canonicalized because processing the phantom
                // validation and later acquiring missed ledgers are part of
                // the behavior under test, not mocked harness shortcuts.
                net.canonicalJobs();
                net.recordForensics();
                net.validators(n).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};
                net.runTo(3);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                    return Payload{};

                // Phase 1, Byzantine input while connected. The prefix check
                // prevents a vacuous pass where the network remains fork-free
                // only because the injected lie never reached validation
                // processing. The node also emits its ordinary honest vote for
                // this sequence, so peers observe two claims by one signer.
                auto const lieSeq = net.minValidatedSeq() + 1;
                auto const fake = fakeHash(lieSeq);
                net.lieValidation(byz, lieSeq, fake);
                net.runTo(lieSeq);
                if (!BEAST_EXPECT(sawValidationPrefix(net, fake)))
                    return Payload{};
                if (!BEAST_EXPECT(net.validatedForkFree()))
                    return Payload{};

                // Phase 2, partition plus pressure. Flush in-flight link work
                // while isolating node 4 so its frozen validated tip is a clean
                // partition boundary, not a message-delivery race.
                auto const partitionPoint = net.minValidatedSeq();
                net.isolateNodeAndFlush(byz);

                auto honestMin = [&net, &honest]() {
                    auto out = std::numeric_limits<std::uint32_t>::max();
                    for (auto const h : honest)
                        out = std::min(out, net.validSeq(h));
                    return out;
                };

                auto const honestTarget = partitionPoint + 3;
                std::uint32_t beat = 0;
                std::uint32_t firstHonestAheadBeat = 0;
                std::uint32_t maxHonestAhead = 0;
                bool forkFreeDuringPressure = true;
                // Observe only quiescent beat boundaries. This establishes
                // temporal separation, not merely different final tips: we
                // record when the honest cohort first gets ahead and its
                // maximum lead while continuously checking fork freedom.
                auto const afterBeat = [&]() {
                    ++beat;
                    forkFreeDuringPressure = forkFreeDuringPressure && net.validatedForkFree();
                    auto const honestTip = honestMin();
                    auto const byzTip = net.validSeq(byz);
                    if (honestTip > byzTip)
                    {
                        if (firstHonestAheadBeat == 0)
                            firstHonestAheadBeat = beat;
                        maxHonestAhead = std::max(maxHonestAhead, honestTip - byzTip);
                    }
                };

                // Each scheduler event receives a deterministic modeled cost:
                // K * unitCost * event-kind weight * node multiplier. In
                // per-node horizon mode, over-budget work becomes owner-local
                // observed-time lag; it does not advance global ordering time.
                // The 20x multiplier therefore makes node 4 the explicit
                // laggard without sacrificing deterministic event order.
                auto options = SteppingNetwork::KProfiledOptions{
                    /*k=*/1,
                    /*unitCost=*/milliseconds{5},
                    HarnessScheduler::ProfiledPacer::NodeMultipliers::single(
                        /*nodeId=*/byz, /*value=*/20)};
                options.horizonMode = HarnessScheduler::ProfiledPacer::HorizonMode::perNode;

                auto const stats = net.runProfiledTo(
                    honestTarget,
                    options,
                    SteppingNetwork::RunBudget{/*heartbeats=*/24, /*steps=*/1'000'000},
                    SteppingNetwork::Cadence{/*dt=*/seconds{1}, /*skew=*/milliseconds{20}},
                    afterBeat);

                // The partition phase is meaningful only if the honest quorum
                // advanced, node 4 demonstrably lagged, and the pressure model
                // actually charged and clamped work.
                auto const honestTip = honestMin();
                auto const byzTipAtPartition = net.validSeq(byz);
                if (!BEAST_EXPECT(forkFreeDuringPressure && net.validatedForkFree()))
                    return Payload{};
                if (!BEAST_EXPECT(honestTip >= honestTarget))
                    return Payload{};
                if (!BEAST_EXPECT(byzTipAtPartition < honestTip))
                    return Payload{};
                BEAST_EXPECT(firstHonestAheadBeat != 0);
                BEAST_EXPECT(maxHonestAhead != 0);
                BEAST_EXPECT(stats.weightedEvents != 0);
                BEAST_EXPECT(stats.clampHits != 0);

                // Phase 3, heal under the SAME modeled pressure. Node 4 is
                // honest internally, so reconnecting it should drive the real
                // status/acquire/validation path until it rejoins the honest
                // chain. Reaching an all-node validated minimum does not imply
                // that asynchronous historical acquisition at that exact tip
                // has settled. runProfiledUntil() therefore checks agreement
                // at a quiescent beat boundary while retaining ONE profiling
                // state from reconnect through catch-up. In particular,
                // node-local lag is not reset between the beat that reaches
                // healTarget and any later beat needed to acquire its history.
                // An ordinary runTo/runUntil fallback here would mask failure
                // of the property named by this test.
                net.reconnectNode(byz);
                auto const healTarget = honestTarget + 2;
                std::size_t healBeat = 0;
                std::size_t targetReachedBeat = 0;
                std::size_t agreementBeat = 0;
                auto const afterHealBeat = [&]() {
                    ++healBeat;
                    if (targetReachedBeat == 0 && net.minValidatedSeq() >= healTarget)
                        targetReachedBeat = healBeat;
                    if (agreementBeat == 0 && net.minValidatedSeq() >= healTarget &&
                        net.ledgersAgree(healTarget))
                        agreementBeat = healBeat;
                };
                auto const healStats = net.runProfiledUntil(
                    [&net, healTarget]() {
                        return net.minValidatedSeq() >= healTarget && net.ledgersAgree(healTarget);
                    },
                    options,
                    SteppingNetwork::RunBudget{/*heartbeats=*/80, /*steps=*/1'000'000},
                    SteppingNetwork::Cadence{/*dt=*/seconds{1}, /*skew=*/milliseconds{20}},
                    afterHealBeat);

                if (!net.expectConverged(healTarget))
                    return Payload{};
                if (!BEAST_EXPECT(targetReachedBeat != 0 && agreementBeat >= targetReachedBeat))
                    return Payload{};
                auto const settleBeats = agreementBeat - targetReachedBeat;
                BEAST_EXPECT(healStats.weightedEvents != 0);
                BEAST_EXPECT(healStats.clampHits != 0);
                BEAST_EXPECT(net.validatedForkFree());
                BEAST_EXPECT(net.validatedAgree(honest, healTarget));
                BEAST_EXPECT(net.offThreadJobs() == 0);
                BEAST_EXPECT(net.failedJobs() == 0);

                log << "  composed Axis B: pre-heal honestTip=" << honestTip
                    << ", pre-heal byzTip=" << byzTipAtPartition
                    << ", post-heal byzTip=" << net.validSeq(byz)
                    << ", post-heal byzMode=" << static_cast<int>(net.mode(byz))
                    << ", pressureClamps=" << stats.clampHits
                    << ", healClamps=" << healStats.clampHits << ", healBeats=" << healStats.beats
                    << ", targetReachedBeat=" << targetReachedBeat
                    << ", agreementBeat=" << agreementBeat << ", settleBeats=" << settleBeats
                    << ", firstAheadBeat=" << firstHonestAheadBeat
                    << ", maxAhead=" << maxHonestAhead << std::endl;

                // Replay payload includes the agreed chain, the lie, and
                // pressure/lead witnesses. Two executions that reach the same
                // ledger tip through different scheduler behavior must compare
                // unequal instead of being reported as deterministic.
                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= healTarget; ++seq)
                    chain.push_back(net.ledgerHash(0, seq));
                chain.push_back(fake);
                chain.push_back(uint256{stats.clampHits});
                chain.push_back(uint256{healStats.clampHits});
                chain.push_back(uint256{healStats.beats});
                chain.push_back(uint256{targetReachedBeat});
                chain.push_back(uint256{agreementBeat});
                chain.push_back(uint256{settleBeats});
                chain.push_back(uint256{firstHonestAheadBeat});
                chain.push_back(uint256{maxHonestAhead});
                chain.push_back(uint256{byzTipAtPartition});
                return Payload{std::move(chain)};
            });
    }
    //@@end composed-equivocation-partition-heal-per-node-k

public:
    void
    run() override
    {
        testEquivocatingValidator();
        testWithholdingQuorumStarvation();
        testPerPeerEquivocation();
        testEquivocationPartitionHealUnderPerNodeK();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingByzantine, consensus, ripple);

}  // namespace ripple::test
