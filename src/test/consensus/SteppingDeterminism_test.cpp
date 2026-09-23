//------------------------------------------------------------------------------
// SteppingDeterminism — the standing determinism regression gate for the strict
// stepping harness (harness-evolution-plan.md §7.2, guarding the §4.5
// invariant: nondeterministic inputs must never reach consensus outcomes).
//
// Two proofs, kept deliberately lean (this is a gate, not a showcase):
//   1. CONVERGENCE — N = 2..4 validators on a full SimOverlay mesh converge to
//   a
//      shared validated ledger under strict stepping, within a bounded budget,
//      with the structural invariants intact: zero off-thread jobs, zero
//      unmodeled-job failures, and bit-identical ledgers at the target seq.
//   2. REPRODUCIBILITY — the validated-ledger hash CHAIN (not just one hash) of
//      two INDEPENDENT runs of the same scenario is identical, run-to-run, in
//      the same process. Empty ledgers hash only prev-hash + (empty) tx tree +
//      state tree + closeTime; closeTime is scheduler-driven (deterministic)
//      and the PRNG-derived validation cookies are metadata, never hashed. Any
//      capability that lets a nondeterministic input reach the hash breaks this
//      suite — which is exactly its job.
//
// This is also the first suite exercising SteppingNetwork itself (the scenario
// wrapper); the original SteppingNet_test remained on the development branch.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/basics/chrono.h>
#include <xrpl/beast/clock/abstract_clock.h>
#include <xrpl/beast/unit_test/suite.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingDeterminism_test : public beast::unit_test::suite
{
    // Drive N validators on a full mesh to `target` in the given net, assert
    // the structural invariants, and return node 0's validated hash chain
    // [2 .. target] (seq 1 is genesis, seq 2 the deterministic startup ledger;
    // consensus rounds build from there). Empty on any failure.
    std::vector<uint256>
    convergedChainIn(SteppingNetwork& net, std::size_t n, std::uint32_t target)
    {
        net.validators(n).mesh();
        if (!BEAST_EXPECT(net.allUp()))
            return {};
        // simConnect handshakes synchronously; this bounded poll just confirms
        // every node reached n-1 active peers.
        if (!BEAST_EXPECT(net.meshReady()))
            return {};

        auto const steps = net.runTo(target);

        log << "  N=" << n << ": " << steps
            << " scheduler events, minValidated=" << net.minValidatedSeq()
            << " (target " << target
            << "), offThreadJobs=" << net.offThreadJobs()
            << ", failedJobs=" << net.failedJobs() << std::endl;

        // Converged within the default budget (120 heartbeats / 1M steps)...
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return {};
        }
        // ...with the structural determinism invariants intact...
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        // ...and bit-identical history across all N nodes.
        BEAST_EXPECT(net.ledgersAgree(target));

        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
        {
            auto const h = net.ledgerHash(0, seq);
            BEAST_EXPECT(h != uint256{});
            chain.push_back(h);
        }
        return chain;
    }

    void
    testConvergence()
    {
        testcase("N=2..4 validators converge under strict stepping");
        for (std::size_t n : {2u, 3u, 4u})
        {
            SteppingNetwork net(*this);
            BEAST_EXPECT(!convergedChainIn(net, n, /*target=*/4).empty());
        }
    }

    void
    testReproducibleChain()
    {
        testcase(
            "validated hash chain AND executed event order are identical "
            "across in-process runs (grind: --unittest-arg=replays=N)");
        expectReplays(*this, "N=3 convergence", [this](SteppingNetwork& net) {
            auto chain = convergedChainIn(net, 3, /*target=*/4);
            if (chain.empty())
                return std::optional<std::vector<uint256>>{};
            return std::optional<std::vector<uint256>>(std::move(chain));
        });
    }

    // The ONE timeline-snapshot canary (jest-snapshot-style, deliberately
    // singular): the pinned literals below are the executed-order identity
    // of the N=3 target-4 scenario. A failure here means a code change
    // ALTERED CONSENSUS SCHEDULING — if that was intended, update the
    // literals in the same commit, with intent, and say so in its message.
    // (Every other suite asserts run-to-run equality instead, precisely so
    // that legitimate timeline changes only ever break THIS one place.)
    void
    testTimelineCanary()
    {
        testcase("timeline snapshot canary (pinned fingerprint)");
        SteppingNetwork net(*this);
        net.recordForensics();
        if (!BEAST_EXPECT(!convergedChainIn(net, 3, /*target=*/4).empty()))
            return;
        log << "  canary observed: fingerprint 0x" << std::hex
            << net.traceFingerprint() << std::dec << ", " << net.traceCount()
            << " events" << std::endl;
        if (net.traceFingerprint() != kCanaryFingerprint ||
            net.traceCount() != kCanaryEvents)
        {
            for (auto const& event : net.controller().scheduler().traceLog())
                log << "  canary event: when=" << event.when
                    << " tier=" << event.tier << " node=" << event.nodeId
                    << " kind=" << static_cast<int>(event.kind)
                    << " label=" << event.label << std::endl;
        }
        BEAST_EXPECT(net.traceFingerprint() == kCanaryFingerprint);
        BEAST_EXPECT(net.traceCount() == kCanaryEvents);
    }

    // Includes the deferred validated-ledger callbacks that run before the
    // N=3 target-4 stopping boundary; queued work beyond it is not counted.
    static constexpr std::uint64_t kCanaryFingerprint = 0xae0ffc783db25050ull;
    static constexpr std::uint64_t kCanaryEvents = 332;

    struct KProfiledSample
    {
        std::uint32_t k = 0;
        std::uint64_t fingerprint = 0;
        std::uint64_t events = 0;
        std::size_t steps = 0;
        std::size_t beats = 0;
        std::uint32_t minValidated = 0;
        std::uint32_t maxValidated = 0;
        std::uint32_t forkCheckedSeqs = 0;
        std::uint64_t clampHits = 0;
        std::int64_t requestedMs = 0;
        std::int64_t consumedMs = 0;
        std::int64_t maxConsumedBeatMs = 0;
        std::int64_t schedulerMs = 0;
        std::int64_t closeTimeSeconds = -1;
        bool forkFree = false;
        bool converged = false;
        std::uint64_t weightedEvents = 0;
        std::uint64_t heartbeatEvents = 0;
        std::uint64_t deliverEvents = 0;
        std::uint64_t jobEvents = 0;
        std::uint64_t timerEvents = 0;
        std::uint32_t firstClampWeight = 0;

        [[nodiscard]] bool
        operator==(KProfiledSample const& o) const
        {
            return k == o.k && fingerprint == o.fingerprint &&
                events == o.events && steps == o.steps && beats == o.beats &&
                minValidated == o.minValidated &&
                maxValidated == o.maxValidated &&
                forkCheckedSeqs == o.forkCheckedSeqs &&
                clampHits == o.clampHits && requestedMs == o.requestedMs &&
                consumedMs == o.consumedMs &&
                maxConsumedBeatMs == o.maxConsumedBeatMs &&
                schedulerMs == o.schedulerMs &&
                closeTimeSeconds == o.closeTimeSeconds &&
                forkFree == o.forkFree && converged == o.converged &&
                weightedEvents == o.weightedEvents &&
                heartbeatEvents == o.heartbeatEvents &&
                deliverEvents == o.deliverEvents && jobEvents == o.jobEvents &&
                timerEvents == o.timerEvents &&
                firstClampWeight == o.firstClampWeight;
        }
    };

    [[nodiscard]] static std::int64_t
    asMs(HarnessScheduler::duration d)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    }

    [[nodiscard]] static std::int64_t
    asMs(HarnessScheduler::time_point t)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   t.time_since_epoch())
            .count();
    }

    KProfiledSample
    runKProfiledScenario(std::uint32_t k)
    {
        using namespace std::chrono;
        constexpr std::uint32_t target = 4;
        KProfiledSample out;
        out.k = k;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return out;

        auto const stats = net.runProfiledTo(
            target,
            SteppingNetwork::KProfiledOptions{
                /*k=*/k,
                /*unitCost=*/milliseconds{5},
                HarnessScheduler::ProfiledPacer::NodeMultipliers{}},
            SteppingNetwork::RunBudget{
                /*heartbeats=*/160, /*steps=*/1'000'000});

        out.fingerprint = net.traceFingerprint();
        out.events = net.traceCount();
        out.steps = stats.steps;
        out.beats = stats.beats;
        out.minValidated = net.minValidatedSeq();
        for (std::uint32_t i = 0; i < 3; ++i)
            out.maxValidated = std::max(out.maxValidated, net.validSeq(i));
        out.forkCheckedSeqs = net.forkCheckedSeqs();
        out.clampHits = stats.clampHits;
        out.requestedMs = asMs(stats.requestedVirtualAdvance);
        out.consumedMs = asMs(stats.consumedVirtualAdvance);
        out.schedulerMs = asMs(stats.schedulerNow);
        out.weightedEvents = stats.weightedEvents;
        out.heartbeatEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::heartbeat)];
        out.deliverEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::deliver)];
        out.jobEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::job)];
        out.timerEvents = stats.eventsByKind[HarnessScheduler::kindIndex(
            HarnessScheduler::Kind::timer)];
        out.firstClampWeight = stats.firstClampWeight;
        for (auto const d : stats.consumedPerBeat)
        {
            auto const ms = asMs(d);
            if (ms > out.maxConsumedBeatMs)
                out.maxConsumedBeatMs = ms;
        }
        out.forkFree = net.validatedForkFree();
        out.converged = out.minValidated >= target;
        auto const closeSeq = out.converged ? target : out.minValidated;
        if (closeSeq >= 2)
            if (auto const closeTime = net.ledgerCloseTime(0, closeSeq))
                out.closeTimeSeconds = closeTime->time_since_epoch().count();

        log << "  K=" << k << ": fp=0x" << std::hex << out.fingerprint
            << std::dec << ", events=" << out.events << ", steps=" << out.steps
            << ", beats=" << out.beats << ", minValidated=" << out.minValidated
            << ", maxValidated=" << out.maxValidated
            << ", forkCheckedSeqs=" << out.forkCheckedSeqs
            << ", clampHits=" << out.clampHits
            << ", requestedMs=" << out.requestedMs
            << ", consumedMs=" << out.consumedMs
            << ", maxBeatMs=" << out.maxConsumedBeatMs
            << ", schedulerMs=" << out.schedulerMs
            << ", closeTime=" << out.closeTimeSeconds
            << ", forkFree=" << out.forkFree << ", converged=" << out.converged
            << ", weightedEvents=" << out.weightedEvents
            << ", kindEvents={heartbeat:" << out.heartbeatEvents
            << ", deliver:" << out.deliverEvents << ", job:" << out.jobEvents
            << ", timer:" << out.timerEvents
            << "}, firstClampWeight=" << out.firstClampWeight << std::endl;
        if (stats.saturated())
        {
            log << "    first clamp: kind="
                << HarnessScheduler::kindName(stats.firstClampEvent.kind)
                << ", tier="
                << HarnessScheduler::tierName(stats.firstClampEvent.tier)
                << ", node=" << stats.firstClampEvent.nodeId
                << ", requestedMs=" << asMs(stats.firstClampRequested)
                << ", budgetMs=" << asMs(stats.firstClampBudget)
                << ", weight=" << stats.firstClampWeight << std::endl;
        }

        bool globalLagZero = true;
        for (auto const lag : stats.nodeLag)
            globalLagZero =
                globalLagZero && lag == HarnessScheduler::duration{};
        BEAST_EXPECT(globalLagZero);
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(out.forkFree);
        if (!stats.saturated() && out.converged)
            BEAST_EXPECT(net.ledgersAgree(target));
        return out;
    }

    void
    testKProfiledOptionNodeMultipliers()
    {
        testcase("K-profiled options preserve per-node multipliers");

        using namespace std::chrono;
        auto const nodeMultipliers =
            HarnessScheduler::ProfiledPacer::NodeMultipliers::single(
                /*nodeId=*/2, /*value=*/4);
        SteppingNetwork::KProfiledOptions const options{
            /*k=*/3, /*unitCost=*/milliseconds{7}, nodeMultipliers};
        auto const pacer = options.pacer();

        BEAST_EXPECT(
            pacer.eventWeight(/*nodeId=*/0, HarnessScheduler::Kind::job) == 3);
        BEAST_EXPECT(
            pacer.eventWeight(/*nodeId=*/2, HarnessScheduler::Kind::job) == 12);
        BEAST_EXPECT(
            pacer.eventCost(/*nodeId=*/2, HarnessScheduler::Kind::job) ==
            milliseconds{252});
        BEAST_EXPECT(pacer.nodeMultipliers.multiplier(2) == 4);
        BEAST_EXPECT(
            pacer.horizonMode ==
            HarnessScheduler::ProfiledPacer::HorizonMode::global);

        auto perNode = options;
        perNode.horizonMode =
            HarnessScheduler::ProfiledPacer::HorizonMode::perNode;
        BEAST_EXPECT(
            perNode.pacer().horizonMode ==
            HarnessScheduler::ProfiledPacer::HorizonMode::perNode);
    }

    [[nodiscard]] static uint256
    encodeSigned(std::int64_t value)
    {
        constexpr std::int64_t bias = 1'000'000'000;
        return uint256{static_cast<std::uint64_t>(value + bias)};
    }

    [[nodiscard]] static std::optional<std::vector<uint256>>
    runPerNodeHorizonScenario(
        beast::unit_test::suite& suite,
        SteppingNetwork& net)
    {
        using namespace std::chrono;

        net.validators(5).mesh();
        if (!suite.expect(
                net.allUp() && net.meshReady(), "per-node K: mesh ready"))
            return std::nullopt;
        net.runTo(3);
        if (!suite.expect(
                net.minValidatedSeq() >= 3, "per-node K: reached warmup"))
            return std::nullopt;

        auto const target = net.minValidatedSeq() + 5;
        std::uint32_t beat = 0;
        std::uint32_t firstFastAheadBeat = 0;
        std::uint32_t maxFastAhead = 0;
        bool sawFastCloseAheadOfSlowClose = false;
        auto const afterBeat = [&]() {
            ++beat;
            auto fastMin = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t i = 1; i < 5; ++i)
                fastMin = std::min(fastMin, net.validSeq(i));
            auto const slow = net.validSeq(0);
            if (fastMin > slow)
            {
                if (firstFastAheadBeat == 0)
                    firstFastAheadBeat = beat;
                maxFastAhead = std::max(maxFastAhead, fastMin - slow);
                auto const fastClose = net.ledgerCloseTime(1, fastMin);
                auto const slowClose = net.ledgerCloseTime(0, slow);
                sawFastCloseAheadOfSlowClose = sawFastCloseAheadOfSlowClose ||
                    (fastClose && slowClose && *fastClose > *slowClose);
            }
        };

        auto options = SteppingNetwork::KProfiledOptions{
            /*k=*/1,
            /*unitCost=*/milliseconds{5},
            HarnessScheduler::ProfiledPacer::NodeMultipliers::single(
                /*nodeId=*/0, /*value=*/20, /*fallback=*/0)};
        options.horizonMode =
            HarnessScheduler::ProfiledPacer::HorizonMode::perNode;

        auto const stats = net.runProfiledTo(
            target,
            options,
            SteppingNetwork::RunBudget{/*heartbeats=*/120, /*steps=*/1'000'000},
            SteppingNetwork::Cadence{
                /*dt=*/seconds{1}, /*skew=*/milliseconds{20}},
            afterBeat);

        auto fastMin = std::numeric_limits<std::uint32_t>::max();
        auto fastMax = std::uint32_t{0};
        for (std::uint32_t i = 1; i < 5; ++i)
        {
            fastMin = std::min(fastMin, net.validSeq(i));
            fastMax = std::max(fastMax, net.validSeq(i));
        }

        auto const zeroLag = HarnessScheduler::duration{};
        auto const lag0 = stats.nodeLag.empty() ? zeroLag : stats.nodeLag[0];
        bool otherLagZero = true;
        for (std::uint32_t i = 1; i < 5; ++i)
        {
            auto const lag =
                i < stats.nodeLag.size() ? stats.nodeLag[i] : zeroLag;
            otherLagZero = otherLagZero && lag == zeroLag;
        }
        bool lagGrew = false;
        auto prevLag = zeroLag;
        for (auto const& beatLag : stats.nodeLagPerBeat)
        {
            auto const current = beatLag.empty() ? zeroLag : beatLag[0];
            if (current > prevLag)
                lagGrew = true;
            prevLag = current;
        }

        auto const slowNow = net.node(0).app().timeKeeper().now();
        auto const slowClose = net.validSeq(0) >= 2
            ? net.ledgerCloseTime(0, net.validSeq(0))
            : std::nullopt;
        auto const fastClose =
            fastMin >= 2 ? net.ledgerCloseTime(1, fastMin) : std::nullopt;
        auto const fastCloseAheadOfSlowNow = fastClose && *fastClose > slowNow;

        suite.log << "  per-node K: lag0Ms=" << asMs(lag0)
                  << ", clamps=" << stats.clampHits
                  << ", slowValid=" << net.validSeq(0)
                  << ", fastMin=" << fastMin << ", fastMax=" << fastMax
                  << ", firstFastAheadBeat=" << firstFastAheadBeat
                  << ", maxFastAhead=" << maxFastAhead
                  << ", slowNowSec=" << slowNow.time_since_epoch().count()
                  << ", slowCloseSec="
                  << (slowClose ? slowClose->time_since_epoch().count() : -1)
                  << ", fastCloseSec="
                  << (fastClose ? fastClose->time_since_epoch().count() : -1)
                  << ", forkFree=" << net.validatedForkFree() << std::endl;

        bool ok = true;
        ok &=
            suite.expect(stats.clampHits != 0, "per-node K: saturated node 0");
        ok &=
            suite.expect(lag0 > zeroLag, "per-node K: node 0 accumulated lag");
        ok &= suite.expect(
            otherLagZero, "per-node K: fast nodes accumulated no lag");
        ok &= suite.expect(
            slowNow == NetClock::time_point{},
            "per-node K: excessive lag stops at epoch");
        ok &= suite.expect(lagGrew, "per-node K: node 0 lag grew across beats");
        ok &= suite.expect(
            fastMin >= target, "per-node K: fast quorum reached target");
        // Timestamp pressure permits later catch-up. The ledger-lag witness
        // belongs to the observed history, not necessarily the final sample.
        ok &= suite.expect(
            maxFastAhead != 0, "per-node K: node 0 was observed behind");
        ok &= suite.expect(
            firstFastAheadBeat != 0, "per-node K: fast quorum outran node 0");
        ok &= suite.expect(
            net.validatedAgree({1, 2, 3, 4}, fastMin),
            "per-node K: fast quorum agreed at its advanced seq");
        ok &= suite.expect(
            sawFastCloseAheadOfSlowClose,
            "per-node K: quorum validated beyond node 0's validated close "
            "time");
        ok &= suite.expect(
            fastCloseAheadOfSlowNow,
            "per-node K: quorum validated a ledger beyond node 0's observed "
            "clock");
        ok &= suite.expect(
            net.validatedForkFree(), "per-node K: no validated fork");
        ok &= suite.expect(
            net.offThreadJobs() == 0, "per-node K: no off-thread jobs");
        ok &= suite.expect(net.failedJobs() == 0, "per-node K: no failed jobs");
        if (!ok)
            return std::nullopt;

        return std::vector<uint256>{
            uint256{static_cast<std::uint64_t>(asMs(lag0))},
            uint256{stats.clampHits},
            uint256{net.validSeq(0)},
            uint256{fastMin},
            uint256{fastMax},
            uint256{firstFastAheadBeat},
            uint256{maxFastAhead},
            encodeSigned(slowNow.time_since_epoch().count()),
            encodeSigned(slowClose->time_since_epoch().count()),
            encodeSigned(fastClose->time_since_epoch().count())};
    }

    void
    testKProfiledPerNodeHorizon()
    {
        testcase(
            "K-profiled per-node horizon: lagged owner sees stale NetClock "
            "while quorum advances fork-free");
        expectReplays(
            *this, "per-node K horizon", [this](SteppingNetwork& net) {
                return runPerNodeHorizonScenario(*this, net);
            });
    }

    struct KZeroHorizonSample
    {
        std::vector<uint256> chain;
        std::uint64_t fingerprint = 0;
        std::uint64_t events = 0;

        [[nodiscard]] bool
        operator==(KZeroHorizonSample const& o) const
        {
            return chain == o.chain && fingerprint == o.fingerprint &&
                events == o.events;
        }
    };

    [[nodiscard]] KZeroHorizonSample
    runKZeroHorizonScenario(
        HarnessScheduler::ProfiledPacer::HorizonMode horizonMode)
    {
        using namespace std::chrono;
        constexpr std::uint32_t target = 4;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return {};

        auto options = SteppingNetwork::KProfiledOptions{
            /*k=*/0,
            /*unitCost=*/milliseconds{5},
            HarnessScheduler::ProfiledPacer::NodeMultipliers::single(
                /*nodeId=*/0, /*value=*/100)};
        options.horizonMode = horizonMode;

        auto const stats = net.runProfiledTo(
            target,
            options,
            SteppingNetwork::RunBudget{
                /*heartbeats=*/160, /*steps=*/1'000'000});

        BEAST_EXPECT(stats.clampHits == 0);
        BEAST_EXPECT(stats.nodeLag.empty());
        BEAST_EXPECT(net.minValidatedSeq() >= target);
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);

        KZeroHorizonSample out;
        out.fingerprint = net.traceFingerprint();
        out.events = net.traceCount();
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            out.chain.push_back(net.ledgerHash(0, seq));
        return out;
    }

    void
    testKProfiledPerNodeKZeroInert()
    {
        testcase("K-profiled per-node horizon: K=0 is inert");

        using HorizonMode = HarnessScheduler::ProfiledPacer::HorizonMode;
        auto const global = runKZeroHorizonScenario(HorizonMode::global);
        auto const perNode = runKZeroHorizonScenario(HorizonMode::perNode);
        BEAST_EXPECT(perNode == global);
    }

    void
    testKProfiledPacerDeterminism()
    {
        testcase(
            "K-profiled pacer: K=0 inert, weighted K sweep pinned, stable, "
            "fork-free");

        static constexpr KProfiledSample kExpected[] = {
            {0,
             kCanaryFingerprint,
             kCanaryEvents,
             kCanaryEvents,
             0,
             4,
             4,
             3,
             0,
             0,
             0,
             0,
             42010,
             40,
             true,
             true,
             0,
             0,
             0,
             0,
             0,
             0},
            {1,     0xa46f06e1f6ed2b0dull,
             330,   330,
             42,    4,
             4,     3,
             0,     3420,
             3420,  375,
             42315, 40,
             true,  true,
             684,   126,
             54,    150,
             0,     0},
            {4,     0x703626cd73acab29ull,
             330,   330,
             43,    4,
             4,     3,
             3,     13680,
             13620, 1000,
             43240, 40,
             true,  true,
             684,   126,
             54,    150,
             0,     3},
            {16,    0xb273bcb2871f6db1ull,
             451,   451,
             71,    4,
             4,     3,
             40,    73040,
             69040, 1000,
             71240, 40,
             true,  true,
             913,   192,
             50,    203,
             6,     2},
            {64,     0x3ea4ef48fc47a7a0ull,
             451,    451,
             160,    0,
             0,      0,
             160,    254400,
             160000, 1000,
             161000, -1,
             true,   false,
             795,    270,
             12,     163,
             6,      3},
        };

        bool sawSaturation = false;
        for (auto const& expected : kExpected)
        {
            auto const first = runKProfiledScenario(expected.k);
            BEAST_EXPECT(first == expected);
            BEAST_EXPECT(first.forkFree);
            if (expected.k == 0)
            {
                BEAST_EXPECT(first.fingerprint == kCanaryFingerprint);
                BEAST_EXPECT(first.events == kCanaryEvents);
                BEAST_EXPECT(first.converged);
            }
            sawSaturation = sawSaturation || first.clampHits != 0;

            for (int replay = 2; replay <= 3; ++replay)
            {
                auto const next = runKProfiledScenario(expected.k);
                BEAST_EXPECT(next == expected);
                BEAST_EXPECT(next == first);
                BEAST_EXPECT(next.forkFree);
            }
        }
        BEAST_EXPECT(sawSaturation);
    }

    // The environment-DI wiring contract for the two elapsed-time domains
    // (issue 005 / codex round-6): production keeps TWO distinct steady
    // clocks — the cached-seconds stopwatch() and the raw full-resolution
    // steady clock (peer RTT precision lives on the second) — while a
    // stepping node collapses BOTH accessors onto its one injected manual
    // clock, which advances with virtual time and never with wall time.
    void
    testEnvironmentClockIdentity()
    {
        testcase("one injected clock serves both elapsed-time domains");

        // The two production globals are distinct instances: routing RTT
        // through getStopwatch() would quantize it to cached seconds,
        // which is exactly why getPreciseStopwatch() exists.
        BEAST_EXPECT(
            &stopwatch() !=
            &beast::get_abstract_clock<std::chrono::steady_clock>());

        SteppingNetwork net(*this);
        net.validators(2).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return;
        for (std::uint32_t i = 0; i < 2; ++i)
        {
            auto& app = net.node(i).app();
            BEAST_EXPECT(&app.getStopwatch() == &app.getPreciseStopwatch());
        }
        auto const t0 = net.node(0).app().getPreciseStopwatch().now();
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return;
        // Virtual time moved the precise clock; no wall sleep occurred.
        BEAST_EXPECT(net.node(0).app().getPreciseStopwatch().now() > t0);
    }

    // 005 slice 4, closed: the PeerImp 60s heartbeat — the LAST timer that
    // used to be gated off under stepping — fires as a virtual Tier::timer
    // event, and the ping/pong RTT it measures is pure virtual-time
    // physics: 5ms out + 5ms back = exactly 10ms, every peer, every run.
    // Before the seam, latency_ was never populated in stepping at all
    // (the arm site was gated); a wall-clock leak here would read as
    // machine-speed milliseconds and break both the equality and the
    // replay.
    void
    testVirtualPeerHeartbeat()
    {
        testcase(
            "the peer heartbeat lives on the virtual timeline: ping RTT == "
            "2 x link delay, bit-for-bit replays");
        expectReplays(
            *this,
            "virtual peer heartbeat",
            [this](SteppingNetwork& net) {
                using Payload = std::optional<std::vector<uint256>>;
                net.validators(2).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};
                // The heartbeat arms at doProtocolStart and fires at
                // virtual +60s; the pong lands 10ms later. Poll, don't
                // count beats.
                auto const allLatenciesMeasured = [&net]() {
                    for (std::uint32_t i = 0; i < 2; ++i)
                        for (auto const& p :
                             net.node(i).app().overlay().getActivePeers())
                            if (!p->json().isMember(jss::latency))
                                return false;
                    return true;
                };
                if (!BEAST_EXPECT(net.runUntil(
                        allLatenciesMeasured,
                        SteppingNetwork::RunBudget{/*heartbeats=*/70})))
                    return Payload{};
                std::vector<uint256> out;
                for (std::uint32_t i = 0; i < 2; ++i)
                    for (auto const& p :
                         net.node(i).app().overlay().getActivePeers())
                    {
                        auto const ms = p->json()[jss::latency].asUInt();
                        BEAST_EXPECT(ms == 10);  // 2 x 5ms link delay
                        out.push_back(uint256{ms});
                    }
                out.push_back(net.ledgerHash(0, 3));
                return Payload{std::move(out)};
            },
            /*minRuns=*/2);
    }

public:
    void
    run() override
    {
        testConvergence();
        testReproducibleChain();
        testTimelineCanary();
        testKProfiledOptionNodeMultipliers();
        testKProfiledPacerDeterminism();
        testKProfiledPerNodeHorizon();
        testKProfiledPerNodeKZeroInert();
        testEnvironmentClockIdentity();
        testVirtualPeerHeartbeat();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingDeterminism, consensus, ripple);

}  // namespace ripple::test
