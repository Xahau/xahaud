//------------------------------------------------------------------------------
// SteppingCombined — the combined-stress timeline: every axis the harness
// models, interacting in ONE deterministic run. Each axis has its own suite
// (SteppingTxStress, SteppingLateJoiner, SteppingFaults, SteppingPartition's
// restart cousin); their INTERACTIONS were untested — and interactions are
// where real networks live: transactions flowing while links are lossy, a
// brand-new validator catching up to TX-BEARING ledgers (everything before
// this caught up over empty chains), a restart whose gap contains payments.
//
// The timeline (6 identities, quorum(6)=5 — deliberately at the edge):
//   1. five validators up, converge, fund a two-account economy;
//   2. seeded 10% loss on everything node 0 sends (absorbed regime) — and it
//      STAYS on through everything below;
//   3. payments submitted on different nodes while the loss runs;
//   4. node 5 is CREATED mid-scenario (identities/spawn unweld) and catches
//      up through the real acquire path — over tx-bearing ledgers, under
//      loss;
//   5. node 4 stops and restarts across a tx-bearing gap, and rejoins;
//   6. faults clear; the whole network converges; every payment is present
//      in the JOINER's acquired history exactly as in the submitters'.
// The entire timeline replays bit-for-bit (expectReplays) — stress is part
// of the timeline, not noise on top of it.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpld/app/misc/NetworkOPs.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingCombined_test : public beast::unit_test::suite
{
    using Payload = std::optional<std::vector<uint256>>;

    Payload
    runCombined(
        SteppingNetwork& net,
        std::vector<std::unique_ptr<beast::xor_shift_engine>>& engines)
    {
        using namespace jtx;
        using namespace std::chrono;
        Account const alice{"alice"};
        Account const bob{"bob"};

        // Six identities, five up: quorum(6)=5, so the live set validates
        // with zero slack until the sixth joins.
        net.identities(6);
        net.spawn({0, 1, 2, 3, 4}).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return Payload{};
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return Payload{};

        // A funded economy BEFORE the stress: from here on, ledgers bear txs.
        net.fund(0, XRP(100000), {alice, bob});

        // Loss goes on and STAYS on: 10% seeded drop on everything node 0
        // sends to the current mesh (fixed seeds; the engines outlive the
        // injectors and are re-created identically each replay run).
        for (std::uint32_t to = 1; to <= 4; ++to)
        {
            engines.push_back(std::make_unique<beast::xor_shift_engine>(
                0xC0B1EDFA017ull + to));
            net.faultLink(
                0, to, simfaults::dropWithProbability(*engines.back(), 0.10));
        }

        // Payments while lossy, submitted on DIFFERENT nodes; each autofills
        // from its node's open ledger at its virtual instant.
        std::vector<uint256> txids;
        net.in(milliseconds{500}, 1, [&]() {
            txids.push_back(net.submit(1, pay(alice, bob, XRP(1000)), alice)
                                ->getID());
        });
        net.in(milliseconds{1500}, 2, [&]() {
            txids.push_back(
                net.submit(2, pay(bob, alice, XRP(400)), bob)->getID());
        });
        net.runTo(net.minValidatedSeq() + 2);

        // A sixth validator is CREATED mid-scenario and must catch up
        // through the real acquire path — over TX-BEARING ledgers, with the
        // loss still running on node 0's links.
        auto const joinedAt = net.minValidatedSeq();
        net.spawn({5});
        if (!BEAST_EXPECT(net.isLive(5)))
            return Payload{};
        for (std::uint32_t peer = 0; peer < 5; ++peer)
            if (!BEAST_EXPECT(net.connect(5, peer) != nullptr))
                return Payload{};
        net.runTo(joinedAt + 2);
        if (!BEAST_EXPECT(net.validSeq(5) >= joinedAt + 2))
        {
            log << "  joiner stalled; diagnostics: " << net.jobDiagnostics()
                << std::endl;
            return Payload{};
        }

        // Restart node 4 across a tx-bearing gap: cut, stop, keep the
        // network moving (with another payment in the gap), restart, rejoin.
        net.isolateNodeAndFlush(4);
        net.stopNode(4);
        net.in(milliseconds{500}, 1, [&]() {
            txids.push_back(
                net.submit(1, pay(alice, bob, XRP(250)), alice)->getID());
        });
        net.runOnly({0, 1, 2, 3, 5}, net.validSeq(0) + 2);
        net.restartNode(4);
        if (!BEAST_EXPECT(net.isLive(4)))
            return Payload{};
        net.reconnectNode(4);

        // Heal: clear node 0's injectors (0->4's faulted wire died with the
        // isolate; clearing the fresh wire is a no-op) and converge everyone.
        for (std::uint32_t to = 1; to <= 4; ++to)
            net.faultLink(0, to, {});
        auto const target = net.validSeq(0) + 3;
        net.runTo(target);
        if (!net.expectConverged(target))
            return Payload{};
        for (std::uint32_t i = 0; i < 6; ++i)
            BEAST_EXPECT(net.mode(i) == OperatingMode::FULL);

        // History BACKFILL must have completed on every probed node — the
        // joiner (never saw pre-join ledgers), the restarted node (gap),
        // and node 0 (its own genesis-side holes: publish is what marks
        // complete, and nothing below the first published seq was marked).
        // haveLedger reads the same complete-ledgers set prevMissing
        // consults, per sequence — every ledger from genesis to the node's
        // publish point, no holes. This entire capability was dark until the
        // earliest_seq config fix + the doAdvance progress fix — see
        // .ai-docs/issues/open/002-upstream-doadvance-history-backfill-starvation.md
        for (std::uint32_t i : {0u, 4u, 5u})
        {
            auto& lm = net.node(i).app().getLedgerMaster();
            // completeLedgers marks at PUBLISH; the scenario ends when the
            // tip is VALIDATED, which can precede its publish by a beat — so
            // genesis-to-published is the honest completeness claim.
            auto const pub = lm.getPublishedLedger();
            if (!BEAST_EXPECT(pub != nullptr))
                continue;
            bool complete = true;
            for (std::uint32_t seq = 1; seq <= pub->info().seq; ++seq)
                complete = complete && lm.haveLedger(seq);
            if (!BEAST_EXPECT(complete))
                log << "  node " << i << " incomplete: "
                    << lm.getCompleteLedgers() << std::endl;
        }
        log << "  history backfilled to genesis on nodes 0, 4 and 5"
            << std::endl;

        // The acquired history must be BYTE-EXACT against submitter node
        // 0's: the per-seq ledger hash (which commits to the full ledger
        // contents) matches on the joiner and the restarted node for every
        // ledger, genesis to target — and one level below the hash, for a
        // readable failure and to pin apply ORDER explicitly, the joiner's
        // applied-transaction record (index, txid, account, TER) matches
        // node 0's exactly.
        for (std::uint32_t seq = 1; seq <= target; ++seq)
        {
            auto const refHash = net.ledgerHash(0, seq);
            BEAST_EXPECT(refHash != uint256{});
            for (std::uint32_t i : {4u, 5u})
                if (!BEAST_EXPECT(net.ledgerHash(i, seq) == refHash))
                    log << "  seq " << seq << ": node " << i << " hash "
                        << net.ledgerHash(i, seq) << " vs node0 " << refHash
                        << std::endl;

            auto const onZero = net.appliedTxs(0, seq);
            auto const onJoiner = net.appliedTxs(5, seq);
            bool match = onJoiner.size() == onZero.size();
            for (std::size_t k = 0; match && k < onZero.size(); ++k)
                match = onJoiner[k].index == onZero[k].index &&
                    onJoiner[k].txid == onZero[k].txid &&
                    onJoiner[k].account == onZero[k].account &&
                    onJoiner[k].result == onZero[k].result;
            if (!BEAST_EXPECT(match))
                log << "  seq " << seq << ": applied record differs; joiner "
                    << onJoiner.size() << " txs vs node0 " << onZero.size()
                    << " txs" << std::endl;
        }

        // And the submitted payments are actually IN that history: all
        // three appear on the joiner with tesSUCCESS (presence, on top of
        // the record-equality above).
        BEAST_EXPECT(txids.size() == 3);
        std::size_t found = 0;
        for (std::uint32_t seq = 1; seq <= target; ++seq)
            for (auto const& tx : net.appliedTxs(5, seq))
                for (auto const& id : txids)
                    if (tx.txid == id)
                    {
                        ++found;
                        BEAST_EXPECT(tx.result == tesSUCCESS);
                    }
        BEAST_EXPECT(found == txids.size());
        log << "  found " << found << "/" << txids.size()
            << " payments in the joiner's history" << std::endl;

        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            chain.push_back(net.ledgerHash(5, seq));
        return Payload{std::move(chain)};
    }

    void
    testCombinedStress()
    {
        testcase(
            "submit + loss + late join + restart in one timeline: every "
            "payment lands, the joiner's acquired history is exact, and the "
            "whole stressed run replays bit-for-bit");
        auto engines = std::make_shared<
            std::vector<std::unique_ptr<beast::xor_shift_engine>>>();
        expectReplays(
            *this, "combined stress", [this, engines](SteppingNetwork& net) {
                return runCombined(net, *engines);
            });
    }

public:
    void
    run() override
    {
        testCombinedStress();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingCombined, consensus, ripple);

}  // namespace ripple::test
