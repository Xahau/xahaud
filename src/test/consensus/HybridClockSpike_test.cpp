//------------------------------------------------------------------------------
// CSF + PeerImp hybrid harness — virtual-time spike.
//
// Settles, empirically, the contested claims from the v1.1 spec review about
// whether/how a virtual clock can drive real consensus. Referenced from
// .ai-docs/specs/csf-peerimp-hybrid-overlay-harness.md.j2 via projected-source
// markers (//@@start <name> ... //@@end <name>).
//
// What this proves:
//   1. csf::Scheduler's ManualClock IS the consensus clock interface (the seam
//      is real) — but it is NOT an asio-timer clock (so "template the heartbeat
//      timer on a ManualClock" is impossible as written; review kill-claim #2).
//   2. Advancing the clock does not drive consensus by itself; a *self-
//      rescheduling Scheduler event* does — deterministically, in virtual time.
//      That event is the stand-in for "post the jtNETOP_TIMER job at virtual
//      deadlines" that must replace NetworkOPs' asio heartbeatTimer_.
//
// What this does NOT prove (the remaining gating spike): driving the *real*
// RCLConsensus this way inside a real ApplicationImp without master-mutex
// deadlock (NetworkOPs.cpp:1104 holds getMasterMutex across timerEntry).
//------------------------------------------------------------------------------
#include <test/csf/BasicNetwork.h>
#include <test/csf/Scheduler.h>
#include <test/jtx/Env.h>

#include <xrpl/beast/clock/abstract_clock.h>
#include <xrpl/beast/clock/manual_clock.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpld/core/JobQueue.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <type_traits>

namespace ripple::test {

// asio::basic_waitable_timer<Clock> requires a *static* Clock::now(). This
// concept detects it: steady_clock satisfies it; an AbstractClock-derived
// ManualClock (instance/virtual now()) does not.
template <class C>
concept HasStaticNow = requires { C::now(); };

class HybridClockSpike_test : public beast::unit_test::suite
{
    using SteadyManual = beast::manual_clock<std::chrono::steady_clock>;
    // Same type as Consensus::clock_type (Consensus.h:314).
    using ConsensusClock = beast::abstract_clock<std::chrono::steady_clock>;

    void
    testClockTypeContract()
    {
        testcase("clock contract: scheduler clock IS the consensus clock, but is NOT an asio clock");

        //@@start spike-clock-seam
        // SEAM (review claim #3 — the seam is real, only the spec's "up-casts"
        // prose was loose). csf::Scheduler::clock() returns ManualClock<steady>&,
        // which IS-A AbstractClock<steady>& — exactly Consensus's clock_type.
        static_assert(std::is_same_v<csf::Scheduler::clock_type, SteadyManual>);
        static_assert(std::is_base_of_v<ConsensusClock, SteadyManual>);
        static_assert(std::is_convertible_v<SteadyManual&, ConsensusClock const&>);
        // ...but they are DIFFERENT typedefs: the scheduler exposes the concrete
        // ManualClock; consensus wants the abstract interface.
        static_assert(!std::is_same_v<csf::Scheduler::clock_type, ConsensusClock>);
        //@@end spike-clock-seam

        //@@start spike-no-static-now
        // KILL-CLAIM (review #2). asio basic_waitable_timer<Clock> needs a STATIC
        // Clock::now(). steady_clock has one; ManualClock's now() is a virtual
        // INSTANCE method (abstract_clock.h:51, manual_clock.h:36). So the very
        // per-node ManualClock that drives consensus cannot also back an asio
        // timer — "template the timer Clock to a ManualClock" is impossible.
        static_assert(HasStaticNow<std::chrono::steady_clock>);
        static_assert(!HasStaticNow<SteadyManual>);
        //@@end spike-no-static-now

        SteadyManual mc;
        BEAST_EXPECT(mc.now() == mc.now());  // now() is an instance method
    }

    void
    testVirtualTimePumpsHeartbeat()
    {
        using namespace std::chrono_literals;
        testcase("virtual time deterministically pumps a self-rescheduling heartbeat (the v2 driver)");

        csf::Scheduler scheduler;

        //@@start spike-virtual-pump
        // The v2 driver mechanism the review prescribes. Advancing the clock and
        // hoping an asio timer fires is a no-op (heartbeatTimer_ is a real
        // steady_timer, NetworkOPs.cpp:828). Instead post a self-rescheduling
        // event on the Scheduler at a fixed virtual interval — the stand-in for
        // "post the jtNETOP_TIMER job at virtual deadlines". Stepping the
        // Scheduler advances VIRTUAL time and fires the heartbeat deterministically
        // with no wall-clock sleeping (this test advances 10s instantly).
        constexpr auto heartbeat = 1s;  // stand-in for ConsensusParms::ledgerGRANULARITY
        int beats = 0;
        std::function<void()> tick = [&] {
            ++beats;
            scheduler.in(heartbeat, tick);  // self-perpetuating heartbeat
        };
        scheduler.in(heartbeat, tick);  // arm

        auto const t0 = scheduler.now();
        scheduler.step_for(10s);  // advance 10 virtual seconds
        //@@end spike-virtual-pump

        BEAST_EXPECT(beats == 10);                  // one beat per virtual second
        BEAST_EXPECT(scheduler.now() == t0 + 10s);  // clock advanced exactly

        //@@start spike-one-clock
        // The SAME scheduler clock that pumps the heartbeat is the one a real
        // Consensus would sample via now() — one ManualClock, two roles (driver
        // timeline + consensus's now()).
        ConsensusClock const& consensusClock = scheduler.clock();
        BEAST_EXPECT(consensusClock.now() == scheduler.now());
        //@@end spike-one-clock
    }

    void
    testTransportSubstrate()
    {
        using namespace std::chrono_literals;
        testcase("BasicNetwork is a generic, partitionable transport (substrate reuse)");

        csf::Scheduler scheduler;

        //@@start spike-transport-substrate
        // BasicNetwork<Peer> is already a template over the vertex type; csf only
        // instantiates BasicNetwork<csf::Peer*> by convention. Here we drive it
        // with a NON-csf vertex (int, standing in for a RealNode id) carrying an
        // arbitrary payload — proving it can host real-Application nodes unchanged.
        // connect()/disconnect() are the partition/eclipse primitives.
        csf::BasicNetwork<int> net{scheduler};
        net.connect(1, 2, 50ms);

        int delivered = 0;
        net.send(1, 2, [&] { ++delivered; });  // payload would carry Message bytes
        BEAST_EXPECT(delivered == 0);          // in flight
        scheduler.step_for(100ms);
        BEAST_EXPECT(delivered == 1);          // delivered after the link delay

        // PARTITION: a message in flight when the link drops is discarded by
        // BasicNetwork's established<=sent guard — heal/partition for free.
        net.send(1, 2, [&] { ++delivered; });
        net.disconnect(1, 2);
        scheduler.step_for(100ms);
        BEAST_EXPECT(delivered == 1);          // dropped by the partition
        //@@end spike-transport-substrate
    }

    void
    testQuiescenceBarrierFixpoint()
    {
        using namespace std::chrono_literals;
        testcase("quiescence barrier needs a bounded predicate (self-rescheduling work has no empty-queue fixpoint)");

        csf::Scheduler scheduler;

        //@@start spike-quiescence-fixpoint
        // The review's determinism caveat, demonstrated. Self-rescheduling events
        // (like the acquire retry timers, TimeoutCounter) keep the event queue
        // NON-empty forever, so a "drain until the queue is empty" barrier would
        // never return. A correct quiescence barrier must use a BOUNDED predicate
        // (a virtual deadline / progress condition) — which terminates cleanly.
        int work = 0;
        std::function<void()> reschedule = [&] {
            ++work;
            scheduler.in(1s, reschedule);  // never stops on its own
        };
        scheduler.in(1s, reschedule);

        auto const t0 = scheduler.now();
        // A BOUNDED predicate (virtual deadline) terminates cleanly:
        scheduler.step_while([&] { return scheduler.now() < t0 + 5s; });
        BEAST_EXPECT(work == 5);                   // exactly 5 beats before the deadline
        BEAST_EXPECT(scheduler.now() == t0 + 5s);  // bounded predicate terminated

        // The queue is still non-empty (the t0+6 reschedule is pending), so a
        // drain-to-empty barrier (scheduler.step()) would loop here FOREVER —
        // the exact bug a naive quiescence barrier hits. stepOne() confirms
        // there is still pending work, safely.
        BEAST_EXPECT(scheduler.step_one());
        //@@end spike-quiescence-fixpoint
    }

    void
    testRealAppJobPostAndDrain()
    {
        testcase("real Application: post a heartbeat-style job and drain to quiescence (Stage-2 driver primitive)");
        jtx::Env env{*this};

        //@@start spike-real-app-jobpost
        // The Stage-2 driver mechanism against a REAL ApplicationImp (not a csf
        // mock). In production the asio heartbeat handler does
        //   jobQueue_.addJob(jtNETOP_TIMER, "NetHeart", []{ processHeartbeatTimer(); })
        // (NetworkOPs.cpp:1067). Crucially, processHeartbeatTimer RELEASES the
        // master mutex (its lock block ends at NetworkOPs.cpp:1164) BEFORE calling
        // consensus_.timerEntry() at NetworkOPs.cpp:1166 — so timerEntry does NOT
        // run under the master mutex. That corrects the review's "holds master
        // mutex across timerEntry" deadlock fear: virtualizing the heartbeat only
        // changes WHO/WHEN posts the job, while the JobQueue worker runs it with
        // prod's exact (already deadlock-safe) locking. Here we post a job to the
        // REAL JobQueue and drain it to quiescence via rendezvous() — proving the
        // post-and-drain driver primitive works against a real Application.
        std::atomic<int> ran{0};
        auto& jq = env.app().getJobQueue();
        BEAST_EXPECT(jq.addJob(jtCLIENT, "spike", [&] { ran.fetch_add(1); }));
        jq.rendezvous();  // bounded drain-to-quiescence (cf. spike-quiescence-fixpoint)
        BEAST_EXPECT(ran.load() == 1);
        //@@end spike-real-app-jobpost
    }

public:
    void
    run() override
    {
        testClockTypeContract();
        testVirtualTimePumpsHeartbeat();
        testTransportSubstrate();
        testQuiescenceBarrierFixpoint();
        testRealAppJobPostAndDrain();
    }
};

BEAST_DEFINE_TESTSUITE(HybridClockSpike, consensus, ripple);

}  // namespace ripple::test
