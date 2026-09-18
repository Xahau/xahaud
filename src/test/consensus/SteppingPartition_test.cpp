//------------------------------------------------------------------------------
// SteppingPartition — plan §6 formalized: deterministic quorum loss under
// partition, and deterministic recovery after heal, under strict stepping.
//
// Three validators (UNL 3 → quorum 3: every validator must agree, the
// intended safety posture). Cutting one node makes quorum unreachable for
// EVERYONE: the two connected survivors keep closing ledgers together (their
// closedSeq advances) but can never fully validate; the isolated node's
// heartbeat stalls at the peer gate (DISCONNECTED, frozen at the partition
// point). Healing reconnects the third node, the network re-converges — the
// isolated node catching up through the (now modeled) acquire path — and full
// validation resumes. The entire stall-and-recover timeline replays
// bit-identically, which is what makes partition scenarios non-flaky here
// where their wall-clock ancestors were deferred as inherently flaky.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpld/app/misc/NetworkOPs.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingPartition_test : public beast::unit_test::suite
{
    struct PartitionOutcome
    {
        std::uint32_t stalledAt = 0;    // validated seq when the cut happened
        std::uint32_t survivorClosed = 0;  // survivors' closedSeq during stall
        std::uint32_t recoveredTo = 0;  // validated seq after heal
        std::vector<uint256> chain;     // node 2's hashes [2 .. recoveredTo]
    };

    std::optional<PartitionOutcome>
    runPartitionHeal()
    {
        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;

        // Healthy phase.
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        PartitionOutcome out;
        out.stalledAt = net.minValidatedSeq();

        // Partition: isolate node 2 (sever its wires, settle the EOFs, flush
        // the io cleanup so its peer slots free up for the later reconnect).
        net.isolateNodeAndFlush(2);

        // Quorum lost: a generous beat budget must not advance ANY node's
        // fully-validated ledger (UNL 3 → quorum 3, and one validator is dark).
        auto const unreachable = out.stalledAt + 1;
        net.runTo(unreachable, SteppingNetwork::RunBudget{/*heartbeats=*/15});
        for (std::uint32_t n = 0; n < 3; ++n)
            BEAST_EXPECT(net.validSeq(n) <= out.stalledAt);
        // ...but the two connected survivors kept CLOSING ledgers together
        // (closed-vs-validated divergence — the quorum-loss signature).
        out.survivorClosed = net.closedSeq(0);
        BEAST_EXPECT(out.survivorClosed > out.stalledAt);
        BEAST_EXPECT(net.closedSeq(1) == out.survivorClosed);
        // The isolated node stalls at the peer gate.
        BEAST_EXPECT(net.mode(2) == OperatingMode::DISCONNECTED);

        // Heal: reconnect node 2 and run. It must catch up (acquire path) and
        // full validation must resume across all three.
        net.reconnectNode(2);
        auto const target = out.stalledAt + 4;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        out.recoveredTo = net.minValidatedSeq();
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.mode(2) == OperatingMode::FULL);
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);

        for (std::uint32_t seq = 2; seq <= target; ++seq)
            out.chain.push_back(net.ledgerHash(2, seq));
        return out;
    }

    void
    testPartitionStallsThenHealRecovers()
    {
        testcase(
            "partition loses quorum deterministically (survivors close but "
            "cannot validate); heal recovers full validation");
        auto const out = runPartitionHeal();
        if (out)
            log << "  stalled at " << out->stalledAt << " (survivors closed to "
                << out->survivorClosed << "), recovered to " << out->recoveredTo
                << std::endl;
        BEAST_EXPECT(out.has_value());
    }

    void
    testPartitionHealReplays()
    {
        testcase("the stall-and-recover timeline replays identically");
        auto const run1 = runPartitionHeal();
        auto const run2 = runPartitionHeal();
        if (!BEAST_EXPECT(run1 && run2))
            return;
        BEAST_EXPECT(run1->stalledAt == run2->stalledAt);
        BEAST_EXPECT(run1->survivorClosed == run2->survivorClosed);
        BEAST_EXPECT(run1->recoveredTo == run2->recoveredTo);
        BEAST_EXPECT(run1->chain == run2->chain);
        log << "  partition/heal chains "
            << (run1->chain == run2->chain ? "MATCH" : "DIFFER") << std::endl;
    }

public:
    void
    run() override
    {
        testPartitionStallsThenHealRecovers();
        testPartitionHealReplays();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingPartition, consensus, ripple);

}  // namespace ripple::test
