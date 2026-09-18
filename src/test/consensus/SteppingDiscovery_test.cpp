//------------------------------------------------------------------------------
// SteppingDiscovery (Stage 3, S3.3b discovery phase) — the cheap, decisive
// first experiment of the deterministic push: ENUMERATE every JobQueue job type
// that an empty-ledger 2-node consensus actually posts, and from which thread.
// It reuses the working hybrid convergence (rung K: SimOverlay + simConnect +
// virtual ticks) and just OBSERVES, via an install-before-setup dispatch hook
// that records the type then returns false (jobs still run on workers — pure
// observation).
//
// This is the "closed world" the strict stepping mode must model or gate: each
// type here is either a consensus event we route onto the scheduler, or a
// background poster we must turn off. "posted off-driver-thread" flags the
// types that, in the hybrid, come from worker/io threads — i.e. the timing
// leaks the deterministic mode has to eliminate. Spec: csf-...-harness.md
// (Stage 3).
//------------------------------------------------------------------------------
#include <test/jtx/MultiNode.h>
#include <test/jtx/SimOverlay.h>
#include <test/jtx/SimTransport.h>

#include <xrpld/core/Job.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/core/JobTypes.h>
#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>

namespace ripple {
namespace test {

class SteppingDiscovery_test : public beast::unit_test::suite
{
    void
    testEnumerateJobs()
    {
        testcase(
            "discovery: job types posted during empty-ledger 2-node "
            "convergence");
        using namespace std::chrono_literals;

        // Thread-safe recorder, keyed by (JobType, per-job NAME). The name
        // matters: one JobType can serve several call sites (e.g. jtADVANCE is
        // used for both AdvanceLedger and the GetConsL1/GetConsL2
        // ledger-ACQUIRE paths), so the closed world must classify by (type,
        // name), not type alone.
        struct Key
        {
            JobType type;
            std::string name;
            bool
            operator<(Key const& o) const
            {
                return std::tie(type, name) < std::tie(o.type, o.name);
            }
        };
        struct Recorder
        {
            std::mutex m;
            std::map<Key, std::size_t> counts;
            std::set<Key> offDriver;
            std::thread::id const driver{std::this_thread::get_id()};

            void
            rec(JobType t, std::string const& n)
            {
                std::scoped_lock lk(m);
                Key const k{t, n};
                ++counts[k];
                if (std::this_thread::get_id() != driver)
                    offDriver.insert(k);
            }
        } recorder;

        // Observe-only hook (pass → jobs still run on workers).
        auto recHook = [&recorder](
                           JobType t,
                           std::string const& name,
                           JobQueue::JobFunction const&) {
            recorder.rec(t, name);
            return JobQueue::JobDisposition::pass;
        };

        MultiNode net(*this, /*virtualClock=*/true);
        auto const valA = ValidatorKey::fromPassphrase("disc-validator-A");
        auto const valB = ValidatorKey::fromPassphrase("disc-validator-B");
        std::vector<std::string> const unl{valA.pubKey, valB.pubKey};
        auto simFactory = [](Application& app) {
            return std::unique_ptr<Overlay>(std::make_unique<SimOverlay>(app));
        };
        net.add(
            TrustConfig{valA.seed, unl},
            simFactory,
            recHook,
            /*bindServerListeners=*/false);
        net.add(
            TrustConfig{valB.seed, unl},
            simFactory,
            recHook,
            /*bindServerListeners=*/false);
        if (!BEAST_EXPECT(net.allUp()))
            return;

        simConnect(net[0].app(), net[1].app());
        if (!BEAST_EXPECT(net.waitForPeers(1, 20s)))
            return;

        constexpr std::size_t kMaxTicks = 200;
        auto const ticks = net.runVirtual(/*target=*/2, kMaxTicks, 1s);
        bool const converged = net.minValidated() >= 2;
        BEAST_EXPECT(converged);

        // The inventory: the closed world the strict stepping mode must cover.
        std::scoped_lock lk(recorder.m);
        log << "  --- empty-ledger 2-node job inventory (" << ticks
            << " ticks, converged=" << (converged ? "yes" : "NO") << ") ---"
            << std::endl;
        for (auto const& [k, c] : recorder.counts)
        {
            bool const off = recorder.offDriver.count(k) != 0;
            log << "    type=" << JobTypes::name(k.type) << " (jt#"
                << static_cast<int>(k.type) << ") name=\"" << k.name
                << "\" count=" << c
                << (off ? "  [off-driver-thread]" : "  [driver-thread only]")
                << std::endl;
        }
        log << "  --- " << recorder.counts.size()
            << " distinct (type,name) jobs posted ---" << std::endl;
    }

public:
    void
    run() override
    {
        testEnumerateJobs();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingDiscovery, consensus, ripple);

}  // namespace test
}  // namespace ripple
