//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2015 Ripple Labs Inc.

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/main/Application.h>
#include <ripple/app/misc/SHAMapStore.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>
#include <test/jtx/envconfig.h>

namespace ripple {
namespace test {

class SHAMapStore_test : public beast::unit_test::suite
{
    static auto const deleteInterval = 8;

    static auto
    onlineDelete(std::unique_ptr<Config> cfg)
    {
        cfg->LEDGER_HISTORY = deleteInterval;
        auto& section = cfg->section(ConfigSection::nodeDatabase());
        section.set("online_delete", std::to_string(deleteInterval));
        return cfg;
    }

    static auto
    advisoryDelete(std::unique_ptr<Config> cfg)
    {
        cfg = onlineDelete(std::move(cfg));
        cfg->section(ConfigSection::nodeDatabase()).set("advisory_delete", "1");
        return cfg;
    }

    bool
    goodLedger(
        jtx::Env& env,
        Json::Value const& json,
        std::string ledgerID,
        bool checkDB = false)
    {
        auto good = json.isMember(jss::result) &&
            !RPC::contains_error(json[jss::result]) &&
            json[jss::result][jss::ledger][jss::ledger_index] == ledgerID;
        if (!good || !checkDB)
            return good;

        auto const seq = json[jss::result][jss::ledger_index].asUInt();

        std::optional<LedgerInfo> oinfo =
            env.app().getRelationalDatabase().getLedgerInfoByIndex(seq);
        if (!oinfo)
            return false;
        const LedgerInfo& info = oinfo.value();

        const std::string outHash = to_string(info.hash);
        const LedgerIndex outSeq = info.seq;
        const std::string outParentHash = to_string(info.parentHash);
        const std::string outDrops = to_string(info.drops);
        const std::uint64_t outCloseTime =
            info.closeTime.time_since_epoch().count();
        const std::uint64_t outParentCloseTime =
            info.parentCloseTime.time_since_epoch().count();
        const std::uint64_t outCloseTimeResolution =
            info.closeTimeResolution.count();
        const std::uint64_t outCloseFlags = info.closeFlags;
        const std::string outAccountHash = to_string(info.accountHash);
        const std::string outTxHash = to_string(info.txHash);

        auto const& ledger = json[jss::result][jss::ledger];
        return outHash == ledger[jss::hash].asString() && outSeq == seq &&
            outParentHash == ledger[jss::parent_hash].asString() &&
            outDrops == ledger[jss::total_coins].asString() &&
            outCloseTime == ledger[jss::close_time].asUInt() &&
            outParentCloseTime == ledger[jss::parent_close_time].asUInt() &&
            outCloseTimeResolution ==
            ledger[jss::close_time_resolution].asUInt() &&
            outCloseFlags == ledger[jss::close_flags].asUInt() &&
            outAccountHash == ledger[jss::account_hash].asString() &&
            outTxHash == ledger[jss::transaction_hash].asString();
    }

    bool
    bad(Json::Value const& json, error_code_i error = rpcLGR_NOT_FOUND)
    {
        return json.isMember(jss::result) &&
            RPC::contains_error(json[jss::result]) &&
            json[jss::result][jss::error_code] == error;
    }

    std::string
    getHash(Json::Value const& json)
    {
        BEAST_EXPECT(
            json.isMember(jss::result) &&
            json[jss::result].isMember(jss::ledger) &&
            json[jss::result][jss::ledger].isMember(jss::hash) &&
            json[jss::result][jss::ledger][jss::hash].isString());
        return json[jss::result][jss::ledger][jss::hash].asString();
    }

    void
    ledgerCheck(jtx::Env& env, int const rows, int const first)
    {
        const auto [actualRows, actualFirst, actualLast] =
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase())
                ->getLedgerCountMinMax();

        BEAST_EXPECT(actualRows == rows);
        BEAST_EXPECT(actualFirst == first);
        BEAST_EXPECT(actualLast == first + rows - 1);
    }

    void
    transactionCheck(jtx::Env& env, int const rows)
    {
        BEAST_EXPECT(
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase())
                ->getTransactionCount() == rows);
    }

    void
    accountTransactionCheck(jtx::Env& env, int const rows)
    {
        BEAST_EXPECT(
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase())
                ->getAccountTransactionCount() == rows);
    }

    int
    waitForReady(jtx::Env& env)
    {
        using namespace std::chrono_literals;

        auto& store = env.app().getSHAMapStore();

        int ledgerSeq = 3;
        store.rendezvous();
        BEAST_EXPECT(!store.getLastRotated());

        env.close();
        store.rendezvous();

        auto ledger = env.rpc("ledger", "validated");
        BEAST_EXPECT(goodLedger(env, ledger, std::to_string(ledgerSeq++)));

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        return ledgerSeq;
    }

public:
    void
    testClear()
    {
        using namespace std::chrono_literals;

        testcase("clearPrior");
        using namespace jtx;

        Env env(*this, envconfig(onlineDelete));

        auto& store = env.app().getSHAMapStore();
        env.fund(XRP(10000), noripple("alice"));

        ledgerCheck(env, 1, 2);
        transactionCheck(env, 0);
        accountTransactionCheck(env, 0);

        std::map<std::uint32_t, Json::Value const> ledgers;

        auto ledgerTmp = env.rpc("ledger", "0");
        BEAST_EXPECT(bad(ledgerTmp));

        ledgers.emplace(std::make_pair(1, env.rpc("ledger", "1")));
        BEAST_EXPECT(goodLedger(env, ledgers[1], "1"));

        ledgers.emplace(std::make_pair(2, env.rpc("ledger", "2")));
        BEAST_EXPECT(goodLedger(env, ledgers[2], "2"));

        ledgerTmp = env.rpc("ledger", "current");
        BEAST_EXPECT(goodLedger(env, ledgerTmp, "3"));

        ledgerTmp = env.rpc("ledger", "4");
        BEAST_EXPECT(bad(ledgerTmp));

        ledgerTmp = env.rpc("ledger", "100");
        BEAST_EXPECT(bad(ledgerTmp));

        auto const firstSeq = waitForReady(env);
        auto lastRotated = firstSeq - 1;

        // Create ledgers 4-11 with transactions (8 ledgers)
        // These transactions will survive the first rotation but not the second
        for (auto i = firstSeq + 1; i < deleteInterval + firstSeq; ++i)
        {
            env.fund(XRP(10000), noripple("test" + std::to_string(i)));
            env.close();

            ledgerTmp = env.rpc("ledger", "current");
            BEAST_EXPECT(goodLedger(env, ledgerTmp, std::to_string(i)));
        }
        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        SQLiteDatabase* const db =
            dynamic_cast<SQLiteDatabase*>(&env.app().getRelationalDatabase());

        // Simple helper to show what's in the database
        auto showDBState = [&env, &db](
                               const std::string& when,
                               const std::string& expectation = "") {
            auto [ledgerCount, firstLedger, lastLedger] =
                db->getLedgerCountMinMax();
            auto txCount = db->getTransactionCount();
            auto accTxCount = db->getAccountTransactionCount();

            std::cout << "\n" << when << ":" << std::endl;
            std::cout << "  Ledgers: " << firstLedger << "-" << lastLedger
                      << " (keeping " << ledgerCount << " ledgers)"
                      << std::endl;
            std::cout << "  Transactions: " << txCount;
            std::cout << ", Account Transactions: " << accTxCount;
            if (!expectation.empty())
            {
                std::cout << " " << expectation;
            }
            std::cout << std::endl;

            // Also show RPC counts to see in-memory objects
            auto const result = env.rpc("get_counts")[jss::result];
            if (result.isMember("ripple::STTx") ||
                result.isMember("ripple::Transaction"))
            {
                std::cout << "  In-memory objects: "
                          << "STTx="
                          << (result.isMember("ripple::STTx")
                                  ? result["ripple::STTx"].asInt()
                                  : 0)
                          << ", Transaction="
                          << (result.isMember("ripple::Transaction")
                                  ? result["ripple::Transaction"].asInt()
                                  : 0)
                          << std::endl;
            }
        };

        showDBState("Initial state");
        BEAST_EXPECT(*db->getTransactionsMinLedgerSeq() == 3);

        for (auto i = 3; i < deleteInterval + lastRotated; ++i)
        {
            ledgers.emplace(
                std::make_pair(i, env.rpc("ledger", std::to_string(i))));
            BEAST_EXPECT(
                goodLedger(env, ledgers[i], std::to_string(i), true) &&
                getHash(ledgers[i]).length());
        }

        // After creating 8 more ledgers with transactions
        showDBState("Before first rotation");

        // Verify expected state
        ledgerCheck(env, deleteInterval + 1, 2);
        transactionCheck(env, deleteInterval);
        accountTransactionCheck(env, 2 * deleteInterval);

        // Additional verification
        BEAST_EXPECT(db->getTransactionCount() == deleteInterval);
        BEAST_EXPECT(db->getAccountTransactionCount() == 2 * deleteInterval);

        {
            // Closing one more ledger triggers a rotate
            env.close();

            auto ledger = env.rpc("ledger", "current");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(deleteInterval + 4)));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == deleteInterval + 3);
        lastRotated = store.getLastRotated();
        BEAST_EXPECT(lastRotated == 11);

        showDBState(
            "After FIRST rotation",
            "(EXPECTED: still have 8 txns from ledgers 3-11)");

        // First rotation: Deleted ledgers 1-2, kept 3-11
        // Transactions from ledgers 3-11 should STILL EXIST
        ledgerCheck(env, deleteInterval + 1, 3);
        transactionCheck(env, deleteInterval);  // Still have transactions!
        accountTransactionCheck(env, 2 * deleteInterval);

        // Additional verification
        BEAST_EXPECT(db->getTransactionCount() == deleteInterval);
        BEAST_EXPECT(db->getAccountTransactionCount() == 2 * deleteInterval);
        BEAST_EXPECT(*db->getTransactionsMinLedgerSeq() == 3);

        // The last iteration of this loop should trigger a rotate
        for (auto i = lastRotated - 1; i < lastRotated + deleteInterval - 1;
             ++i)
        {
            env.close();

            ledgerTmp = env.rpc("ledger", "current");
            BEAST_EXPECT(goodLedger(env, ledgerTmp, std::to_string(i + 3)));

            ledgers.emplace(
                std::make_pair(i, env.rpc("ledger", std::to_string(i))));
            BEAST_EXPECT(
                store.getLastRotated() == lastRotated ||
                i == lastRotated + deleteInterval - 2);
            BEAST_EXPECT(
                goodLedger(env, ledgers[i], std::to_string(i), true) &&
                getHash(ledgers[i]).length());
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == deleteInterval + lastRotated);

        showDBState(
            "After SECOND rotation",
            "(EXPECTED: 0 txns - ledgers 3-11 were deleted)");

        ledgerCheck(env, deleteInterval + 1, lastRotated);
        transactionCheck(env, 0);         // NOW transactions should be gone!
        accountTransactionCheck(env, 0);  // Account transactions too!

        // Additional verification - all transactions should be gone
        BEAST_EXPECT(db->getTransactionCount() == 0);
        BEAST_EXPECT(db->getAccountTransactionCount() == 0);
        BEAST_EXPECT(!db->getTransactionsMinLedgerSeq().has_value());
    }

    void
    testAutomatic()
    {
        testcase("automatic online_delete");
        using namespace jtx;
        using namespace std::chrono_literals;

        Env env(*this, envconfig(onlineDelete));
        auto& store = env.app().getSHAMapStore();

        auto ledgerSeq = waitForReady(env);
        auto lastRotated = ledgerSeq - 1;
        BEAST_EXPECT(store.getLastRotated() == lastRotated);
        BEAST_EXPECT(lastRotated != 2);

        // Because advisory_delete is unset,
        // "can_delete" is disabled.
        auto const canDelete = env.rpc("can_delete");
        BEAST_EXPECT(bad(canDelete, rpcNOT_ENABLED));

        // Close ledgers without triggering a rotate
        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        // The database will always have back to ledger 2,
        // regardless of lastRotated.
        ledgerCheck(env, ledgerSeq - 2, 2);
        BEAST_EXPECT(lastRotated == store.getLastRotated());

        {
            // Closing one more ledger triggers a rotate
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);
        BEAST_EXPECT(lastRotated != store.getLastRotated());

        lastRotated = store.getLastRotated();

        // Close enough ledgers to trigger another rotate
        for (; ledgerSeq < lastRotated + deleteInterval + 1; ++ledgerSeq)
        {
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        ledgerCheck(env, deleteInterval + 1, lastRotated);
        BEAST_EXPECT(lastRotated != store.getLastRotated());
    }

    void
    testCanDelete()
    {
        testcase("online_delete with advisory_delete");
        using namespace jtx;
        using namespace std::chrono_literals;

        // Same config with advisory_delete enabled
        Env env(*this, envconfig(advisoryDelete));
        auto& store = env.app().getSHAMapStore();

        auto ledgerSeq = waitForReady(env);
        auto lastRotated = ledgerSeq - 1;
        BEAST_EXPECT(store.getLastRotated() == lastRotated);
        BEAST_EXPECT(lastRotated != 2);

        auto canDelete = env.rpc("can_delete");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(canDelete[jss::result][jss::can_delete] == 0);

        canDelete = env.rpc("can_delete", "never");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(canDelete[jss::result][jss::can_delete] == 0);

        auto const firstBatch = deleteInterval + ledgerSeq;
        for (; ledgerSeq < firstBatch; ++ledgerSeq)
        {
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - 2, 2);
        BEAST_EXPECT(lastRotated == store.getLastRotated());

        // This does not kick off a cleanup
        canDelete = env.rpc(
            "can_delete", std::to_string(ledgerSeq + deleteInterval / 2));
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(
            canDelete[jss::result][jss::can_delete] ==
            ledgerSeq + deleteInterval / 2);

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - 2, 2);
        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off a cleanup, but it stays small.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;

        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            // No cleanups in this loop.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off another cleanup.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - firstBatch, firstBatch);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;

        // This does not kick off a cleanup
        canDelete = env.rpc("can_delete", "always");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(
            canDelete[jss::result][jss::can_delete] ==
            std::numeric_limits<unsigned int>::max());

        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            // No cleanups in this loop.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off another cleanup.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;

        // This does not kick off a cleanup
        canDelete = env.rpc("can_delete", "now");
        BEAST_EXPECT(!RPC::contains_error(canDelete[jss::result]));
        BEAST_EXPECT(canDelete[jss::result][jss::can_delete] == ledgerSeq - 1);

        for (; ledgerSeq < lastRotated + deleteInterval; ++ledgerSeq)
        {
            // No cleanups in this loop.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq), true));
        }

        store.rendezvous();

        BEAST_EXPECT(store.getLastRotated() == lastRotated);

        {
            // This kicks off another cleanup.
            env.close();

            auto ledger = env.rpc("ledger", "validated");
            BEAST_EXPECT(
                goodLedger(env, ledger, std::to_string(ledgerSeq++), true));
        }

        store.rendezvous();

        ledgerCheck(env, ledgerSeq - lastRotated, lastRotated);

        BEAST_EXPECT(store.getLastRotated() == ledgerSeq - 1);
        lastRotated = ledgerSeq - 1;
    }

    void
    run() override
    {
        testClear();
        testAutomatic();
        testCanDelete();
    }
};

// VFALCO This test fails because of thread asynchronous issues
BEAST_DEFINE_TESTSUITE(SHAMapStore, app, ripple);

}  // namespace test
}  // namespace ripple
