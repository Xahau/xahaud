//------------------------------------------------------------------------------
// SteppingSubmit — the §5.1 keystone spike (harness-evolution-plan.md §7.3).
//
// Proves a transaction can be submitted through the REAL local submission entry
// (NetworkOPs::processTransaction, bLocal=true) under strict stepping, relayed
// over the real overlay to every validator, agreed in consensus, and applied in
// the same validated ledger on all N nodes — with the closed world intact
// (zero off-thread jobs, zero unmodeled-job failures).
//
// The scenario is also the fund-lite: genesis funds the master account
// ("masterpassphrase" == jtx::Account::master), so the first meaningful submit
// IS `pay(master, alice, XRP(n))` — it creates alice's AccountRoot on every
// node, which the test reads back directly from each node's validated ledger.
//
// Job-model coverage this suite pins (SteppingController::classify additions):
//   jtTRANSACTION "SubmitTxn" / "RcvCheckTx"  -> Tier::process
//   jtBATCH       "TxBatchAsync"/"TxBatchSync" -> Tier::process
//
// And the §4.5 determinism invariant, now with a tx in play: two independent
// runs of the same scenario must produce identical validated hash CHAINS and
// land the tx in the same ledger seq.
//------------------------------------------------------------------------------
#include <test/jtx/SteppingNetwork.h>
#include <test/jtx/amount.h>
#include <test/jtx/pay.h>

#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/unit_test/suite.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TER.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ripple::test {

class SteppingSubmit_test : public beast::unit_test::suite
{
    struct RunOutcome
    {
        std::vector<uint256> chain;  // validated hashes [2 .. target]
        std::uint32_t txSeq = 0;     // the seq the payment landed in
    };

    // 3 validators, full mesh, steady state; then master pays alice on node 0
    // and the network runs until the tx-bearing ledger fully validates
    // everywhere. Returns the hash chain + tx seq (nullopt on failure).
    std::optional<RunOutcome>
    runScenario()
    {
        using namespace jtx;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return std::nullopt;

        // Steady state first: submit into a synced, proposing network.
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return std::nullopt;

        Account const alice{"alice"};
        auto const txn = net.submit(
            0, pay(Account::master, alice, XRP(10000)), Account::master);

        // The open-ledger apply ran INLINE during submit(); the tx must be in
        // node 0's open ledger now.
        BEAST_EXPECT(txn->getResult() == tesSUCCESS);
        auto const txid = txn->getID();

        auto const target = net.minValidatedSeq() + 3;
        auto const steps = net.runTo(target);
        log << "  submit: " << steps
            << " scheduler events to minValidated=" << net.minValidatedSeq()
            << ", offThreadJobs=" << net.offThreadJobs()
            << ", failedJobs=" << net.failedJobs() << std::endl;
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return std::nullopt;
        }
        BEAST_EXPECT(net.offThreadJobs() == 0);
        BEAST_EXPECT(net.failedJobs() == 0);

        // Find the validated ledger that carries the payment.
        std::uint32_t txSeq = 0;
        for (std::uint32_t seq = 3; seq <= target; ++seq)
        {
            if (auto const l = net.ledger(0, seq); l && l->txExists(txid))
            {
                txSeq = seq;
                break;
            }
        }
        if (!BEAST_EXPECT(txSeq != 0))
            return std::nullopt;
        // Same ledger (bit-identical) on all three validators...
        BEAST_EXPECT(net.ledgersAgree(txSeq));
        BEAST_EXPECT(net.ledgersAgree(target));

        // ...and alice exists with the full amount on EVERY node's validated
        // ledger (she paid no fee; master did).
        for (std::uint32_t n = 0; n < 3; ++n)
        {
            auto const l = net.ledger(n, target);
            if (!BEAST_EXPECT(l != nullptr))
                return std::nullopt;
            auto const sle = l->read(keylet::account(alice.id()));
            if (BEAST_EXPECT(sle != nullptr))
                BEAST_EXPECT(
                    sle->getFieldAmount(sfBalance) == XRP(10000).value());
        }

        RunOutcome out;
        out.txSeq = txSeq;
        for (std::uint32_t seq = 2; seq <= target; ++seq)
        {
            auto const h = net.ledgerHash(0, seq);
            BEAST_EXPECT(h != uint256{});
            out.chain.push_back(h);
        }
        return out;
    }

    void
    testSubmitPropagatesAndApplies()
    {
        testcase(
            "master pays alice on one node; all validators apply it in the "
            "same validated ledger");
        auto const out = runScenario();
        if (out)
            log << "  payment landed in validated seq " << out->txSeq
                << std::endl;
        BEAST_EXPECT(out.has_value());
    }

    void
    testApplyOrderAgreesAcrossNodes()
    {
        testcase(
            "appliedTxs: two payments land in one ledger, in the same apply "
            "order with the same TERs on every node (5.2 reader)");
        using namespace jtx;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return;

        // Two payments from master, submitted back-to-back on node 0. Each
        // apply runs inline, so the second fillSeq reads the open ledger with
        // the first already applied — consecutive master sequences.
        Account const alice{"alice"};
        Account const bob{"bob"};
        auto const tx1 = net.submit(
            0, pay(Account::master, alice, XRP(10000)), Account::master);
        auto const tx2 = net.submit(
            0, pay(Account::master, bob, XRP(5000)), Account::master);
        BEAST_EXPECT(tx1->getResult() == tesSUCCESS);
        BEAST_EXPECT(tx2->getResult() == tesSUCCESS);

        auto const target = net.minValidatedSeq() + 3;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return;
        }
        BEAST_EXPECT(net.failedJobs() == 0);

        // Both were in the same open ledger, so they close together.
        std::uint32_t txSeq = 0;
        for (std::uint32_t seq = 3; seq <= target; ++seq)
        {
            if (auto const l = net.ledger(0, seq);
                l && l->txExists(tx1->getID()))
            {
                txSeq = seq;
                break;
            }
        }
        if (!BEAST_EXPECT(txSeq != 0))
            return;

        auto const reference = net.appliedTxs(0, txSeq);
        if (!BEAST_EXPECT(reference.size() == 2))
            return;
        // Same account, consecutive sequences → the canonical order MUST apply
        // them in sequence order: alice's payment strictly before bob's.
        BEAST_EXPECT(reference[0].txid == tx1->getID());
        BEAST_EXPECT(reference[1].txid == tx2->getID());
        for (auto const& a : reference)
        {
            BEAST_EXPECT(a.result == tesSUCCESS);
            BEAST_EXPECT(a.account == Account::master.id());
        }

        // Every node reproduces the exact same apply order and results.
        for (std::uint32_t n = 1; n < 3; ++n)
        {
            auto const applied = net.appliedTxs(n, txSeq);
            if (!BEAST_EXPECT(applied.size() == reference.size()))
                continue;
            for (std::size_t k = 0; k < applied.size(); ++k)
            {
                BEAST_EXPECT(applied[k].index == reference[k].index);
                BEAST_EXPECT(applied[k].txid == reference[k].txid);
                BEAST_EXPECT(applied[k].result == reference[k].result);
            }
        }

        log << "  two payments applied at seq " << txSeq
            << " in identical order on all nodes" << std::endl;
    }

    void
    testFundedEconomy()
    {
        testcase(
            "fund + user-to-user payment: accounts funded via one node, "
            "alice pays bob signed by alice via another (5.3)");
        using namespace jtx;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return;

        // Fund two user accounts, submitted on node 1 (not node 0 — funding
        // must work from any node). fund() is fail-loud: it throws unless the
        // roots validate on every node.
        Account const alice{"alice"};
        Account const bob{"bob"};
        net.fund(1, XRP(10000), {alice, bob});

        // A USER-signed payment (not master), submitted on node 2: alice pays
        // bob. Exercises real sequence/fee autofill from alice's account root
        // and real signature checking on the submission path.
        auto const txn = net.submit(2, pay(alice, bob, XRP(100)), alice);
        BEAST_EXPECT(txn->getResult() == tesSUCCESS);
        auto const fee = txn->getSTransaction()->getFieldAmount(sfFee);

        auto const target = net.minValidatedSeq() + 3;
        net.runTo(target);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= target))
        {
            log << "  diagnostics: " << net.jobDiagnostics() << std::endl;
            return;
        }
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(net.ledgersAgree(target));

        // Balances on EVERY node's validated ledger: alice paid 100 XRP + the
        // fee; bob received 100 XRP on top of his 10000 funding.
        auto const aliceExpected =
            STAmount{XRP(10000).value()} - XRP(100).value() - fee;
        auto const bobExpected =
            STAmount{XRP(10000).value()} + XRP(100).value();
        for (std::uint32_t n = 0; n < 3; ++n)
        {
            auto const l = net.ledger(n, target);
            if (!BEAST_EXPECT(l != nullptr))
                continue;
            auto const aliceSle = l->read(keylet::account(alice.id()));
            auto const bobSle = l->read(keylet::account(bob.id()));
            if (BEAST_EXPECT(aliceSle && bobSle))
            {
                BEAST_EXPECT(
                    aliceSle->getFieldAmount(sfBalance) == aliceExpected);
                BEAST_EXPECT(bobSle->getFieldAmount(sfBalance) == bobExpected);
            }
        }

        log << "  funded economy validated to seq " << target << std::endl;
    }

    void
    testRpcReadSurface()
    {
        testcase(
            "socketless rpc: account_info / ledger / fee / server_info agree "
            "with direct ReadView state; role gating enforced (5.4)");
        using namespace jtx;

        SteppingNetwork net(*this);
        net.validators(3).mesh();
        if (!BEAST_EXPECT(net.allUp() && net.meshReady()))
            return;
        net.runTo(3);
        if (!BEAST_EXPECT(net.minValidatedSeq() >= 3))
            return;

        Account const alice{"alice"};
        net.fund(0, XRP(10000), {alice});

        // account_info: every node reports alice's funded balance (drops).
        for (std::uint32_t n = 0; n < 3; ++n)
        {
            Json::Value params{Json::objectValue};
            params[jss::account] = alice.human();
            auto const r = net.rpc(n, "account_info", params);
            if (!BEAST_EXPECT(r.isMember(jss::account_data)))
            {
                log << "  account_info[" << n << "]: " << r << std::endl;
                continue;
            }
            BEAST_EXPECT(
                r[jss::account_data]["Balance"].asString() == "10000000000");
            BEAST_EXPECT(r[jss::account_data].isMember("Sequence"));
        }

        // ledger: the RPC's validated-ledger hash equals the ReadView's.
        {
            Json::Value params{Json::objectValue};
            params[jss::ledger_index] = "validated";
            auto const r = net.rpc(0, "ledger", params);
            if (BEAST_EXPECT(r.isMember(jss::ledger)))
            {
                auto const seq = net.validSeq(0);
                BEAST_EXPECT(
                    r[jss::ledger][jss::ledger_hash].asString() ==
                    to_string(net.ledgerHash(0, seq)));
            }
        }

        // fee: base fee drops from the open ledger.
        {
            auto const r = net.rpc(0, "fee");
            if (BEAST_EXPECT(r.isMember(jss::drops)))
                BEAST_EXPECT(r[jss::drops][jss::base_fee].asString() == "10");
        }

        // server_info: answers inline under stepping.
        BEAST_EXPECT(net.rpc(0, "server_info").isMember(jss::info));

        // Role gating: an admin-only command as Role::USER is refused.
        {
            auto const r =
                net.rpc(0, "peers", Json::Value{Json::objectValue}, Role::USER);
            BEAST_EXPECT(r.isMember(jss::error));
        }

        // The whole read surface ran without leaving the modeled job world.
        BEAST_EXPECT(net.failedJobs() == 0);
        BEAST_EXPECT(net.offThreadJobs() == 0);
    }

    void
    testSubmitIsDeterministic()
    {
        testcase("tx-bearing runs replay identically (hash chain + tx seq)");
        auto const run1 = runScenario();
        auto const run2 = runScenario();
        if (!BEAST_EXPECT(run1 && run2))
            return;
        BEAST_EXPECT(run1->txSeq == run2->txSeq);
        BEAST_EXPECT(run1->chain == run2->chain);
        log << "  run1 txSeq=" << run1->txSeq << " run2 txSeq=" << run2->txSeq
            << ", chains " << (run1->chain == run2->chain ? "MATCH" : "DIFFER")
            << std::endl;
    }

public:
    void
    run() override
    {
        testSubmitPropagatesAndApplies();
        testApplyOrderAgreesAcrossNodes();
        testFundedEconomy();
        testRpcReadSurface();
        testSubmitIsDeterministic();
    }
};

BEAST_DEFINE_TESTSUITE(SteppingSubmit, consensus, ripple);

}  // namespace ripple::test
