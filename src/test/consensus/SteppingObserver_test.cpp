//------------------------------------------------------------------------------
// SteppingObserver — §5.6c: a non-validator OBSERVER node (UNL but no signing
// identity) rides along with a validator network — the shape of a real
// client-facing tracking node, and the first per-node TRUST asymmetry in the
// harness (every earlier suite gave all nodes identical validator configs).
//
// Proves: the observer fully-validates the chain it merely watches (quorum
// comes from the validators it trusts), walks its mode machine to FULL,
// never contributes a validation of its own — and works as a SUBMISSION
// GATEWAY: a client transaction submitted through the observer relays to the
// validators, gets agreed, and comes back to the observer as validated state.
// Deterministically, of course: the whole ride replays bit-identically.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TER.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingObserver_test : public beast::unit_test::suite
{
    struct ObserverOutcome
    {
        std::uint32_t txSeq = 0;     // seq of the ledger carrying the payment
        std::vector<uint256> chain;  // observer's validated hashes [2..target]
    };

    std::optional<ObserverOutcome>
    runObserverScenario()
    {
        using namespace jtx;

        SteppingNetwork net(*this);
        net.validators(3);
        auto const obs = net.observer();  // node 3: UNL of the 3, no seed
        net.mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;

        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }

        // The observer tracks the chain it cannot vote on: fully-validated
        // seq keeps pace and its mode machine reaches FULL.
        BEAST_EXPECT(net.validSeq(obs) >= 3);
        BEAST_EXPECT(net.mode(obs) == OperatingMode::FULL);

        // A client submits THROUGH the observer: the tx relays to the
        // validators, is agreed, and validates everywhere.
        Account const alice{"alice"};
        auto const txn = net.submit(
            obs, pay(Account::master, alice, XRP(10000)), Account::master);
        BEAST_EXPECT(txn->getResult() == tesSUCCESS);

        auto const target = net.minValidatedSeq() + 3;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        BEAST_EXPECT(net.ledgersAgree(target));
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);

        ObserverOutcome out;
        for (std::uint32_t seq = 3; seq <= target; ++seq)
            if (auto const l = net.ledger(obs, seq);
                l && l->txExists(txn->getID()))
            {
                out.txSeq = seq;
                break;
            }
        if (!BEAST_EXPECT(out.txSeq != 0))
            return std::nullopt;

        // Alice exists on the observer's validated ledger too — client-visible
        // state without ever validating a thing.
        auto const l = net.ledger(obs, target);
        if (BEAST_EXPECT(l != nullptr))
        {
            auto const sle = l->read(keylet::account(alice.id()));
            if (BEAST_EXPECT(sle != nullptr))
                BEAST_EXPECT(
                    sle->getFieldAmount(sfBalance) == XRP(10000).value());
        }

        for (std::uint32_t seq = 2; seq <= target; ++seq)
            out.chain.push_back(net.ledgerHash(obs, seq));
        return out;
    }

    void
    testObserverTracksAndGateways()
    {
        testcase(
            "a non-validator observer fully-validates the watched chain and "
            "gateways client submissions to the validators");
        auto const out = runObserverScenario();
        if (out)
            log << "  gatewayed payment validated at seq " << out->txSeq
                << std::endl;
        BEAST_EXPECT(out.has_value());
    }

    void
    testObserverRideReplays()
    {
        testcase("the observer's ride replays identically");
        auto const run1 = runObserverScenario();
        auto const run2 = runObserverScenario();
        if (!BEAST_EXPECT(run1 && run2))
            return;
        BEAST_EXPECT(run1->txSeq == run2->txSeq);
        BEAST_EXPECT(run1->chain == run2->chain);
        log << "  observer chains "
            << (run1->chain == run2->chain ? "MATCH" : "DIFFER") << std::endl;
    }

public:
    void
    run() override
    {
        testObserverTracksAndGateways();
        testObserverRideReplays();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingObserver, consensus, ripple);

}  // namespace ripple::test
