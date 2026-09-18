#include <test/jtx.h>

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/rdb/backend/detail/Node.h>
#include <xrpld/core/DatabaseCon.h>
#include <xrpld/core/JobQueue.h>

#include <array>
#include <future>
#include <memory>
#include <string>
#include <thread>

namespace ripple {
namespace test {

class ConcurrentLedgerSave_test : public beast::unit_test::suite
{
    void
    run() override
    {
        testcase(
            "independent database pairs save without sharing SQL formatters");
        using namespace jtx;
        Env env(*this);
        Account const alice("alice");
        Account const bob("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        constexpr std::size_t writers = 4;
        constexpr int savesPerWriter = 50;
        std::array<std::shared_ptr<Ledger const>, writers> ledgers;
        std::array<uint256, writers> transactionIDs;
        for (std::size_t i = 0; i < writers; ++i)
        {
            env(pay(alice, bob, drops(i + 1)));
            transactionIDs[i] = env.tx()->getTransactionID();
            env.close();
            ledgers[i] = env.app().getLedgerMaster().getClosedLedger();
        }
        env.app().getJobQueue().rendezvous();

        // The production save function accepts independent database handles.
        // Separate connections make its local-storage ownership observable
        // without depending on multi-Application construction seams.
        auto const setup = setup_DatabaseCon(env.app().config());
        auto const journal = env.app().journal("ConcurrentLedgerSave");
        std::array<std::unique_ptr<DatabaseCon>, writers> ledgerDBs;
        std::array<std::unique_ptr<DatabaseCon>, writers> transactionDBs;
        for (std::size_t i = 0; i < writers; ++i)
        {
            ledgerDBs[i] = std::make_unique<DatabaseCon>(
                setup, LgrDBName, setup.lgrPragma, LgrDBInit, journal);
            transactionDBs[i] = std::make_unique<DatabaseCon>(
                setup, TxDBName, setup.txPragma, TxDBInit, journal);
        }

        std::promise<void> ready;
        auto start = ready.get_future().share();
        std::array<std::string, writers> errors;
        std::array<int, writers> saved{};
        std::array<std::thread, writers> threads;
        for (std::size_t i = 0; i < writers; ++i)
            threads[i] = std::thread([&, i] {
                start.wait();
                try
                {
                    for (int attempt = 0; attempt < savesPerWriter; ++attempt)
                    {
                        if (!ripple::detail::saveValidatedLedger(
                                *ledgerDBs[i],
                                *transactionDBs[i],
                                env.app(),
                                ledgers[i],
                                false))
                        {
                            errors[i] = "save returned false";
                            break;
                        }
                        ++saved[i];
                    }
                }
                catch (std::exception const& e)
                {
                    errors[i] = e.what();
                }
            });
        ready.set_value();
        for (auto& thread : threads)
            thread.join();

        for (std::size_t i = 0; i < writers; ++i)
        {
            BEAST_EXPECTS(errors[i].empty(), errors[i]);
            BEAST_EXPECT(saved[i] == savesPerWriter);
            auto const expectedSeq = ledgers[i]->info().seq;
            auto ledgerSession = ledgerDBs[i]->checkoutDb();
            auto transactionSession = transactionDBs[i]->checkoutDb();
            std::size_t rows = 0;
            std::uint32_t minSeq = 0, maxSeq = 0;
            *ledgerSession << "SELECT COUNT(*), MIN(LedgerSeq), MAX(LedgerSeq) "
                              "FROM Ledgers;",
                soci::into(rows), soci::into(minSeq), soci::into(maxSeq);
            BEAST_EXPECT(rows == 1);
            BEAST_EXPECT(minSeq == expectedSeq && maxSeq == expectedSeq);

            std::string hash;
            *ledgerSession << "SELECT LedgerHash FROM Ledgers;",
                soci::into(hash);
            BEAST_EXPECT(hash == to_string(ledgers[i]->info().hash));

            *transactionSession << "SELECT COUNT(*), MIN(LedgerSeq), "
                                   "MAX(LedgerSeq) FROM Transactions;",
                soci::into(rows), soci::into(minSeq), soci::into(maxSeq);
            BEAST_EXPECT(rows == 1);
            BEAST_EXPECT(minSeq == expectedSeq && maxSeq == expectedSeq);
            std::string txid;
            *transactionSession << "SELECT TransID FROM Transactions;",
                soci::into(txid);
            BEAST_EXPECT(txid == to_string(transactionIDs[i]));

            *transactionSession << "SELECT COUNT(*), MIN(LedgerSeq), "
                                   "MAX(LedgerSeq) FROM AccountTransactions;",
                soci::into(rows), soci::into(minSeq), soci::into(maxSeq);
            BEAST_EXPECT(rows == 2);
            BEAST_EXPECT(minSeq == expectedSeq && maxSeq == expectedSeq);
        }
    }
};

BEAST_DEFINE_TESTSUITE(ConcurrentLedgerSave, rdb, ripple);

}  // namespace test
}  // namespace ripple
