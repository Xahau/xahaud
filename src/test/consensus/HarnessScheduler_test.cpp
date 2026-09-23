//------------------------------------------------------------------------------
// HarnessScheduler unit suite (Stage 3, S3.5) — proves the virtual-time event
// scheduler's determinism contract in ISOLATION, with no Application/overlay
// machinery, so it compiles + runs fast and pins the ordering guarantees the
// stepping harness depends on:
//   - events fire in (when, tier, nodeId, seq) order and now() tracks them;
//   - same-instant cascades are totally + stably ordered (the csf gap we
//   close);
//   - a handler-driven cascade replays identically across independent runs;
//   - cancel / stepUntil / stepWhile behave as specified.
// Spec: .ai-docs/specs/csf-peerimp-hybrid-overlay-harness.md (Stage 3).
//------------------------------------------------------------------------------
#include <test/jtx/HarnessScheduler.h>

#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace ripple::test {

class HarnessScheduler_test : public beast::unit_test::suite
{
    using Tier = HarnessScheduler::Tier;
    using ms = std::chrono::milliseconds;

    // A common cascade used by both reproducibility runs: each fired event may
    // schedule follow-ups, the way a delivered message triggers a reply.
    // Records a textual trace of the order events actually ran in.
    static void
    runCascade(HarnessScheduler& s, std::vector<std::string>& trace)
    {
        auto const t0 = s.now();
        // Two nodes "deliver" at the same instant; each delivery, when it runs,
        // schedules a "process" 5ms later on the same node, which in turn
        // schedules one more delivery to the other node 5ms after that.
        for (std::uint32_t node :
             {1u, 0u})  // intentionally out of nodeId order
        {
            s.at(t0 + ms{10}, Tier::deliver, node, [&, node]() {
                trace.push_back("deliver@n" + std::to_string(node));
                auto const t = s.now();
                s.at(t + ms{5}, Tier::process, node, [&, node]() {
                    trace.push_back("process@n" + std::to_string(node));
                    std::uint32_t const other = node ^ 1u;
                    s.at(s.now() + ms{5}, Tier::deliver, other, [&, other]() {
                        trace.push_back("reply->n" + std::to_string(other));
                    });
                });
            });
        }
    }

    void
    testTimeOrdering()
    {
        testcase("events fire in virtual-time order; now() tracks them");
        HarnessScheduler s;
        std::vector<int> order;
        auto const t0 = s.now();
        // Schedule out of time order.
        s.at(t0 + ms{30}, Tier::deliver, 0, [&]() { order.push_back(30); });
        s.at(t0 + ms{10}, Tier::deliver, 0, [&]() { order.push_back(10); });
        s.at(t0 + ms{20}, Tier::deliver, 0, [&]() { order.push_back(20); });

        BEAST_EXPECT(s.size() == 3);
        BEAST_EXPECT(s.now() == t0);  // scheduling does not advance time

        BEAST_EXPECT(s.stepOne() && s.now() == t0 + ms{10});
        BEAST_EXPECT(s.stepOne() && s.now() == t0 + ms{20});
        BEAST_EXPECT(s.stepOne() && s.now() == t0 + ms{30});
        BEAST_EXPECT(!s.stepOne());  // empty

        BEAST_EXPECT((order == std::vector<int>{10, 20, 30}));
    }

    void
    testSameInstantTieBreak()
    {
        testcase("same-instant events ordered by (tier, nodeId, seq)");
        HarnessScheduler s;
        std::vector<std::string> order;
        auto const t = s.now() + ms{10};  // everything at the SAME instant

        // Deliberately scheduled in a SCRAMBLED order; the key must sort them.
        s.at(t, Tier::heartbeat, 0, [&]() { order.push_back("hb.n0"); });
        s.at(t, Tier::deliver, 1, [&]() { order.push_back("dl.n1"); });
        s.at(t, Tier::deliver, 0, [&]() { order.push_back("dl.n0.a"); });
        s.at(t, Tier::deliver, 0, [&]() {
            order.push_back("dl.n0.b");
        });  // FIFO
        s.at(t, Tier::process, 0, [&]() { order.push_back("pr.n0"); });

        s.step();

        // tier asc (deliver<process<heartbeat), then nodeId asc, then seq
        // (FIFO):
        std::vector<std::string> const want{
            "dl.n0.a", "dl.n0.b", "dl.n1", "pr.n0", "hb.n0"};
        BEAST_EXPECT(order == want);
        BEAST_EXPECT(s.now() == t);
    }

    void
    testReproducibleCascade()
    {
        testcase("handler-driven cascade replays identically across runs");
        std::vector<std::string> trace1, trace2;
        {
            HarnessScheduler s;
            runCascade(s, trace1);
            s.step();
        }
        {
            HarnessScheduler s;
            runCascade(s, trace2);
            s.step();
        }
        BEAST_EXPECT(!trace1.empty());
        BEAST_EXPECT(trace1 == trace2);

        // And the order is exactly what the (tier,nodeId,seq) key dictates:
        // both deliveries at t+10 (node 0 before node 1 by nodeId, despite node
        // 1 being scheduled first), then their processes at t+15, then the two
        // replies at t+20 (both deliver-tier, so again node 0 before node 1 by
        // nodeId).
        std::vector<std::string> const want{
            "deliver@n0",
            "deliver@n1",
            "process@n0",
            "process@n1",
            "reply->n0",
            "reply->n1"};
        BEAST_EXPECT(trace1 == want);
    }

    void
    testCancel()
    {
        testcase("cancel removes a not-yet-fired event");
        HarnessScheduler s;
        std::vector<int> order;
        auto const t0 = s.now();
        s.at(t0 + ms{10}, Tier::deliver, 0, [&]() { order.push_back(1); });
        auto tok =
            s.at(t0 + ms{20}, Tier::deliver, 0, [&]() { order.push_back(2); });
        s.at(t0 + ms{30}, Tier::deliver, 0, [&]() { order.push_back(3); });

        s.cancel(tok);
        BEAST_EXPECT(s.size() == 2);
        s.step();
        BEAST_EXPECT((order == std::vector<int>{1, 3}));
    }

    void
    testStepUntilAndWhile()
    {
        testcase("stepUntil runs only due events; stepWhile honors predicate");
        {
            HarnessScheduler s;
            std::vector<int> order;
            auto const t0 = s.now();
            s.at(t0 + ms{10}, Tier::deliver, 0, [&]() { order.push_back(10); });
            s.at(t0 + ms{20}, Tier::deliver, 0, [&]() { order.push_back(20); });
            s.at(t0 + ms{30}, Tier::deliver, 0, [&]() { order.push_back(30); });

            bool const remain = s.stepUntil(t0 + ms{20});
            BEAST_EXPECT(remain);  // the t+30 event is left
            BEAST_EXPECT(s.now() == t0 + ms{20});
            BEAST_EXPECT((order == std::vector<int>{10, 20}));

            // stepUntil past the end advances now() even with an empty queue.
            s.stepUntil(t0 + ms{100});
            BEAST_EXPECT(s.now() == t0 + ms{100});
            BEAST_EXPECT((order == std::vector<int>{10, 20, 30}));
            BEAST_EXPECT(s.empty());
        }
        {
            HarnessScheduler s;
            int ran = 0;
            auto const t0 = s.now();
            for (int i = 1; i <= 5; ++i)
                s.at(t0 + ms{i}, Tier::deliver, 0, [&]() { ++ran; });
            // Stop after 3 events.
            s.stepWhile([&]() { return ran < 3; });
            BEAST_EXPECT(ran == 3);
            BEAST_EXPECT(s.size() == 2);
        }
    }

    void
    testAtOrNowClamp()
    {
        testcase(
            "atOrNow clamps a stale deadline up to now() (at() requires "
            "future)");
        HarnessScheduler s;
        std::vector<int> order;
        auto const t0 = s.now();

        // Move virtual time forward to t0+50 by stepping a future event.
        s.at(t0 + ms{50}, Tier::deliver, 0, [&]() { order.push_back(50); });
        BEAST_EXPECT(s.stepOne() && s.now() == t0 + ms{50});

        // A timer whose virtual deadline (t0+10) already elapsed: atOrNow
        // clamps it up to now() (t0+50) instead of rewinding the clock. at()
        // would assert.
        s.atOrNow(t0 + ms{10}, Tier::timer, 0, [&]() { order.push_back(10); });
        // A genuinely future event still orders strictly after the clamped one.
        s.at(s.now() + ms{5}, Tier::deliver, 0, [&]() { order.push_back(55); });

        s.step();
        BEAST_EXPECT((order == std::vector<int>{50, 10, 55}));
        BEAST_EXPECT(s.now() == t0 + ms{55});
    }

    void
    testAtRejectsPast()
    {
        testcase(
            "at() throws on a past deadline in EVERY build (not just debug)");
        HarnessScheduler s;
        auto const t0 = s.now();
        s.at(t0 + ms{50}, Tier::deliver, 0, []() {});
        BEAST_EXPECT(s.stepOne() && s.now() == t0 + ms{50});

        // Scheduling into the past must throw — and this must hold in Release,
        // where XRPL_ASSERT would have been stripped (the determinism proofs
        // run in Release), so a stripped assert would have let now_ rewind.
        BEAST_EXPECT(except<std::logic_error>(
            [&]() { s.at(t0 + ms{10}, Tier::deliver, 0, []() {}); }));
        // The rejected schedule left no event behind…
        BEAST_EXPECT(s.empty());
        // …while atOrNow() tolerates the same stale deadline (clamps to now()).
        s.atOrNow(t0 + ms{10}, Tier::timer, 0, []() {});
        BEAST_EXPECT(s.size() == 1);
    }

    void
    testProfiledPacerChargeAndClamp()
    {
        testcase(
            "K-profiled pacer charges before handlers and clamps at horizon");

        HarnessScheduler s;
        auto const t0 = s.now();
        std::vector<int> observedMs;
        s.setRecordTrace(true);

        for (std::uint32_t node : {0u, 1u})
        {
            s.at(t0 + ms{10}, Tier::deliver, node, [&]() {
                observedMs.push_back(static_cast<int>(
                    std::chrono::duration_cast<ms>(s.now() - t0).count()));
            });
        }

        HarnessScheduler::ProfiledPacer const pacer{
            /*k=*/2,
            /*unitCost=*/ms{3},
            HarnessScheduler::ProfiledPacer::KindWeights::flat(),
            HarnessScheduler::ProfiledPacer::NodeMultipliers{},
            HarnessScheduler::ProfiledPacer::HorizonMode::global};
        HarnessScheduler::ProfiledStepStats stats;
        auto const horizon = t0 + ms{20};

        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(s.now() == t0 + ms{16});
        BEAST_EXPECT(!stats.saturated);

        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(s.now() == horizon);
        BEAST_EXPECT(stats.saturated);
        BEAST_EXPECT(stats.clampHits == 1);
        BEAST_EXPECT(stats.requestedAdvance == ms{12});
        BEAST_EXPECT(stats.consumedAdvance == ms{10});
        BEAST_EXPECT((observedMs == std::vector<int>{16, 20}));
        if (BEAST_EXPECT(s.traceLog().size() == 2))
        {
            BEAST_EXPECT(
                s.traceLog()[0].when ==
                (t0 + ms{16}).time_since_epoch().count());
            BEAST_EXPECT(
                s.traceLog()[1].when == horizon.time_since_epoch().count());
        }
    }

    void
    testOrdinarySteppingAfterProfiled()
    {
        testcase("ordinary and K=0 stepping preserve time after profiled work");
        for (bool const viaKZero : {false, true})
        {
            HarnessScheduler s;
            s.setRecordTrace(true);
            auto const t0 = s.now();
            std::vector<int> order;
            s.at(t0 + ms{10}, Tier::process, 0, [&]() { order.push_back(0); });
            s.at(t0 + ms{10}, Tier::process, 0, [&]() {
                order.push_back(1);
                BEAST_EXPECT(s.now() == t0 + ms{16});
                s.in(ms{1}, Tier::process, 0, [&]() { order.push_back(3); });
            });
            s.at(t0 + ms{12}, Tier::process, 0, [&]() {
                order.push_back(2);
                BEAST_EXPECT(s.now() == t0 + ms{16});
            });
            HarnessScheduler::ProfiledPacer pacer;
            pacer.k = 2;
            pacer.unitCost = ms{3};
            pacer.weights =
                HarnessScheduler::ProfiledPacer::KindWeights::flat();
            HarnessScheduler::ProfiledStepStats stats;
            BEAST_EXPECT(s.stepOneProfiled(t0 + ms{20}, pacer, stats));
            BEAST_EXPECT(s.now() == t0 + ms{16});

            // The fence constrains execution time, including overdue events.
            s.setFence(t0 + ms{15});
            BEAST_EXPECT(except<std::logic_error>([&]() { s.stepOne(); }));
            BEAST_EXPECT(except<std::logic_error>(
                [&]() { s.stepOneProfiled(t0 + ms{20}, pacer, stats); }));
            BEAST_EXPECT(s.size() == 2 && order.size() == 1);
            s.clearFence();
            if (viaKZero)
            {
                pacer.k = 0;
                BEAST_EXPECT(s.stepOneProfiled(t0 + ms{20}, pacer, stats));
            }
            else
                BEAST_EXPECT(s.stepOne());

            s.stepUntil(t0 + ms{17});
            BEAST_EXPECT((order == std::vector<int>{0, 1, 2, 3}));
            BEAST_EXPECT(s.now() == t0 + ms{17});
            if (BEAST_EXPECT(s.traceLog().size() == 4))
            {
                for (std::size_t i = 0; i < 3; ++i)
                    BEAST_EXPECT(
                        s.traceLog()[i].when ==
                        (t0 + ms{16}).time_since_epoch().count());
                BEAST_EXPECT(
                    s.traceLog()[3].when ==
                    (t0 + ms{17}).time_since_epoch().count());
            }
        }
    }

    void
    testProfiledPacerKindWeights()
    {
        testcase(
            "K-profiled pacer charges deterministic weights by event kind");

        HarnessScheduler s;
        auto const t0 = s.now();
        std::vector<std::string> observed;

        s.at(
            t0 + ms{10},
            Tier::heartbeat,
            0,
            [&]() {
                observed.push_back(
                    "heartbeat@" +
                    std::to_string(
                        std::chrono::duration_cast<ms>(s.now() - t0).count()));
            },
            HarnessScheduler::Kind::heartbeat);
        s.at(
            t0 + ms{10},
            Tier::deliver,
            0,
            [&]() {
                observed.push_back(
                    "deliver@" +
                    std::to_string(
                        std::chrono::duration_cast<ms>(s.now() - t0).count()));
            },
            HarnessScheduler::Kind::deliver);
        s.at(
            t0 + ms{10},
            Tier::process,
            0,
            [&]() {
                observed.push_back(
                    "job@" +
                    std::to_string(
                        std::chrono::duration_cast<ms>(s.now() - t0).count()));
            },
            HarnessScheduler::Kind::job);

        HarnessScheduler::ProfiledPacer const pacer{
            /*k=*/1,
            /*unitCost=*/ms{5},
            HarnessScheduler::ProfiledPacer::KindWeights::eventTypeV1(),
            HarnessScheduler::ProfiledPacer::NodeMultipliers{},
            HarnessScheduler::ProfiledPacer::HorizonMode::global};
        HarnessScheduler::ProfiledStepStats stats;
        auto const horizon = t0 + ms{100};

        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(!s.stepOneProfiled(horizon, pacer, stats));

        BEAST_EXPECT(
            (observed ==
             std::vector<std::string>{"deliver@20", "job@35", "heartbeat@40"}));
        BEAST_EXPECT(stats.requestedAdvance == ms{30});
        BEAST_EXPECT(stats.consumedAdvance == ms{30});
        BEAST_EXPECT(stats.weightedEvents == 6);
        BEAST_EXPECT(
            stats.eventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::heartbeat)] == 1);
        BEAST_EXPECT(
            stats.eventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::deliver)] == 1);
        BEAST_EXPECT(
            stats.eventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::job)] == 1);
        BEAST_EXPECT(
            stats.weightedEventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::heartbeat)] == 1);
        BEAST_EXPECT(
            stats.weightedEventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::deliver)] == 2);
        BEAST_EXPECT(
            stats.weightedEventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::job)] == 3);
    }

    void
    testProfiledPacerNodeMultipliers()
    {
        testcase(
            "K-profiled pacer can charge deterministic multipliers by node");

        HarnessScheduler s;
        auto const t0 = s.now();
        std::vector<std::string> observed;

        for (auto const node : {0u, 1u, 2u})
        {
            s.at(
                t0 + ms{10},
                Tier::deliver,
                node,
                [&, node]() {
                    observed.push_back(
                        "n" + std::to_string(node) + "@" +
                        std::to_string(
                            std::chrono::duration_cast<ms>(s.now() - t0)
                                .count()));
                },
                HarnessScheduler::Kind::deliver);
        }

        HarnessScheduler::ProfiledPacer const pacer{
            /*k=*/1,
            /*unitCost=*/ms{5},
            HarnessScheduler::ProfiledPacer::KindWeights::flat(),
            HarnessScheduler::ProfiledPacer::NodeMultipliers::single(
                /*nodeId=*/1, /*value=*/3),
            HarnessScheduler::ProfiledPacer::HorizonMode::global};
        HarnessScheduler::ProfiledStepStats stats;
        auto const horizon = t0 + ms{100};

        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(s.stepOneProfiled(horizon, pacer, stats));
        BEAST_EXPECT(!s.stepOneProfiled(horizon, pacer, stats));

        BEAST_EXPECT(
            (observed == std::vector<std::string>{"n0@15", "n1@30", "n2@35"}));
        BEAST_EXPECT(stats.requestedAdvance == ms{25});
        BEAST_EXPECT(stats.consumedAdvance == ms{25});
        BEAST_EXPECT(stats.weightedEvents == 5);
        BEAST_EXPECT(
            stats.eventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::deliver)] == 3);
        BEAST_EXPECT(
            stats.weightedEventsByKind[HarnessScheduler::kindIndex(
                HarnessScheduler::Kind::deliver)] == 5);
        BEAST_EXPECT(
            pacer.eventWeight(/*nodeId=*/0, HarnessScheduler::Kind::deliver) ==
            1);
        BEAST_EXPECT(
            pacer.eventWeight(/*nodeId=*/1, HarnessScheduler::Kind::deliver) ==
            3);
        BEAST_EXPECT(
            pacer.eventWeight(/*nodeId=*/2, HarnessScheduler::Kind::deliver) ==
            1);

        auto const uniform =
            HarnessScheduler::ProfiledPacer::NodeMultipliers::uniform(1);
        BEAST_EXPECT(uniform.multiplier(0) == 1);
        BEAST_EXPECT(uniform.multiplier(99) == 1);
    }

public:
    void
    run() override
    {
        testTimeOrdering();
        testSameInstantTieBreak();
        testReproducibleCascade();
        testCancel();
        testStepUntilAndWhile();
        testAtOrNowClamp();
        testAtRejectsPast();
        testProfiledPacerChargeAndClamp();
        testOrdinarySteppingAfterProfiled();
        testProfiledPacerKindWeights();
        testProfiledPacerNodeMultipliers();
    }
};

BEAST_DEFINE_TESTSUITE(HarnessScheduler, consensus, ripple);

}  // namespace ripple::test
