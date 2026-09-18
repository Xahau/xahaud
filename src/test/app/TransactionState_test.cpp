#include <test/jtx.h>

#include <xrpld/app/misc/Transaction.h>
#include <xrpld/rpc/CTID.h>

#include <future>
#include <thread>

namespace ripple {
namespace test {

class TransactionState_test : public beast::unit_test::suite
{
    void
    run() override
    {
        testcase(
            "concurrent response readers observe coherent locator snapshots");
        using namespace jtx;
        Env env(*this, envconfig([](std::unique_ptr<Config> config) {
            config->NETWORK_ID = 11;
            return config;
        }));
        Account const alice("alice");
        env.fund(XRP(1000), alice);
        env.close();
        env(noop(alice));
        if (!BEAST_EXPECT(!env.tx()->isFieldPresent(sfNetworkID)))
            return;

        std::string reason;
        Transaction transaction(env.tx(), reason, env.app());
        BEAST_EXPECT(reason.empty());
        auto const& reader = transaction;
        auto const ctidA = RPC::encodeCTID(101, 7, 11);
        auto const ctidB = RPC::encodeCTID(202, 9, 22);
        if (!BEAST_EXPECT(ctidA && ctidB))
            return;

        auto setState = [&](bool second) {
            std::uint32_t const ledger = second ? 202 : 101;
            transaction.setStatus(
                COMMITTED, ledger, second ? 9 : 7, second ? 22 : 11);
            transaction.setCurrentLedgerState(
                ledger, XRPAmount{ledger}, ledger + 1, ledger + 2);
            transaction.setResult(second ? TER{terQUEUED} : TER{tesSUCCESS});
            transaction.clearSubmitResult();
            transaction.setApplied();
            transaction.setQueued();
            transaction.setBroadcast();
            transaction.setKept();
        };
        setState(false);

        std::promise<void> ready;
        auto start = ready.get_future().share();
        constexpr int iterations = 10000;
        int snapshots = 0;
        int inconsistent = 0;
        std::thread writer([&] {
            start.wait();
            for (int i = 0; i < iterations; ++i)
                setState(i % 2 != 0);
        });
        std::thread observer([&] {
            start.wait();
            for (int i = 0; i < iterations; ++i)
            {
                auto const json = reader.getJson(JsonOptions::none);
                auto const ledger = json[jss::ledger_index].asUInt();
                auto const ctid = json[jss::ctid].asString();
                if (!((ledger == 101 && ctid == *ctidA) ||
                      (ledger == 202 && ctid == *ctidB)))
                    ++inconsistent;
                auto const state = reader.getCurrentLedgerState();
                if (!state ||
                    state->minFeeRequired !=
                        XRPAmount{state->validatedLedger} ||
                    state->accountSeqNext != state->validatedLedger + 1 ||
                    state->accountSeqAvail != state->validatedLedger + 2)
                    ++inconsistent;
                auto const result = reader.getResult();
                if (result != tesSUCCESS && result != terQUEUED)
                    ++inconsistent;
                if (!reader.isValidated() || reader.getStatus() != COMMITTED)
                    ++inconsistent;
                auto const flags = reader.getSubmitResult();
                (void)flags;
                ++snapshots;
            }
        });
        ready.set_value();
        writer.join();
        observer.join();

        BEAST_EXPECT(snapshots == iterations);
        BEAST_EXPECT(inconsistent == 0);
        BEAST_EXPECT(reader.getLedger() == 202);
        BEAST_EXPECT(reader.getResult() == terQUEUED);
        auto flags = reader.getSubmitResult();
        BEAST_EXPECT(
            flags.applied && flags.queued && flags.broadcast && flags.kept);
        flags.clear();
        BEAST_EXPECT(reader.getSubmitResult().any());
        transaction.clearSubmitResult();
        BEAST_EXPECT(!reader.getSubmitResult().any());
    }
};

BEAST_DEFINE_TESTSUITE(TransactionState, app, ripple);

}  // namespace test
}  // namespace ripple
