//------------------------------------------------------------------------------
// SteppingController unit suite (Stage 3, S3.3) — exercises the controller in
// ISOLATION (no JobQueue/app: the dispatch hook is a plain callable we invoke
// directly), pinning the deterministic-dispatch contract the 2-node experiment
// relies on:
//   - JobType → tier mapping (consensus-critical vs fall-through);
//   - the hook ENQUEUES claimed jobs (returns true) and leaves the rest
//   (false);
//   - clock coherence is mandatory (missing setSyncClock → hard failure) and is
//     applied at the event's virtual time;
//   - cross-node delivery delay must be strictly positive;
//   - off-stepping-thread consensus jobs fail hard (and are counted);
//   - dropPending() releases pending events (teardown safety vs
//   JobCounter::join).
//   - canonicalAllJobs runs non-denied real closures without weakening strict
//     known-only discovery mode.
// Spec: csf-peerimp-hybrid-overlay-harness.md (Stage 3).
//------------------------------------------------------------------------------
#include <test/jtx/SteppingController.h>

#include <xrpld/core/Job.h>
#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ripple {
namespace test {

class SteppingController_test : public beast::unit_test::suite
{
    using Tier = SteppingController::Tier;
    using ms = std::chrono::milliseconds;

    void
    testClassify()
    {
        testcase("classify(type,name): name-aware enqueue / drop / fail");
        using A = SteppingController::Action;
        auto cls = [](JobType t, std::string const& n) {
            return SteppingController::classify(t, n);
        };

        // Modeled consensus jobs → enqueue in the right tier.
        BEAST_EXPECT(
            cls(jtNETOP_TIMER, "NetHeart").action == A::enqueue &&
            cls(jtNETOP_TIMER, "NetHeart").tier == Tier::heartbeat);
        BEAST_EXPECT(
            cls(jtPROPOSAL_t, "checkPropose").action == A::enqueue &&
            cls(jtPROPOSAL_t, "checkPropose").tier == Tier::process);
        BEAST_EXPECT(
            cls(jtVALIDATION_t, "ChkTrust").action == A::enqueue &&
            cls(jtVALIDATION_t, "ChkTrust").tier == Tier::process);
        BEAST_EXPECT(
            cls(jtACCEPT, "AcceptLedger").action == A::enqueue &&
            cls(jtACCEPT, "AcceptLedger").tier == Tier::accept);

        // jtADVANCE is NAME-AWARE: "advanceLedger" is the validated-ledger
        // advance; GetConsL1/L2 + TryFill are the acquire/backfill paths,
        // modeled since 5.6b (the late-joiner increment). Unknown names fail.
        BEAST_EXPECT(
            cls(jtADVANCE, "advanceLedger").action == A::enqueue &&
            cls(jtADVANCE, "advanceLedger").tier == Tier::advance);
        BEAST_EXPECT(
            cls(jtADVANCE, "getConsensusLedger1").action == A::enqueue &&
            cls(jtADVANCE, "getConsensusLedger1").tier == Tier::process);
        BEAST_EXPECT(
            cls(jtADVANCE, "getConsensusLedger2").action == A::enqueue &&
            cls(jtADVANCE, "getConsensusLedger2").tier == Tier::process);
        BEAST_EXPECT(cls(jtADVANCE, "SomethingElse").action == A::fail);
        BEAST_EXPECT(
            cls(jtWAL, "WAL").action == A::enqueue &&
            cls(jtWAL, "WAL").tier == Tier::process);
        BEAST_EXPECT(cls(jtWAL, "UnknownCheckpoint").action == A::fail);

        // The acquire data pipeline: peer-serving reads and received-data
        // processing run in arrival order; the TimeoutCounter retry is a timer.
        BEAST_EXPECT(
            cls(jtLEDGER_REQ, "RcvGetLedger").action == A::enqueue &&
            cls(jtLEDGER_REQ, "RcvGetLedger").tier == Tier::process);
        BEAST_EXPECT(
            cls(jtLEDGER_DATA, "ProcessLData").action == A::enqueue &&
            cls(jtLEDGER_DATA, "ProcessLData").tier == Tier::process);
        BEAST_EXPECT(
            cls(jtLEDGER_DATA, "InboundLedger").action == A::enqueue &&
            cls(jtLEDGER_DATA, "InboundLedger").tier == Tier::timer);

        BEAST_EXPECT(
            cls(jtMANIFEST, "receiveManifests").action == A::enqueue &&
            cls(jtMANIFEST, "receiveManifests").tier == Tier::process);
        BEAST_EXPECT(
            cls(jtMANIFEST, "unexpectedManifestJob").action == A::fail);

        // Harness no-ops → drop; unmodeled → fail.
        BEAST_EXPECT(cls(jtCLIENT_CONSENSUS, "PubCons").action == A::drop);
        BEAST_EXPECT(cls(jtUPDATE_PF, "OB3").action == A::drop);
        BEAST_EXPECT(cls(jtSWEEP, "Sweep").action == A::fail);
    }

    void
    testWalCheckpointScheduled()
    {
        testcase("SQLite WAL checkpoint work remains deferred");
        using D = JobQueue::JobDisposition;
        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        auto hook = c.makeJobHook(0);

        bool checkpointRan = false;
        BEAST_EXPECT(hook(jtWAL, "WAL", [&]() {
                         checkpointRan = true;
                     }) == D::claimedQueued);
        BEAST_EXPECT(!checkpointRan);
        BEAST_EXPECT(c.stepOne());
        BEAST_EXPECT(checkpointRan);
        BEAST_EXPECT(c.empty());
    }

    void
    testJobHookClosedWorld()
    {
        testcase(
            "closed-world hook: enqueue modeled, drop no-ops, FAIL unmodeled");
        using D = JobQueue::JobDisposition;
        SteppingController c;
        c.setSyncClock(
            [](SteppingController::time_point) {});  // no-op (required)
        auto hook = c.makeJobHook(/*nodeId*/ 0);

        bool ran = false;
        // A modeled consensus job is claimedQueued and ENQUEUED, not run yet.
        BEAST_EXPECT(hook(jtPROPOSAL_t, "checkPropose", [&]() {
                         ran = true;
                     }) == D::claimedQueued);
        BEAST_EXPECT(c.scheduler().size() == 1);
        BEAST_EXPECT(!ran);  // enqueue-only: nothing runs at the post site

        // A harness no-op is claimedDropped (reported not-queued;
        // counter-safe).
        BEAST_EXPECT(
            hook(jtCLIENT_CONSENSUS, "PubCons", [&]() {}) == D::claimedDropped);
        BEAST_EXPECT(c.scheduler().size() == 1);  // still just the proposal

        // The enqueued modeled job runs only when the scheduler is stepped.
        BEAST_EXPECT(c.stepOne());
        BEAST_EXPECT(ran);
        BEAST_EXPECT(c.empty());

        {
            SteppingController cFail;
            cFail.setSyncClock([](SteppingController::time_point) {});
            auto failHook = cFail.makeJobHook(0);
            // Unmodeled type FAILS HARD; and jtADVANCE under a name outside
            // the modeled set (AdvanceLedger/GetConsL1/GetConsL2/TryFill)
            // must also FAIL — an unknown path can't slip in via the type.
            BEAST_EXPECT(except<std::logic_error>(
                [&]() { failHook(jtSWEEP, "Sweep", [&]() {}); }));
            BEAST_EXPECT(except<std::logic_error>(
                [&]() { failHook(jtADVANCE, "UnknownAdvance", [&]() {}); }));
            BEAST_EXPECT(cFail.failedJobs() == 2);
            BEAST_EXPECT(
                cFail.jobDiagnostics().find("UnknownAdvance") !=
                std::string::npos);
        }
    }

    void
    testNamedJobLagAndObservation()
    {
        testcase(
            "named lag delays only the selected job; observations precede real "
            "bodies");
        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        c.setJobLag(0, Tier::process, ms{2});
        c.setJobLag(0, jtADVANCE, "tryFill", ms{7});
        auto hook0 = c.makeJobHook(0);
        auto hook1 = c.makeJobHook(1);
        std::vector<int> order;
        std::vector<SteppingController::time_point> times;
        bool observed = false;
        c.observeJobs([&](std::uint32_t, JobType, std::string const&) {
            BEAST_EXPECT(!observed);
            observed = true;
        });
        auto body = [&](int id) {
            BEAST_EXPECT(observed);
            observed = false;
            order.push_back(id);
            times.push_back(c.now());
        };
        using D = JobQueue::JobDisposition;
        BEAST_EXPECT(
            hook0(jtADVANCE, "tryFill", [&] { body(3); }) == D::claimedQueued);
        BEAST_EXPECT(hook0(jtADVANCE, "getConsensusLedger2", [&] {
                         body(1);
                     }) == D::claimedQueued);
        BEAST_EXPECT(hook0(jtPROPOSAL_t, "checkPropose", [&] {
                         body(2);
                     }) == D::claimedQueued);
        BEAST_EXPECT(
            hook1(jtADVANCE, "tryFill", [&] { body(0); }) == D::claimedQueued);
        while (c.stepOne())
        {
        }
        BEAST_EXPECT((order == std::vector<int>{0, 1, 2, 3}));
        BEAST_EXPECT(
            (times ==
             std::vector<SteppingController::time_point>{
                 {},
                 SteppingController::time_point{ms{2}},
                 SteppingController::time_point{ms{2}},
                 SteppingController::time_point{ms{7}}}));
        c.clearJobLag(0);
        auto const now = c.now();
        BEAST_EXPECT(
            hook0(jtADVANCE, "tryFill", [&] { body(4); }) == D::claimedQueued);
        BEAST_EXPECT(c.stepOne());
        BEAST_EXPECT(times.back() == now);
        BEAST_EXPECT(except<std::logic_error>(
            [&] { c.setJobLag(0, jtADVANCE, "W", ms{-1}); }));
    }

    void
    testLaggedPendingJobCount()
    {
        testcase("laggedPendingJobCount is current, per node and name");
        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        c.setJobLag(0, jtADVANCE, "tryFill", ms{7});
        c.setJobLag(1, jtADVANCE, "tryFill", ms{7});
        c.setJobLag(0, jtADVANCE, "getConsensusLedger2", ms{7});
        auto hook0 = c.makeJobHook(0);
        auto hook1 = c.makeJobHook(1);
        using D = JobQueue::JobDisposition;
        BEAST_EXPECT(hook0(jtADVANCE, "tryFill", [] {}) == D::claimedQueued);
        BEAST_EXPECT(hook0(jtADVANCE, "tryFill", [] {}) == D::claimedQueued);
        BEAST_EXPECT(
            hook0(jtADVANCE, "getConsensusLedger2", [] {}) == D::claimedQueued);
        BEAST_EXPECT(hook1(jtADVANCE, "tryFill", [] {}) == D::claimedQueued);
        BEAST_EXPECT(c.laggedPendingJobCount(0, jtADVANCE, "tryFill") == 2);
        BEAST_EXPECT(
            c.laggedPendingJobCount(0, jtADVANCE, "getConsensusLedger2") == 1);
        BEAST_EXPECT(c.laggedPendingJobCount(1, jtADVANCE, "tryFill") == 1);
        BEAST_EXPECT(
            c.laggedPendingJobCount(1, jtADVANCE, "getConsensusLedger2") == 0);
        while (c.stepOne())
        {
        }
        BEAST_EXPECT(c.laggedPendingJobCount(0, jtADVANCE, "tryFill") == 0);
        BEAST_EXPECT(c.laggedPendingJobCount(1, jtADVANCE, "tryFill") == 0);

        BEAST_EXPECT(hook0(jtADVANCE, "tryFill", [] {}) == D::claimedQueued);
        BEAST_EXPECT(hook1(jtADVANCE, "tryFill", [] {}) == D::claimedQueued);
        BEAST_EXPECT(c.laggedPendingJobCount(0, jtLEDGER_DATA, "tryFill") == 0);
        BEAST_EXPECT(c.dropPendingForNode(0) == 1);
        BEAST_EXPECT(c.laggedPendingJobCount(0, jtADVANCE, "tryFill") == 0);
        BEAST_EXPECT(c.laggedPendingJobCount(1, jtADVANCE, "tryFill") == 1);
        c.dropPending();
        BEAST_EXPECT(c.laggedPendingJobCount(1, jtADVANCE, "tryFill") == 0);
        BEAST_EXPECT(c.scheduler().empty());
    }

    void
    testCanonicalAllJobsPolicy()
    {
        testcase("canonicalAllJobs: enqueue real non-denied closures");
        using D = JobQueue::JobDisposition;

        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        c.setJobPolicy(SteppingController::JobPolicy::canonicalAllJobs);
        auto hook = c.makeJobHook(/*nodeId*/ 0);

        bool acquireRan = false;
        BEAST_EXPECT(hook(jtADVANCE, "getConsensusLedger2", [&]() {
                         acquireRan = true;
                     }) == D::claimedQueued);
        BEAST_EXPECT(c.failedJobs() == 0);
        BEAST_EXPECT(c.scheduler().size() == 1);
        BEAST_EXPECT(c.stepOne());
        BEAST_EXPECT(acquireRan);

        // True harness no-ops are still dropped, not promoted by canonical
        // mode.
        BEAST_EXPECT(
            hook(jtCLIENT_CONSENSUS, "PubCons", []() {}) == D::claimedDropped);

        // Explicitly denied background/client lanes still fail loud.
        BEAST_EXPECT(except<std::logic_error>(
            [&]() { hook(jtSWEEP, "sweep", []() {}); }));
        BEAST_EXPECT(c.failedJobs() == 1);
        BEAST_EXPECT(c.jobDiagnostics().find("sweep") != std::string::npos);
    }

    void
    testCanonicalNestedJobNotSwallowed()
    {
        testcase("canonicalAllJobs: formerly-unmodeled nested jobs run later");
        using D = JobQueue::JobDisposition;

        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        c.setJobPolicy(SteppingController::JobPolicy::canonicalAllJobs);
        auto hook = c.makeJobHook(0);

        bool outerRan = false;
        bool acquireRan = false;
        BEAST_EXPECT(hook(jtVALIDATION_t, "ChkTrust", [&]() {
                         outerRan = true;
                         BEAST_EXPECT(
                             hook(jtADVANCE, "getConsensusLedger2", [&]() {
                                 acquireRan = true;
                             }) == D::claimedQueued);
                     }) == D::claimedQueued);

        BEAST_EXPECT(c.stepOne());
        BEAST_EXPECT(outerRan);
        BEAST_EXPECT(!acquireRan);
        BEAST_EXPECT(c.failedJobs() == 0);

        BEAST_EXPECT(c.stepOne());
        BEAST_EXPECT(acquireRan);
        BEAST_EXPECT(c.failedJobs() == 0);
    }

    void
    testCaughtUnmodeledJobFailsStep()
    {
        testcase("unmodeled jobs fail the step even if app code catches them");
        using D = JobQueue::JobDisposition;
        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        auto hook = c.makeJobHook(0);

        BEAST_EXPECT(hook(jtVALIDATION_t, "ChkTrust", [&]() {
                         try
                         {
                             (void)hook(jtADVANCE, "UnknownAdvance", []() {});
                         }
                         catch (std::logic_error const&)
                         {
                             // Production code may catch the original hook
                             // failure. The controller's post-event audit must
                             // still fail.
                         }
                     }) == D::claimedQueued);

        BEAST_EXPECT(except<std::logic_error>([&]() { c.stepOne(); }));
        BEAST_EXPECT(c.failedJobs() == 1);
        BEAST_EXPECT(
            c.jobDiagnostics().find("UnknownAdvance") != std::string::npos);
    }

    void
    testClockSyncRequiredAndApplied()
    {
        testcase(
            "clock sync is mandatory and applied at the event's virtual time");
        {
            // No setSyncClock installed → running an event fails hard.
            SteppingController c;
            auto hook = c.makeJobHook(0);
            BEAST_EXPECT(hook(jtNETOP_TIMER, "hb", []() {
                         }) == JobQueue::JobDisposition::claimedQueued);
            BEAST_EXPECT(except<std::logic_error>([&]() { c.stepOne(); }));
        }
        {
            SteppingController c;
            SteppingController::time_point seen{};
            bool synced = false;
            c.setSyncClock([&](SteppingController::time_point t) {
                seen = t;
                synced = true;
            });
            // A delivery 10ms out; when it runs, the clock is synced to t0+10ms
            // BEFORE the handler body.
            auto const t0 = c.now();
            bool order_ok = false;
            c.scheduleDelivery(0, ms{10}, [&]() { order_ok = synced; });
            BEAST_EXPECT(c.stepOne());
            BEAST_EXPECT(synced);
            BEAST_EXPECT(seen == t0 + ms{10});
            BEAST_EXPECT(c.now() == t0 + ms{10});
            BEAST_EXPECT(order_ok);  // sync happened before the handler body
        }
    }

    void
    testDeliveryPositiveDelay()
    {
        testcase("scheduleDelivery requires strictly-positive delay");
        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        c.scheduleDelivery(0, ms{5}, []() {});  // ok
        BEAST_EXPECT(c.scheduler().size() == 1);
        // Zero / negative delay is a bug (would be same-instant as its cause).
        BEAST_EXPECT(except<std::logic_error>(
            [&]() { c.scheduleDelivery(0, ms{0}, []() {}); }));
        BEAST_EXPECT(except<std::logic_error>(
            [&]() { c.scheduleDelivery(0, ms{-1}, []() {}); }));
    }

    void
    testProfiledPerNodeHorizonDoesNotGloballyStall()
    {
        testcase(
            "profiled per-node horizon: slow node lags without stalling peers");

        using Pacer = SteppingController::ProfiledPacer;

        SteppingController c;
        std::vector<std::pair<std::uint32_t, SteppingController::time_point>>
            synced;
        c.setSyncClock(
            [&](std::uint32_t nodeId, SteppingController::time_point t) {
                synced.emplace_back(nodeId, t);
            });

        auto const start = c.now() + ms{1000};
        std::vector<std::uint32_t> ran;
        c.scheduleAt(
            start,
            Tier::process,
            /*nodeId=*/0,
            [&]() { ran.push_back(0); },
            HarnessScheduler::Kind::job);
        c.scheduleAt(
            start,
            Tier::process,
            /*nodeId=*/1,
            [&]() { ran.push_back(1); },
            HarnessScheduler::Kind::job);
        c.scheduleAt(
            start + ms{10},
            Tier::process,
            /*nodeId=*/2,
            [&]() { ran.push_back(2); },
            HarnessScheduler::Kind::job);
        c.scheduleAt(
            start + ms{20},
            Tier::process,
            /*nodeId=*/0,
            [&]() { ran.push_back(0); },
            HarnessScheduler::Kind::job);

        Pacer const pacer{
            /*k=*/1,
            /*unitCost=*/ms{30},
            Pacer::KindWeights::flat(),
            Pacer::NodeMultipliers::single(/*nodeId=*/0, /*value=*/10),
            Pacer::HorizonMode::perNode};
        SteppingController::ProfiledStepStats stats;

        auto const steps = c.profiledBeat(
            start,
            SteppingController::BeatSpec{
                /*nodeIds=*/{},
                /*dt=*/ms{100},
                /*skew=*/ms{0},
                /*fireHeartbeats=*/false},
            pacer,
            stats,
            [](std::uint32_t) {},
            []() { return false; },
            /*maxSteps=*/10);

        BEAST_EXPECT(steps == 4);
        BEAST_EXPECT((ran == std::vector<std::uint32_t>{0, 1, 2, 0}));
        BEAST_EXPECT(c.now() == start + ms{20});
        BEAST_EXPECT(stats.events == 4);
        BEAST_EXPECT(stats.clampHits == 2);
        BEAST_EXPECT(stats.requestedAdvance == ms{660});
        BEAST_EXPECT(stats.consumedAdvance == ms{160});
        BEAST_EXPECT(stats.weightedEvents == 22);
        BEAST_EXPECT(stats.firstClampEvent.nodeId == 0);
        BEAST_EXPECT(stats.firstClampRequested == ms{300});
        BEAST_EXPECT(stats.firstClampBudget == ms{100});
        BEAST_EXPECT(stats.firstClampWeight == 1);
        BEAST_EXPECT(stats.firstClampNodeMultiplier == 10);
        BEAST_EXPECT(stats.lag(0) == ms{500});
        BEAST_EXPECT(stats.lag(1) == ms{0});
        BEAST_EXPECT(stats.lag(2) == ms{0});
        if (BEAST_EXPECT(synced.size() == 4))
        {
            BEAST_EXPECT(synced[0] == std::make_pair(0u, start - ms{200}));
            BEAST_EXPECT(synced[1] == std::make_pair(1u, start));
            BEAST_EXPECT(synced[2] == std::make_pair(2u, start + ms{10}));
            BEAST_EXPECT(synced[3] == std::make_pair(0u, start - ms{480}));
        }
    }

    void
    testOffThreadConsensusJobFailsHard()
    {
        testcase(
            "consensus job off the stepping thread fails hard + is counted");
        SteppingController c;  // stepping thread = this test thread
        c.setSyncClock([](SteppingController::time_point) {});
        auto hook = c.makeJobHook(0);

        bool threw = false;
        std::thread other([&]() {
            try
            {
                hook(jtVALIDATION_t, "val", []() {});
            }
            catch (std::logic_error const&)
            {
                threw = true;
            }
        });
        other.join();

        BEAST_EXPECT(threw);
        BEAST_EXPECT(c.offThreadJobs() == 1);
        BEAST_EXPECT(c.empty());  // never touched the scheduler off-thread
    }

    void
    testDropPending()
    {
        testcase("dropPending releases pending events without running them");
        SteppingController c;
        c.setSyncClock([](SteppingController::time_point) {});
        auto hook = c.makeJobHook(0);
        bool ran = false;
        BEAST_EXPECT(hook(jtACCEPT, "AcceptLedger", [&]() {
                         ran = true;
                     }) == JobQueue::JobDisposition::claimedQueued);
        c.scheduleDelivery(0, ms{1}, [&]() { ran = true; });
        BEAST_EXPECT(c.scheduler().size() == 2);

        c.dropPending();
        BEAST_EXPECT(c.empty());
        BEAST_EXPECT(!ran);  // dropped, never executed
    }

public:
    void
    run() override
    {
        testClassify();
        testWalCheckpointScheduled();
        testNamedJobLagAndObservation();
        testLaggedPendingJobCount();
        testJobHookClosedWorld();
        testCanonicalAllJobsPolicy();
        testCanonicalNestedJobNotSwallowed();
        testCaughtUnmodeledJobFailsStep();
        testClockSyncRequiredAndApplied();
        testDeliveryPositiveDelay();
        testProfiledPerNodeHorizonDoesNotGloballyStall();
        testOffThreadConsensusJobFailsHard();
        testDropPending();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingController, consensus, ripple);

}  // namespace test
}  // namespace ripple
