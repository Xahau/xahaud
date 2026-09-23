//------------------------------------------------------------------------------
// SteppingFaults — §5.7 wire-altitude fault injection: a byzantine NETWORK
// (drop / duplicate / delay at the pipe, per direction, per message),
// deterministically. No prod surface: the injector runs in SimPipe at the
// write boundary, where one write is exactly one framed protocol message.
//
// The assertions follow the harness's standing pattern: the honest network
// converges DESPITE the fault (quorum math absorbs it), the structural
// invariants hold (offThreadJobs==0, failedJobs==0 — a fault that surfaces
// an unmodeled closure fail-louds, which is a discovery, not a flake), and
// the ENTIRE faulted run replays bit-for-bit (trace fingerprints equal) —
// faults are part of the timeline, not noise on top of it.
//
// Quorum arithmetic for the drop cases: quorum(5) = ⌈0.8·5⌉ = 4, so a node
// deprived of ONE validator's validations still reaches quorum from the
// other three plus its own.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>

#include <ripple.pb.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingFaults_test : public beast::unit_test::suite
{
    // Five validators, steady state to seq 3, then `installFaults` runs at a
    // stepping boundary and the network is driven three more ledgers under
    // the fault. Cross-run identity (chains + executed order, K runs) is
    // asserted by expectReplays — a faulted run replays bit-for-bit or the
    // ladder prints why not.
    void
    assertReplays(
        char const* label,
        std::function<void(SteppingNetwork&)> const& installFaults)
    {
        expectReplays(
            *this, label, [this, &installFaults](SteppingNetwork& net) {
                using Payload = std::optional<std::vector<uint256>>;
                net.validators(5).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};

                net.runTo(3);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                    return Payload{};

                installFaults(net);

                auto const target = net.minValidatedSeq() + 3;
                net.runTo(target);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
                {
                    log << "  diagnostics: " << net.jobDiagnostics()
                        << std::endl;
                    return Payload{};
                }
                BEAST_EXPECT(net.ledgersAgree(target));
                BEAST_EXPECT(net.offThreadJobs() == 0);
                BEAST_EXPECT(net.failedJobs() == 0);

                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= target; ++seq)
                    chain.push_back(net.ledgerHash(0, seq));
                return Payload{std::move(chain)};
            });
    }

    void
    testDropValidationsOneDirection()
    {
        testcase(
            "node 4 never hears node 0's validations; 3 peers + self still "
            "make quorum(5)=4, and the faulted run replays");
        assertReplays("drop validations 0->4", [](SteppingNetwork& net) {
            net.faultLink(0, 4, simfaults::dropType(protocol::mtVALIDATION));
        });
    }

    void
    testDuplicateStorm()
    {
        testcase(
            "every proposal and validation from node 0 arrives in triplicate "
            "on every link; dedup absorbs the storm, and the run replays");
        assertReplays("duplicate storm from 0", [](SteppingNetwork& net) {
            for (std::uint32_t to = 1; to < 5; ++to)
            {
                net.faultLink(0, to, [](std::uint16_t t, std::size_t) {
                    SimFault f;
                    if (t == protocol::mtVALIDATION ||
                        t == protocol::mtPROPOSE_LEDGER)
                        f.duplicates = 2;
                    return f;
                });
            }
        });
    }

    void
    testSeededRandomLoss()
    {
        testcase(
            "10% seeded random loss on every link out of node 0; the engine "
            "is the replay handle — same seed, same faults, same run");
        // The engines must outlive the injectors (captured by reference) —
        // and be re-seeded identically for the replay run, so they are
        // created inside the installer from fixed seeds.
        struct Engines
        {
            std::vector<std::unique_ptr<beast::xor_shift_engine>> kept;
        };
        auto engines = std::make_shared<Engines>();
        assertReplays("10% loss from 0", [engines](SteppingNetwork& net) {
            for (std::uint32_t to = 1; to < 5; ++to)
            {
                engines->kept.push_back(
                    std::make_unique<beast::xor_shift_engine>(
                        0xFA017EED0000ull + to));
                net.faultLink(
                    0,
                    to,
                    simfaults::dropWithProbability(
                        *engines->kept.back(), 0.10));
            }
        });
    }

    void
    testDelayReorder()
    {
        testcase(
            "+50ms in-flight latency on everything node 0 sends to node 4 "
            "(10x the link delay — messages reorder past faster paths); "
            "converges and replays");
        assertReplays("delay 0->4 by 50ms", [](SteppingNetwork& net) {
            net.faultLink(
                0, 4, simfaults::delayAll(std::chrono::milliseconds{50}));
        });
    }

    void
    testSeededJitter()
    {
        testcase(
            "seeded per-MESSAGE jitter (1..40ms) on every link out of node 0 "
            "— each packet a fresh seeded delay; the engine is the replay "
            "handle, so random packet times reproduce bit-for-bit");
        // The engines must outlive the injectors (captured by reference) and
        // be re-seeded identically per replay run, so they are built inside
        // the installer from fixed seeds — the dropWithProbability pattern.
        struct Engines
        {
            std::vector<std::unique_ptr<beast::xor_shift_engine>> kept;
        };
        auto engines = std::make_shared<Engines>();
        assertReplays("40ms jitter from 0", [engines](SteppingNetwork& net) {
            using namespace std::chrono;
            for (std::uint32_t to = 1; to < 5; ++to)
            {
                engines->kept.push_back(
                    std::make_unique<beast::xor_shift_engine>(
                        0xD341A17700ull + to));
                net.faultLink(
                    0,
                    to,
                    simfaults::delayJittered(
                        *engines->kept.back(),
                        milliseconds{1},
                        milliseconds{40}));
            }
        });
    }

    // The safety oracle lives on the wrapper now (promoted so the
    // explorer's edge mode uses the same definition): see
    // SteppingNetwork::validatedForkFree().

    // The fault ENVELOPE (plan §5.7 follow-on): the gentle cases above prove
    // the absorbed regime; this one escalates seeded loss on EVERY directed
    // link until convergence FAILS within budget, and asserts the failure
    // MODE — liveness lost (validated progress stalls), safety kept (no
    // validated fork), structure intact (no off-thread/unmodeled jobs) —
    // then heals and asserts full recovery. 100% loss is the guaranteed
    // stall backstop: no external validations ⇒ 1 < quorum(5)=4 everywhere.
    void
    testLossEnvelope()
    {
        testcase(
            "escalating seeded loss on every link: light rungs absorbed, a "
            "heavy rung stalls WITHOUT forking, heal recovers — loss costs "
            "liveness, never safety");
        constexpr std::uint32_t n = 5;
        // Engines must outlive the injectors that capture them by reference,
        // and be re-created from fixed seeds per run so replays draw the
        // same streams (same pattern as testSeededRandomLoss).
        struct Engines
        {
            std::vector<std::unique_ptr<beast::xor_shift_engine>> kept;
        };
        auto engines = std::make_shared<Engines>();
        expectReplays(
            *this, "loss envelope", [this, engines](SteppingNetwork& net) {
                using Payload = std::optional<std::vector<uint256>>;
                net.validators(n).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};
                net.runTo(3);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                    return Payload{};

                auto const faultAllLinks = [&](int pct, int rung) {
                    for (std::uint32_t a = 0; a < n; ++a)
                        for (std::uint32_t b = 0; b < n; ++b)
                        {
                            if (a == b)
                                continue;
                            engines->kept.push_back(
                                std::make_unique<beast::xor_shift_engine>(
                                    0xFA017E27E10Full +
                                    static_cast<std::uint64_t>(rung) * 1000 +
                                    a * 10 + b));
                            net.faultLink(
                                a,
                                b,
                                simfaults::dropWithProbability(
                                    *engines->kept.back(), pct / 100.0));
                        }
                };

                constexpr int rungPct[] = {25, 50, 75, 100};
                int stalledPct = 0;
                for (int r = 0; r < 4; ++r)
                {
                    faultAllLinks(rungPct[r], r);
                    auto const pre = net.minValidatedSeq();
                    auto const target = pre + 2;
                    net.runTo(
                        target, SteppingNetwork::RunBudget{/*heartbeats=*/15});
                    // Safety + structure hold at EVERY rung, absorbed or not.
                    if (!BEAST_EXPECT(net.validatedForkFree()))
                        return Payload{};
                    BEAST_EXPECT(net.offThreadJobs() == 0);
                    BEAST_EXPECT(net.failedJobs() == 0);
                    if (net.minValidatedSeq() >= target)
                    {
                        log << "  " << rungPct[r] << "% loss: absorbed "
                            << "(validated " << pre << " -> "
                            << net.minValidatedSeq() << ")" << std::endl;
                        continue;
                    }
                    stalledPct = rungPct[r];
                    log << "  " << rungPct[r] << "% loss: STALLED — validated "
                        << "held at " << net.minValidatedSeq()
                        << "; closed/valid per node:";
                    for (std::uint32_t i = 0; i < n; ++i)
                        log << " n" << i << "=" << net.closedSeq(i) << "/"
                            << net.validSeq(i);
                    log << std::endl;
                    break;
                }
                // The escalation must FIND the envelope's edge (100% loss
                // cannot converge: 1 validation < quorum(5)=4 everywhere).
                if (!BEAST_EXPECT(stalledPct > 0))
                    return Payload{};

                // Heal: clear every injector, run, and full validation must
                // resume across all five — through the modeled acquire path
                // where loss left nodes behind.
                for (std::uint32_t a = 0; a < n; ++a)
                    for (std::uint32_t b = 0; b < n; ++b)
                        if (a != b)
                            net.faultLink(a, b, {});
                auto const target = net.minValidatedSeq() + 3;
                net.runTo(target);
                if (!net.expectConverged(target))
                    return Payload{};
                BEAST_EXPECT(net.validatedForkFree());
                for (std::uint32_t i = 0; i < n; ++i)
                    BEAST_EXPECT(net.mode(i) == OperatingMode::FULL);
                log << "  healed: validated " << net.minValidatedSeq()
                    << ", all nodes FULL" << std::endl;

                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= target; ++seq)
                    chain.push_back(net.ledgerHash(0, seq));
                return Payload{std::move(chain)};
            });
    }

    // The 005 seam, exercised for REAL (codex round-6 test strategy): a
    // stalled ledger acquire's TimeoutCounter retries fire as virtual
    // Tier::timer events at their 3s cadence (kLedgerAcquireTimeout;
    // TransactionAcquire's is the 250ms one) — deterministically, with
    // the retry visible in the executed trace — then the acquire completes
    // once the data path heals. Shape: isolate node 4 while the quorum(5)=4
    // cohort validates past it; partially heal so node 4 HEARS validations
    // (acquire triggers, requests go out) but every acquire REPLY into it
    // is dropped (TMLedgerData + fetch-pack objects) — the retry loop is
    // the only thing that can spin; then heal fully and require byte-exact
    // catch-up. Before the seam landed this scenario had NO retry at all
    // (the gate suppressed arming); the non-vacuity gate below fails if the
    // retries ever stop being reachable.
    void
    testAcquireRetryCadence()
    {
        testcase(
            "a stalled acquire retries on the virtual timeline (005 seam): "
            "retry timer events at their virtual 3s cadence, acquire "
            "completes after "
            "heal, bit-for-bit replay");
        constexpr std::uint32_t n = 5;
        constexpr std::uint32_t lag = 4;
        expectReplays(
            *this, "acquire retry cadence", [this](SteppingNetwork& net) {
                using Payload = std::optional<std::vector<uint256>>;
                using namespace std::chrono;
                net.validators(n).mesh();
                if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                    return Payload{};
                net.runTo(3);
                if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                    return Payload{};

                auto const dropEverything = [](std::uint16_t, std::size_t) {
                    SimFault f;
                    f.drop = true;
                    return f;
                };
                // Phase 1 — isolate node 4 completely; the cohort is exactly
                // quorum, so VALIDATED advances while node 4 falls behind.
                for (std::uint32_t a = 0; a < n - 1; ++a)
                {
                    net.faultLink(a, lag, dropEverything);
                    net.faultLink(lag, a, dropEverything);
                }
                auto const pre = net.minValidatedSeq();
                auto const cohortAhead = pre + 2;
                // Poll, don't guess: isolation slows the cohort's rounds (a
                // lost proposer re-rounds at 2-3s virtual), so the budget is
                // a CEILING and the predicate is the success condition.
                auto const cohortValidatedAhead = [&net, cohortAhead]() {
                    for (std::uint32_t i = 0; i < lag; ++i)
                        if (net.validSeq(i) < cohortAhead)
                            return false;
                    return true;
                };
                if (!BEAST_EXPECT(net.runUntil(
                        cohortValidatedAhead,
                        SteppingNetwork::RunBudget{/*heartbeats=*/15})))
                    return Payload{};
                if (!BEAST_EXPECT(net.validSeq(lag) < cohortAhead))
                    return Payload{};

                // Phase 2 — partial heal: node 4 hears the network again
                // (validations for ledgers it lacks -> acquire fires, its
                // requests go OUT), but every acquire reply INTO it is
                // dropped. The only motion available is the retry loop.
                auto const dropAcquireReplies = [](std::uint16_t t,
                                                   std::size_t) {
                    SimFault f;
                    f.drop = t ==
                            static_cast<std::uint16_t>(
                                 protocol::mtLEDGER_DATA) ||
                        t ==
                            static_cast<std::uint16_t>(protocol::mtGET_OBJECTS);
                    return f;
                };
                for (std::uint32_t a = 0; a < n - 1; ++a)
                {
                    net.faultLink(a, lag, dropAcquireReplies);
                    net.faultLink(lag, a, {});
                }
                auto const retriesBefore = countRetryEvents(net, lag);
                // NON-VACUITY (the rung-2b discipline), as a POLL: drive
                // until the starved acquire has demonstrably SPUN the retry
                // loop — two expiries at InboundLedger's virtual 3s interval
                // need ~7s of starvation, so the ceiling is 12 beats and the
                // poll exits the moment the claim holds. A timing change
                // that lets the acquire complete (or never start) means this
                // never returns true — fail it rather than let the scenario
                // silently demote.
                auto const retryLoopSpun = [&net, retriesBefore]() {
                    return countRetryEvents(net, lag) - retriesBefore >= 2;
                };
                if (!BEAST_EXPECT(net.runUntil(
                        retryLoopSpun,
                        SteppingNetwork::RunBudget{/*heartbeats=*/12})))
                    return Payload{};
                auto const retriesDuringStall =
                    countRetryEvents(net, lag) - retriesBefore;
                BEAST_EXPECT(net.offThreadJobs() == 0);
                BEAST_EXPECT(net.failedJobs() == 0);

                // Phase 3 — full heal; node 4 must acquire the missed chain
                // and the network reconverges byte-exact.
                for (std::uint32_t a = 0; a < n - 1; ++a)
                    net.faultLink(a, lag, {});
                auto const target = net.minValidatedSeq() + 3;
                net.runTo(target);
                // The laggard jumps to the cohort's validated TIP first and
                // backfills the gap BEHIND it asynchronously (the 002/004
                // machinery: doAdvance -> fetchForHistory on natural
                // triggers). Byte-agreement at `target` is therefore a
                // condition to POLL for, not to sample at one instant.
                auto const allServeTarget = [&net, target]() {
                    return net.ledgersAgree(target);
                };
                if (!BEAST_EXPECT(net.runUntil(
                        allServeTarget,
                        SteppingNetwork::RunBudget{/*heartbeats=*/10})))
                {
                    for (std::uint32_t i = 0; i < n; ++i)
                        log << "  n" << i << " closed=" << net.closedSeq(i)
                            << " valid=" << net.validSeq(i) << " hash@"
                            << target << "="
                            << to_string(net.ledgerHash(i, target))
                                   .substr(0, 12)
                            << std::endl;
                    log << "  diagnostics: " << net.jobDiagnostics()
                        << std::endl;
                    return Payload{};
                }
                if (!net.expectConverged(target))
                    return Payload{};
                BEAST_EXPECT(net.ledgersAgree(target));
                log << "  retries during starvation: " << retriesDuringStall
                    << "; node " << lag << " validated " << net.validSeq(lag)
                    << std::endl;

                std::vector<uint256> chain;
                for (std::uint32_t seq = 2; seq <= target; ++seq)
                    chain.push_back(net.ledgerHash(0, seq));
                // Fold the retry count and the laggard's tip into the replay
                // payload: a run where the retry cadence shifts is a
                // DIVERGENCE, not a silently-equal outcome.
                chain.push_back(uint256{retriesDuringStall});
                chain.push_back(net.ledgerHash(lag, target));
                return Payload{std::move(chain)};
            });
    }

    // Executed retry expiries owned by `node`: Tier::timer events the
    // injected SteppingTimeoutCounterTimer scheduled (by label).
    [[nodiscard]] static std::uint64_t
    countRetryEvents(SteppingNetwork& net, std::uint32_t node)
    {
        std::uint64_t count = 0;
        for (auto const& ev : net.controller().scheduler().traceLog())
            if (ev.nodeId == node && ev.kind == HarnessScheduler::Kind::timer &&
                ev.label == "TimeoutCounter retry")
                ++count;
        return count;
    }

public:
    void
    run() override
    {
        testDropValidationsOneDirection();
        testDuplicateStorm();
        testSeededRandomLoss();
        testAcquireRetryCadence();
        testDelayReorder();
        testSeededJitter();
        testLossEnvelope();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingFaults, consensus, ripple);

}  // namespace ripple::test
