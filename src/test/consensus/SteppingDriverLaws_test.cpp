//------------------------------------------------------------------------------
// SteppingDriverLaws unit suite (Stage 3, S3 driver-layer hardening §7.9e) —
// the DRIVER-LAWS layer of the stepping harness. Application-FREE by
// construction: no rippled Application, no JobQueue dispatch hook — just a
// SteppingController and its HarnessScheduler, driven exactly like
// SteppingController_test.cpp (plain callables, no app state). What it pins is
// the "beat engine" (plan §7.9a: the ONE primitive every scenario driver builds
// on) and the time-OWNERSHIP discipline it enforces (design-notes §2, the
// injection/cadence collision case study): a beat may only ever execute the
// slice of the timeline it owns — never drain someone else's future.
//
// One law per testcase:
//   1. Horizon      — a beat settles its heartbeat AND everything that cascades
//                     from it (a delivery the heartbeat schedules), but never
//                     executes past its when+dt horizon; far-future injections
//                     stay queued and now() never crosses the horizon.
//   2. Fence        — on a bare scheduler, EXECUTING beyond a planted fence
//                     THROWS and names the offending event's kind; SCHEDULING
//                     beyond it is legal; clearing the fence lets it run.
//   3. Composition  — explicitly anchored beats compose: an unrolled pair and
//                     a loop using the same grid produce identical traces.
//   4. Driven set   — a beat fires exactly its nodeIds, in order, each at the
//                     phase its skew dictates — and no other node.
//   5. Injection    — a far-future injection is owned by the beat whose horizon
//                     first covers it, and survives untouched until then.
//   6. Quiet gap    — stepUntilTime settles everything <= until (now() lands on
//                     until) and leaves later events queued.
//   7. Leftover     — an early-stopped beat leaves un-run heartbeats queued;
//                     beat() must capture `fire` BY VALUE so a later beat can
//                     still run them after the caller's fire TEMPORARY is gone
//                     (a by-reference capture would be use-after-destruction).
//------------------------------------------------------------------------------
#include <test/jtx/HarnessScheduler.h>
#include <test/jtx/SteppingController.h>

#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ripple {
namespace test {

class SteppingDriverLaws_test : public beast::unit_test::suite
{
    using Tier = SteppingController::Tier;
    using Kind = HarnessScheduler::Kind;
    using time_point = SteppingController::time_point;
    using ms = std::chrono::milliseconds;
    using s = std::chrono::seconds;

    // A no-op clock sync — the controller HARD-fails if none is installed
    // before any event runs (SteppingController_test pins that), so every
    // app-free test installs this before driving the scheduler.
    static std::function<void(time_point)>
    noopClock()
    {
        return [](time_point) {};
    }

    void
    testHorizonLaw()
    {
        testcase("horizon: a beat settles its cascade, never past when+dt");
        SteppingController c;
        c.setSyncClock(noopClock());
        auto const t0 = c.now();
        auto const horizon = t0 + s{2};  // beat is when=t0+1s, dt=1s

        // A harness injection far beyond the beat's horizon. The beat must NOT
        // reach it; it stays queued for the beat that eventually owns it.
        bool injectionRan = false;
        c.scheduleAt(
            t0 + s{10},
            Tier::process,
            0,
            [&]() { injectionRan = true; },
            Kind::inject);

        SteppingController::BeatSpec spec;
        spec.nodeIds = {0};
        spec.dt = s{1};

        bool heartbeatRan = false;
        bool deliveryRan = false;
        // The fired heartbeat schedules a +5ms delivery; the beat must settle
        // that cascade too (it is within the horizon), all in one beat.
        c.beat(
            t0 + s{1},
            spec,
            [&](std::uint32_t id) {
                heartbeatRan = true;
                c.scheduleDelivery(id, ms{5}, [&]() { deliveryRan = true; });
            },
            []() { return false; },
            100000);

        BEAST_EXPECT(heartbeatRan);
        BEAST_EXPECT(deliveryRan);    // the cascade settled inside the beat
        BEAST_EXPECT(!injectionRan);  // the +10s injection did NOT run
        BEAST_EXPECT(c.scheduler().size() == 1);  // only the injection remains
        // now() reached the last in-horizon event (the +5ms delivery) and never
        // crossed the horizon.
        BEAST_EXPECT(c.now() == t0 + s{1} + ms{5});
        BEAST_EXPECT(c.now() < horizon);
    }

    void
    testFenceLaw()
    {
        testcase(
            "fence: executing beyond it throws (named); scheduling is legal");
        HarnessScheduler sched;
        auto const t0 = sched.now();
        sched.setFence(t0 + s{1});

        // SCHEDULING beyond the fence is legal — future events wait, they are
        // not rejected. Only EXECUTING beyond the fence is the violation. (A
        // throw here would escape and fail the whole testcase, so reaching the
        // next line IS the no-throw assertion.)
        bool ran = false;
        sched.at(
            t0 + s{2},
            Tier::process,
            /*nodeId*/ 7,
            [&]() { ran = true; },
            Kind::inject);
        BEAST_EXPECT(sched.size() == 1);

        // Stepping it while the fence stands must throw, and the diagnostic
        // must name the offending event's KIND ("inject") so a driver bug is
        // legible.
        bool threw = false;
        std::string what;
        try
        {
            sched.stepOne();
        }
        catch (std::logic_error const& e)
        {
            threw = true;
            what = e.what();
        }
        BEAST_EXPECT(threw);
        BEAST_EXPECT(what.find("inject") != std::string::npos);
        BEAST_EXPECT(!ran);  // the throw ran nothing; the event is still queued

        // Clear the fence and the very same event runs.
        sched.clearFence();
        BEAST_EXPECT(sched.stepOne());
        BEAST_EXPECT(ran);
        BEAST_EXPECT(sched.empty());
    }

    // The fixed compose workload: two beats over nodes {0,1}, each fire records
    // a trace entry and schedules a +5ms delivery that records another. Run
    // once as two explicit beats (asLoop=false), once as a driver-style loop
    // (asLoop=true) — both explicitly preserve t0. Public tick() re-anchors
    // each call and therefore is not the operation whose composition this test
    // proves.
    std::vector<std::string>
    runComposeWorkload(SteppingController& c, bool asLoop)
    {
        std::vector<std::string> trace;
        auto const t0 = c.now();

        SteppingController::BeatSpec spec;
        spec.nodeIds = {0, 1};
        spec.dt = s{1};

        auto fire = [&](std::uint32_t id) {
            trace.push_back("fire n" + std::to_string(id));
            c.scheduleDelivery(id, ms{5}, [&trace, id]() {
                trace.push_back("recv n" + std::to_string(id));
            });
        };
        auto stop = []() { return false; };

        if (asLoop)
        {
            for (int k = 1; k <= 2; ++k)
                c.beat(t0 + s{k}, spec, fire, stop, 100000);
        }
        else
        {
            c.beat(t0 + s{1}, spec, fire, stop, 100000);
            c.beat(t0 + s{2}, spec, fire, stop, 100000);
        }
        return trace;
    }

    void
    testCompositionLaw()
    {
        testcase("composition: unrolled beats == a driver's beat loop");
        SteppingController unrolled;
        unrolled.setSyncClock(noopClock());
        SteppingController looped;
        looped.setSyncClock(noopClock());

        auto const traceUnrolled =
            runComposeWorkload(unrolled, /*asLoop*/ false);
        auto const traceLooped = runComposeWorkload(looped, /*asLoop*/ true);

        BEAST_EXPECT(!traceUnrolled.empty());     // the workload did work
        BEAST_EXPECT(traceUnrolled.size() == 8);  // 2 beats * (2 fire + 2 recv)
        BEAST_EXPECT(traceUnrolled == traceLooped);  // byte-identical
    }

    void
    testDrivenSetLaw()
    {
        testcase(
            "driven set: fires exactly its nodeIds, in order, at its skew");
        SteppingController c;
        c.setSyncClock(noopClock());
        auto const when = c.now() + s{1};

        SteppingController::BeatSpec spec;
        spec.nodeIds = {1, 3};
        spec.skew = ms{100};  // p*skew < dt, so both land within the horizon
        spec.dt = s{1};

        std::vector<std::uint32_t> fired;
        std::vector<time_point> firedAt;
        c.beat(
            when,
            spec,
            [&](std::uint32_t id) {
                fired.push_back(id);
                firedAt.push_back(c.now());  // observe now() inside the fire
            },
            []() { return false; },
            100000);

        BEAST_EXPECT(fired.size() == 2);  // exactly the driven set, nobody else
        BEAST_EXPECT(fired[0] == 1 && fired[1] == 3);  // in order
        BEAST_EXPECT(firedAt[0] == when);              // position 0: no skew
        BEAST_EXPECT(firedAt[1] == when + ms{100});    // position 1: +skew
    }

    void
    testInjectionOwnershipLaw()
    {
        testcase("injection: owned by the beat whose horizon first covers it");
        SteppingController c;
        c.setSyncClock(noopClock());
        auto const t0 = c.now();

        bool nearRan = false;  // 1500ms: covered by beat #1's horizon (t0+2s)
        bool farRan = false;   // +10s: covered by neither beat
        c.scheduleAt(
            t0 + ms{1500},
            Tier::process,
            0,
            [&]() { nearRan = true; },
            Kind::inject);
        c.scheduleAt(
            t0 + s{10},
            Tier::process,
            0,
            [&]() { farRan = true; },
            Kind::inject);

        // Pure horizon-bounded settles (no heartbeats of their own): each beat
        // owns [when, when+dt] and touches nothing beyond it.
        SteppingController::BeatSpec spec;
        spec.fireHeartbeats = false;
        spec.dt = s{1};
        auto stop = []() { return false; };

        c.beat(t0 + s{1}, spec, {}, stop, 100000);  // horizon t0+2s
        BEAST_EXPECT(nearRan);                      // 1500ms <= t0+2s: runs
        BEAST_EXPECT(!farRan);                      // +10s stays queued
        BEAST_EXPECT(c.scheduler().size() == 1);

        c.beat(t0 + s{2}, spec, {}, stop, 100000);  // horizon t0+3s
        BEAST_EXPECT(!farRan);                      // still beyond, untouched
        BEAST_EXPECT(c.scheduler().size() == 1);  // the +10s injection survives
    }

    void
    testQuietGapLaw()
    {
        testcase(
            "quiet gap: stepUntilTime settles <= until, leaves later queued");
        SteppingController c;
        c.setSyncClock(noopClock());
        auto const t0 = c.now();
        auto const until = t0 + s{2};

        bool boundaryRan = false;
        bool farRan = false;
        c.scheduleAt(
            until, Tier::timer, 0, [&]() { boundaryRan = true; }, Kind::timer);
        c.scheduleAt(
            t0 + s{10}, Tier::timer, 0, [&]() { farRan = true; }, Kind::timer);

        c.stepUntilTime(until, 100000);

        BEAST_EXPECT(boundaryRan);       // the event at exactly `until` ran
        BEAST_EXPECT(!farRan);           // the +10s event did not
        BEAST_EXPECT(c.now() == until);  // time landed on the gap boundary
        BEAST_EXPECT(
            c.scheduler().size() == 1);  // the far event is still queued
    }

    void
    testQuietGapWithoutNodeZero()
    {
        testcase("global quiet gap advances clocks with node zero inactive");
        SteppingController c;
        std::vector<time_point> synced;
        c.setSyncClock([&](time_point t) { synced.push_back(t); });
        c.deactivateNode(0);
        auto const t0 = c.now();
        bool due = false;
        bool future = false;
        c.scheduleAt(t0 + s{10}, Tier::process, 1, [&]() { due = true; });
        c.scheduleAt(t0 + s{80}, Tier::process, 1, [&]() { future = true; });
        c.advanceTimeTo(t0 + s{60}, 100);
        BEAST_EXPECT(due && !future);
        BEAST_EXPECT(c.now() == t0 + s{60});
        BEAST_EXPECT(
            (synced == std::vector<time_point>{t0 + s{10}, t0 + s{60}}));
        BEAST_EXPECT(c.scheduler().size() == 1);

        BEAST_EXPECT(except<std::logic_error>(
            [&]() { c.advanceTimeTo(t0 + s{59}, 100); }));
        BEAST_EXPECT(c.now() == t0 + s{60});
        BEAST_EXPECT(except<std::logic_error>(
            [&]() { c.advanceTimeTo(t0 + s{90}, 0); }));
        BEAST_EXPECT(!future && c.now() == t0 + s{60});
        BEAST_EXPECT(c.scheduler().size() == 1);
        c.advanceTimeTo(t0 + s{90}, 100);
        BEAST_EXPECT(future && c.now() == t0 + s{90});
        c.deactivateNode(1);
        c.advanceTimeTo(t0 + s{100}, 100);
        BEAST_EXPECT(c.now() == t0 + s{100});
        BEAST_EXPECT(synced.back() == t0 + s{100});

        SteppingController missingClock;
        BEAST_EXPECT(except<std::logic_error>([&]() {
            missingClock.advanceTimeTo(missingClock.now() + s{1}, 100);
        }));
        BEAST_EXPECT(missingClock.now() == time_point{});
    }

    void
    testLeftoverHeartbeatLifetimeLaw()
    {
        testcase("leftover heartbeat: beat captures fire BY VALUE (lifetime)");
        SteppingController c;
        c.setSyncClock(noopClock());
        auto const t0 = c.now();

        // Records into a vector in THIS scope; the fire lambda captures it by
        // reference, and the lambda itself is a TEMPORARY built inline in the
        // beat() call below — it is destroyed the instant beat() returns.
        std::vector<std::uint32_t> fired;

        SteppingController::BeatSpec spec;
        spec.nodeIds = {0, 1};  // skew 0: both heartbeats land at t0+1s
        spec.dt = s{1};

        // stop() is checked BEFORE each step: false first (node 0 fires), then
        // true — so node 1's heartbeat is left un-executed in the queue.
        int stopChecks = 0;
        c.beat(
            t0 + s{1},
            spec,
            [&fired](std::uint32_t id) { fired.push_back(id); },  // TEMPORARY
            [&stopChecks]() { return stopChecks++ >= 1; },
            100000);

        BEAST_EXPECT(fired.size() == 1);  // only node 0 ran
        BEAST_EXPECT(fired[0] == 0);
        BEAST_EXPECT(c.scheduler().size() == 1);  // node 1's heartbeat lingers

        // A second beat (no new heartbeats) runs the leftover. Because beat()
        // captured `fire` BY VALUE, the leftover event holds a live copy of the
        // (now-destroyed) fire temporary and records correctly. A by-reference
        // capture would dangle here — this run pins that it does not.
        SteppingController::BeatSpec settle;
        settle.fireHeartbeats = false;
        settle.dt = s{1};
        c.beat(c.now(), settle, {}, []() { return false; }, 100000);

        BEAST_EXPECT(fired.size() == 2);  // the leftover heartbeat ran
        BEAST_EXPECT(fired[1] == 1);      // it was node 1, recorded correctly
        BEAST_EXPECT(c.empty());
    }

public:
    void
    run() override
    {
        testHorizonLaw();
        testFenceLaw();
        testCompositionLaw();
        testDrivenSetLaw();
        testInjectionOwnershipLaw();
        testQuietGapLaw();
        testQuietGapWithoutNodeZero();
        testLeftoverHeartbeatLifetimeLaw();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingDriverLaws, consensus, ripple);

}  // namespace test
}  // namespace ripple
