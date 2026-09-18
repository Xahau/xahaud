//------------------------------------------------------------------------------
// SteppingSpike (Stage 3) — the LONE-node guard for the strict stepping mode.
//
// History: this suite originally probed the build plan's "biggest unknown" — can
// a node run with ZERO background io threads (numberOfThreads → 0 under
// Config::steppingMode), driven from the test thread? It proved that AND the
// poll-pumped run()-shutdown teardown. Since then S3.3b.1 made steppingMode ALSO
// zero the JobQueue workers (so EVERY app-visible job must be claimed by the
// closed-world dispatch hook, else addRefCountedJob asserts), and S3.6 landed the
// full 2-node convergence proof (SteppingNet). So the old self-contained,
// hook-less bring-up is obsolete (it would assert at the first heartbeat job).
//
// This suite is now the minimal LONE-node case of the real stepping harness
// (MultiNode stepping mode): a single node boots with 0 io threads + 0 JobQueue
// workers, runs consensus entirely on the test thread via the scheduler, STALLS
// at the "too few peers" gate (a lone non-standalone validator never validates —
// orthogonal to io threads), and tears down cleanly (draining + dropPending +
// poll-pumped shutdown). It is the negative companion to SteppingNet's positive
// 2-node convergence. Spec: csf-peerimp-hybrid-overlay-harness.md (Stage 3).
//------------------------------------------------------------------------------
#include <test/jtx/MultiNode.h>
#include <test/jtx/SimOverlay.h>

#include <xrpld/overlay/Overlay.h>

#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <memory>
#include <vector>

namespace ripple {
namespace test {

class SteppingSpike_test : public beast::unit_test::suite
{
    void
    testLoneNodeStallsUnderStepping()
    {
        testcase(
            "lone stepping node: 0 io threads + 0 workers, peer-gate stall, "
            "clean teardown");
        using namespace std::chrono;

        // One stepping node (0 io threads, 0 JobQueue workers, inline strands,
        // closed-world hook), on a SimOverlay (no DNS / maintenance timer).
        MultiNode net(*this, /*virtualClock=*/true, /*stepping=*/true);
        auto const me = ValidatorKey::fromPassphrase("spike-node-0");
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        // UNL = {self}; even so, a non-standalone node won't propose/validate
        // while it has too few peers — which, with no simConnect(), is forever.
        net.add(TrustConfig{me.seed, std::vector<std::string>{me.pubKey}}, simFactory);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        // Drive heartbeats with NO peer connected. A lone non-standalone validator
        // stalls at the "too few peers" gate, so the valid ledger index must NOT
        // advance — proving the stall is the peer gate, not an io wedge, and that
        // 0 io threads + 0 workers + the closed-world hook is lifecycle-safe.
        auto const start = net.minValidated();
        auto const steps = net.runStepping(
            /*target=*/start + 2,  // unreachable for a lone node
            /*maxHeartbeats=*/30,
            /*maxSteps=*/50000,
            seconds{1});

        log << "  spike[lone]: validIndex start=" << start
            << " end=" << net.minValidated() << " after " << steps
            << " scheduler events (offThreadJobs="
            << net.controller().offThreadJobs()
            << ") — lone node stalls at the peer gate (expected)" << std::endl;

        BEAST_EXPECT(net.minValidated() == start);  // peer-gate stall, not io
        BEAST_EXPECT(net.controller().offThreadJobs() == 0);
        // Clean teardown (no hang) is asserted implicitly: ~MultiNode drains the
        // scheduler then poll-pumps each node's run() shutdown.
    }

public:
    void
    run() override
    {
        testLoneNodeStallsUnderStepping();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingSpike, consensus, ripple);

}  // namespace test
}  // namespace ripple
