//------------------------------------------------------------------------------
// SteppingTiming — §5.5 timing & phase control, in both driving idioms:
//
//   IMPERATIVE  — walk network time beat by beat (net.tick()) and inspect
//                 state between beats; the stepping counterpart of the
//                 Stage-2 virtual tick().
//   DECLARATIVE — inject actions at chosen virtual instants with the generic
//                 combinator (net.in(delay, node, closure)) and let a bounded
//                 runTo() play the timeline out.
//
// Plus deterministic heartbeat phase SKEW (node i beats at k·dt + i·skew):
// real networks are not same-instant synchronized, and a skewed network must
// converge just as deterministically as the synchronized default.
//
// Everything here is ordering, not performance: identical runs must replay
// identically — arrival time deterministically gates ledger inclusion.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/TER.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingTiming_test : public beast::unit_test::suite
{
    void
    testNetClockBounds()
    {
        testcase("modeled clock offsets saturate without unsigned wrap");
        using namespace std::chrono;
        SteppingNetwork net(*this);
        net.validators(1);
        if (!BEAST_EXPECT(net.allUp()))
            return;
        net.settle();
        net.clockOffset(0, seconds{-120});
        net.advanceTime(seconds{60});
        BEAST_EXPECT(net.node(0).clock().now() == NetClock::time_point{});
        net.clockOffset(0, seconds{std::numeric_limits<seconds::rep>::max()});
        net.advanceTime(seconds{1});
        BEAST_EXPECT(
            net.node(0).clock().now().time_since_epoch().count() ==
            std::numeric_limits<NetClock::rep>::max());
        net.clockOffset(0, seconds{std::numeric_limits<seconds::rep>::min()});
        net.advanceTime(seconds{1});
        BEAST_EXPECT(net.node(0).clock().now() == NetClock::time_point{});
    }

    void
    testQuietGapAfterNodeZeroStops()
    {
        testcase("quiet gap advances the surviving node after node zero stops");
        using namespace std::chrono;
        SteppingNetwork net(*this);
        net.validators(2);
        if (!BEAST_EXPECT(net.allUp()))
            return;
        net.settle();
        net.stopNode(0);
        auto const schedulerBefore = net.controller().now();
        auto const steadyBefore = net.node(1).app().getStopwatch().now();
        auto const netBefore = net.node(1).clock().now();
        auto const validatedBefore = net.validSeq(1);
        net.advanceTime(seconds{60});
        BEAST_EXPECT(net.controller().now() == schedulerBefore + seconds{60});
        BEAST_EXPECT(
            net.node(1).app().getStopwatch().now() ==
            steadyBefore + seconds{60});
        BEAST_EXPECT(net.node(1).clock().now() == netBefore + seconds{60});
        BEAST_EXPECT(net.validSeq(1) == validatedBefore);
        BEAST_EXPECT(net.offThreadJobs() == 0 && net.failedJobs() == 0);
    }

    void
    testImperativeTicking()
    {
        testcase("imperative: tick() one beat at a time, inspecting between");
        using namespace std::chrono;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return;

        // Walk time beat by beat until the first ledgers validate, asserting
        // between EVERY beat that validated seq never regresses — the kind of
        // mid-flight invariant only the imperative idiom can watch.
        constexpr std::size_t kMaxBeats = 60;
        std::size_t beats = 0;
        std::uint32_t lastValidated = net.minValidatedSeq();
        while (net.minValidatedSeq() < 3 && beats < kMaxBeats)
        {
            net.tick(seconds{1});
            ++beats;
            auto const v = net.minValidatedSeq();
            BEAST_EXPECT(v >= lastValidated);  // monotone between beats
            lastValidated = v;
        }
        log << "  validated seq " << net.minValidatedSeq() << " after " << beats
            << " imperative beats" << std::endl;
        BEAST_EXPECT(net.minValidatedSeq() >= 3);
        BEAST_EXPECT(beats < kMaxBeats);
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
    }

    struct InjectionOutcome
    {
        std::uint32_t aliceSeq = 0;  // ledger seq carrying the early payment
        std::uint32_t bobSeq = 0;    // ledger seq carrying the late payment
        std::vector<uint256> chain;  // validated hashes [2 .. target]
    };

    // Declarative timeline: at steady state, inject an early payment (2.5s in,
    // on node 0) and a late one (15s in, on node 2), then play out. Arrival
    // time must gate inclusion: the early tx lands in a strictly earlier
    // ledger, and the whole timeline replays identically.
    std::optional<InjectionOutcome>
    runInjectionScenario()
    {
        using namespace std::chrono;
        using namespace jtx;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        Account const alice{"alice"};
        Account const bob{"bob"};
        std::shared_ptr<Transaction> txA, txB;
        net.in(milliseconds{2500}, 0, [&]() {
            txA = net.submit(
                0, pay(Account::master, alice, XRP(1000)), Account::master);
        });
        net.in(seconds{15}, 2, [&]() {
            txB = net.submit(
                2, pay(Account::master, bob, XRP(1000)), Account::master);
        });

        auto const target = net.minValidatedSeq() + 6;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        if (!BEAST_EXPECT(txA && txB))
            return std::nullopt;
        BEAST_EXPECT(txA->getResult() == tesSUCCESS);
        BEAST_EXPECT(txB->getResult() == tesSUCCESS);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(net.offThreadJobs() == 0);

        InjectionOutcome out;
        for (std::uint32_t seq = 3; seq <= target; ++seq)
        {
            auto const l = net.ledger(0, seq);
            if (!l)
                continue;
            if (!out.aliceSeq && l->txExists(txA->getID()))
                out.aliceSeq = seq;
            if (!out.bobSeq && l->txExists(txB->getID()))
                out.bobSeq = seq;
        }
        if (!BEAST_EXPECT(out.aliceSeq != 0 && out.bobSeq != 0))
            return std::nullopt;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            out.chain.push_back(net.ledgerHash(0, seq));
        return out;
    }

    void
    testArrivalTimeGatesInclusion()
    {
        testcase(
            "declarative: injections at different instants land in different "
            "ledgers, and the timeline replays identically");
        auto const run1 = runInjectionScenario();
        auto const run2 = runInjectionScenario();
        if (!BEAST_EXPECT(run1 && run2))
            return;

        // The 2.5s tx must close strictly before the 15s tx.
        BEAST_EXPECT(run1->aliceSeq < run1->bobSeq);
        log << "  early tx seq " << run1->aliceSeq << ", late tx seq "
            << run1->bobSeq << std::endl;

        // And the whole timed timeline is reproducible: same inclusion seqs,
        // same validated hash chain.
        BEAST_EXPECT(run1->aliceSeq == run2->aliceSeq);
        BEAST_EXPECT(run1->bobSeq == run2->bobSeq);
        BEAST_EXPECT(run1->chain == run2->chain);
    }

    // A phase-skewed network (node i beats 2ms·i late) converging to target;
    // returns node 0's validated hash chain [2..target].
    //
    // DISCOVERED BOUNDARY (design-notes §2 addendum): skew must stay BELOW the
    // link delay (5ms here). At skew >= linkDelay, a fast node's validation
    // reaches a slow node BEFORE the slow node has built that ledger, and
    // handleNewValidation fires the real ledger-ACQUIRE path (jtADVANCE
    // "getConsensusLedger2" -> InboundLedgers) — correctly fail-loud, because
    // the acquire closure graph is not modeled yet. Raising skew past linkDelay
    // is the entry ticket for the late-joiner/acquire increment (plan §5.6),
    // not a config tweak.
    std::vector<uint256>
    runSkewed(std::uint32_t target)
    {
        using namespace std::chrono;
        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return {};
        net.runTo(
            target,
            SteppingNetwork::RunBudget{},
            SteppingNetwork::Cadence{seconds{1}, milliseconds{2}});
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return {};
        }
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            chain.push_back(net.ledgerHash(0, seq));
        return chain;
    }

    void
    testSkewedHeartbeatsConverge()
    {
        testcase(
            "phase-skewed heartbeats: converges, agrees, and replays "
            "identically");
        auto const chain1 = runSkewed(4);
        auto const chain2 = runSkewed(4);
        if (!BEAST_EXPECT(!chain1.empty() && !chain2.empty()))
            return;
        BEAST_EXPECT(chain1 == chain2);
        log << "  skewed chains " << (chain1 == chain2 ? "MATCH" : "DIFFER")
            << std::endl;
    }

public:
    void
    run() override
    {
        testNetClockBounds();
        testQuietGapAfterNodeZeroStops();
        testImperativeTicking();
        testArrivalTimeGatesInclusion();
        testSkewedHeartbeatsConverge();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingTiming, consensus, ripple);

}  // namespace ripple::test
