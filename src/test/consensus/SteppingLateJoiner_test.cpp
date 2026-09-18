//------------------------------------------------------------------------------
// SteppingLateJoiner — §5.6b: a node JOINS an advanced network and catches up,
// under strict stepping, through the REAL ledger-acquire machinery.
//
// Five validators (quorum(5) = 4), but only nodes 0..3 are connected — they
// converge several ledgers while node 4 sits disconnected at genesis. Then
// node 4 connects and must CATCH UP: it hears proposals/validations for
// ledgers it never built, kicks the real acquire path (jtADVANCE GetConsL1/
// GetConsL2 -> InboundLedgers::acquireAsync -> TMGetLedger to peers ->
// jtLEDGER_REQ "RcvGetLedger" served from their state -> TMLedgerData back ->
// jtLEDGER_DATA "ProcessLData" -> ledger built -> checkAccept), walks its mode
// machine back to FULL, and rejoins validation. All of that traffic is
// scheduler-routed — so the entire catch-up replays bit-identically.
//
// This suite is what forced the acquire closure graph into classify() (the
// jobs above were fail-loud discoveries until 5.6b — see design-notes §2 for
// the skew boundary that first exposed them).
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpld/app/misc/NetworkOPs.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingLateJoiner_test : public beast::unit_test::suite
{
    struct JoinOutcome
    {
        std::uint32_t joinedAt = 0;   // minValidated when node 4 connected
        std::uint32_t caughtUp = 0;   // node 4's validSeq at the end
        std::vector<uint256> chain;   // node 4's hashes [2 .. caughtUp]
    };

    std::optional<JoinOutcome>
    runJoin()
    {
        SteppingNetwork net(*this);
        net.validators(5);
        if (!BEAST_EXPECT(net.allUp()))
            return std::nullopt;

        // Connect only 0..3 (quorum without node 4).
        for (std::uint32_t a = 0; a < 4; ++a)
            for (std::uint32_t b = a + 1; b < 4; ++b)
                if (!BEAST_EXPECT(net.connect(a, b) != nullptr))
                    return std::nullopt;

        // Phase 1: the quorum advances while node 4 sits dark at genesis.
        // (Progress is the DRIVEN set's — the dark node pins the global
        // minValidated at 0 by design.)
        net.runOnly({0, 1, 2, 3}, /*target=*/5);
        for (std::uint32_t n = 0; n < 4; ++n)
        {
            if (!BEAST_EXPECT(net.validSeq(n) >= 5))
            {
                log << "  quorum stalled; diagnostics: "
                    << net.jobDiagnostics() << std::endl;
                return std::nullopt;
            }
        }
        BEAST_EXPECT(net.validSeq(4) == 0);
        BEAST_EXPECT(net.mode(4) == OperatingMode::DISCONNECTED);

        JoinOutcome out;
        out.joinedAt = net.validSeq(0);

        // Phase 2: node 4 connects to everyone and the whole network runs.
        // Catch-up = hear about a future ledger, acquire the chain for real.
        for (std::uint32_t peer = 0; peer < 4; ++peer)
            if (!BEAST_EXPECT(net.connect(4, peer) != nullptr))
                return std::nullopt;

        auto const target = out.joinedAt + 3;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }

        out.caughtUp = net.validSeq(4);
        BEAST_EXPECT(out.caughtUp >= target);
        BEAST_EXPECT(net.mode(4) == OperatingMode::FULL);
        // The joiner agrees with the network on the ledgers it acquired.
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);

        for (std::uint32_t seq = 2; seq <= target; ++seq)
            out.chain.push_back(net.ledgerHash(4, seq));
        return out;
    }

    void
    testLateJoinerCatchesUp()
    {
        testcase(
            "a 5th validator joins a 4-of-5 quorum network and catches up "
            "through the real acquire path");
        auto const out = runJoin();
        if (out)
            log << "  joined at validated seq " << out->joinedAt
                << ", caught up to " << out->caughtUp << std::endl;
        BEAST_EXPECT(out.has_value());
    }

    void
    testCatchUpReplays()
    {
        testcase("the whole join-and-catch-up timeline replays identically");
        auto const run1 = runJoin();
        auto const run2 = runJoin();
        if (!BEAST_EXPECT(run1 && run2))
            return;
        BEAST_EXPECT(run1->joinedAt == run2->joinedAt);
        BEAST_EXPECT(run1->caughtUp == run2->caughtUp);
        BEAST_EXPECT(run1->chain == run2->chain);
        log << "  catch-up chains "
            << (run1->chain == run2->chain ? "MATCH" : "DIFFER") << std::endl;
    }

public:
    void
    run() override
    {
        testLateJoinerCatchesUp();
        testCatchUpReplays();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingLateJoiner, consensus, ripple);

}  // namespace ripple::test
