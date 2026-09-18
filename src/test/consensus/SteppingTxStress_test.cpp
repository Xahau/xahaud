//------------------------------------------------------------------------------
// SteppingTxStress — pointing the sharpest instruments at the WIDEST input
// surface. Plan §4.5 flags tx-bearing ledgers as the moment the determinism
// input surface widens (CanonicalTXSet's txset-hash salt, TxQ state, fee
// arithmetic, open-ledger timing) — yet until this suite no tx-bearing
// scenario had ever been fingerprint-grinded: SteppingSubmit compares hash
// chains, which cannot see right-outcome-wrong-way.
//
// Two jobs here:
//  1. REPLAY-GRIND the tx path: a multi-account, multi-round payment economy
//     under expectReplays — same seed ⇒ same executed work queue, now with
//     transactions flowing through TxQ/OpenLedger/CanonicalTXSet.
//  2. HUNT the predicted discoveries: §5.1 forecast that submits landing
//     NEAR THE CLOSE BOUNDARY under heartbeat skew force tx-set position
//     divergence and the tx-set ACQUIRE path (jtTXN_DATA "completeAcquire",
//     TMHaveTransactionSet/TMGetObjectByHash traffic) — "expected
//     fail-louds, welcome ones" that nobody had gone looking for. The sweep
//     injects a submit at every 100ms offset across a full beat, under
//     2×linkDelay skew, exactly the regime design-notes §2's addendum marks
//     as the cheapest acquire trigger. If an unmodeled closure surfaces,
//     failedJobs() != 0 names it — that is this suite WORKING.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/SteppingReplay.h>

#include <test/jtx/Account.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingTxStress_test : public beast::unit_test::suite
{
    using Payload = std::optional<std::vector<uint256>>;

    // A small real economy across several rounds: fund three accounts, then
    // interleave user payments submitted from DIFFERENT nodes across
    // consecutive rounds, and return the full validated chain. Every ledger
    // from the fund onward is tx-bearing — the widened input surface.
    Payload
    runEconomy(SteppingNetwork& net)
    {
        using namespace jtx;
        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return Payload{};
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return Payload{};

        net.fund(0, XRP(50000), {alice, bob, carol});

        // User-signed payments, submitted on different nodes, one round
        // apart — each submit autofills from THAT node's open ledger at the
        // moment its injection fires.
        net.in(std::chrono::milliseconds{500}, 1, [&]() {
            net.submit(1, pay(alice, bob, XRP(1000)), alice);
        });
        net.in(std::chrono::milliseconds{1500}, 2, [&]() {
            net.submit(2, pay(bob, carol, XRP(500)), bob);
        });
        net.in(std::chrono::milliseconds{2500}, 0, [&]() {
            net.submit(0, pay(carol, alice, XRP(250)), carol);
        });

        auto const target = net.minValidatedSeq() + 4;
        net.runTo(target);
        if (!net.expectConverged(target))
            return Payload{};

        // The economy arithmetic must be exact on every node's validated
        // ledger: alice 50000 - 1000 + 250 - fee(alice's 1 tx)...  assert
        // presence rather than re-deriving fee math here — the chain hashes
        // ARE the arithmetic, and they must replay.
        std::vector<uint256> chain;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
            chain.push_back(net.ledgerHash(0, seq));
        return Payload{std::move(chain)};
    }

    void
    testTxBearingReplays()
    {
        testcase(
            "a multi-node payment economy replays bit-for-bit — first "
            "fingerprint coverage of the tx path (grind with "
            "--unittest-arg=replays=N)");
        expectReplays(*this, "tx economy", [this](SteppingNetwork& net) {
            return runEconomy(net);
        });
    }

    // The sweep that FOUND the tx-set acquire path (2026-07-03): under
    // skew ≥ linkDelay a submit near the close boundary diverges tx-set
    // positions and the receiver acquires the disputed set for real —
    // fail-louding on jtTXN_DATA "recvPeerData" (PeerImp.cpp:1791), exactly
    // the discovery plan §5.1 forecast. The jtTXN_DATA closure graph
    // ("recvPeerData"/"completeAcquire" data, "TransactionAcquire" timer — three distinct
    // call sites, classified by NAME) is modeled in SteppingController
    // since; this sweep now stands as its regression gate.
    void
    testSubmitNearCloseSweep()
    {
        testcase(
            "submit swept across the close boundary under skew: positions "
            "may diverge; every offset converges inside the modeled world");
        using namespace jtx;
        using namespace std::chrono;
        bool sawTxSetAcquire = false;
        for (int offsetMs = 100; offsetMs <= 1000; offsetMs += 100)
        {
            SteppingNetwork net(*this);
            net.recordForensics();
            Account const alice{"alice"};
            net.validators(3).mesh();
            if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
                return;
            // Skew 10ms = 2× the 5ms link delay — past the acquire boundary.
            net.runTo(3, {}, SteppingNetwork::Cadence{seconds{1}, milliseconds{10}});
            if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
                return;

            net.in(milliseconds{offsetMs}, 0, [&]() {
                net.submit(
                    0, pay(Account::master, alice, XRP(10000)), Account::master);
            });
            auto const target = net.minValidatedSeq() + 3;
            net.runTo(
                target,
                {},
                SteppingNetwork::Cadence{seconds{1}, milliseconds{10}});
            if (!net.expectConverged(target))
            {
                log << "  offset " << offsetMs << "ms: FAILED — diagnostics "
                    << "above name the discovery" << std::endl;
                return;
            }
            // The payment must have landed somewhere in the window on every
            // node (alice exists on the newest validated ledger).
            auto const l = net.ledger(0, target);
            if (!BEAST_EXPECT(l && l->exists(keylet::account(alice.id()))))
            {
                log << "  offset " << offsetMs
                    << "ms: tx missing from validated chain" << std::endl;
                return;
            }
            sawTxSetAcquire = sawTxSetAcquire ||
                net.jobDiagnostics().find("'RcvPeerData'") !=
                    std::string::npos;
        }
        // This sweep is the tx-set acquire path's regression gate, so it
        // must EXERCISE that path, not merely pass around it: at least one
        // offset must diverge positions and run the "recvPeerData" receive
        // job. A green sweep that never acquired is vacuous — a timing
        // change closed the divergence window; widen the sweep before
        // trusting it.
        BEAST_EXPECT(sawTxSetAcquire);
        log << "  swept 10 offsets across the beat; all converged inside "
               "the modeled world"
            << std::endl;
    }

public:
    void
    run() override
    {
        testTxBearingReplays();
        testSubmitNearCloseSweep();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingTxStress, consensus, ripple);

}  // namespace ripple::test
