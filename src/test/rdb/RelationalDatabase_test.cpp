//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <test/jtx.h>
#include <test/jtx/envconfig.h>
#include <xrpld/app/rdb/RelationalDatabase.h>
#include <xrpld/app/rdb/backend/SQLiteDatabase.h>
#include <xrpld/core/ConfigSections.h>
#include <boost/filesystem.hpp>
#include <chrono>

namespace ripple {
namespace test {

class RelationalDatabase_test : public beast::unit_test::suite
{
private:
    // Helper to get SQLiteDatabase* (works for both SQLite and RWDB since RWDB
    // inherits from SQLiteDatabase)
    static SQLiteDatabase*
    getInterface(Application& app)
    {
        return dynamic_cast<SQLiteDatabase*>(&app.getRelationalDatabase());
    }

    static SQLiteDatabase*
    getInterface(RelationalDatabase& db)
    {
        return dynamic_cast<SQLiteDatabase*>(&db);
    }

    static std::unique_ptr<Config>
    makeConfig(std::string const& backend)
    {
        auto config = test::jtx::envconfig();
        // Sqlite backend doesn't need a database_path as it will just use
        // in-memory databases when in standalone mode anyway.
        config->overwrite(SECTION_RELATIONAL_DB, "backend", backend);
        return config;
    }

public:
    RelationalDatabase_test() = default;

    void
    testBasicInitialization(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Basic initialization and empty database - " + backend);

        using namespace test::jtx;
        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        // Test empty database state
        BEAST_EXPECT(db.getMinLedgerSeq() == 2);
        BEAST_EXPECT(db.getMaxLedgerSeq() == 2);
        BEAST_EXPECT(db.getNewestLedgerInfo()->seq == 2);

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);

        if (sqliteDb)
        {
            BEAST_EXPECT(!sqliteDb->getTransactionsMinLedgerSeq().has_value());
            BEAST_EXPECT(
                !sqliteDb->getAccountTransactionsMinLedgerSeq().has_value());

            auto ledgerCount = sqliteDb->getLedgerCountMinMax();
            BEAST_EXPECT(ledgerCount.numberOfRows == 1);
            BEAST_EXPECT(ledgerCount.minLedgerSequence == 2);
            BEAST_EXPECT(ledgerCount.maxLedgerSequence == 2);
        }
    }

    void
    testLedgerSequenceOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Ledger sequence operations - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        // Create initial ledger
        Account alice("alice");
        env.fund(XRP(10000), alice);
        env.close();

        // Test basic sequence operations
        auto minSeq = db.getMinLedgerSeq();
        auto maxSeq = db.getMaxLedgerSeq();

        BEAST_EXPECT(minSeq.has_value());
        BEAST_EXPECT(maxSeq.has_value());
        BEAST_EXPECT(*minSeq == 2);
        BEAST_EXPECT(*maxSeq == 3);

        // Create more ledgers
        env(pay(alice, Account("bob"), XRP(1000)));
        env.close();

        env(pay(alice, Account("carol"), XRP(500)));
        env.close();

        // Verify sequence updates
        minSeq = db.getMinLedgerSeq();
        maxSeq = db.getMaxLedgerSeq();

        BEAST_EXPECT(*minSeq == 2);
        BEAST_EXPECT(*maxSeq == 5);

        auto* sqliteDb = getInterface(db);
        if (sqliteDb)
        {
            auto ledgerCount = sqliteDb->getLedgerCountMinMax();
            BEAST_EXPECT(ledgerCount.numberOfRows == 4);
            BEAST_EXPECT(ledgerCount.minLedgerSequence == 2);
            BEAST_EXPECT(ledgerCount.maxLedgerSequence == 5);
        }
    }

    void
    testLedgerInfoOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Ledger info retrieval operations - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto* db = getInterface(env.app());

        Account alice("alice");
        env.fund(XRP(10000), alice);
        env.close();

        // Test getNewestLedgerInfo
        auto newestLedger = db->getNewestLedgerInfo();
        BEAST_EXPECT(newestLedger.has_value());
        BEAST_EXPECT(newestLedger->seq == 3);

        // Test getLedgerInfoByIndex
        auto ledgerByIndex = db->getLedgerInfoByIndex(3);
        BEAST_EXPECT(ledgerByIndex.has_value());
        BEAST_EXPECT(ledgerByIndex->seq == 3);
        BEAST_EXPECT(ledgerByIndex->hash == newestLedger->hash);

        // Test getLedgerInfoByHash
        auto ledgerByHash = db->getLedgerInfoByHash(newestLedger->hash);
        BEAST_EXPECT(ledgerByHash.has_value());
        BEAST_EXPECT(ledgerByHash->seq == 3);
        BEAST_EXPECT(ledgerByHash->hash == newestLedger->hash);

        // Test getLimitedOldestLedgerInfo
        auto oldestLedger = db->getLimitedOldestLedgerInfo(2);
        BEAST_EXPECT(oldestLedger.has_value());
        BEAST_EXPECT(oldestLedger->seq == 2);

        // Test getLimitedNewestLedgerInfo
        auto limitedNewest = db->getLimitedNewestLedgerInfo(2);
        BEAST_EXPECT(limitedNewest.has_value());
        BEAST_EXPECT(limitedNewest->seq == 3);

        // Test invalid queries
        auto invalidLedger = db->getLedgerInfoByIndex(999);
        BEAST_EXPECT(!invalidLedger.has_value());

        uint256 invalidHash;
        auto invalidHashLedger = db->getLedgerInfoByHash(invalidHash);
        BEAST_EXPECT(!invalidHashLedger.has_value());
    }

    void
    testHashOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Hash retrieval operations - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        Account alice("alice");
        env.fund(XRP(10000), alice);
        env.close();

        env(pay(alice, Account("bob"), XRP(1000)));
        env.close();

        // Test getHashByIndex
        auto hash1 = db.getHashByIndex(3);
        auto hash2 = db.getHashByIndex(4);

        BEAST_EXPECT(hash1 != uint256());
        BEAST_EXPECT(hash2 != uint256());
        BEAST_EXPECT(hash1 != hash2);

        // Test getHashesByIndex (single)
        auto hashPair = db.getHashesByIndex(4);
        BEAST_EXPECT(hashPair.has_value());
        BEAST_EXPECT(hashPair->ledgerHash == hash2);
        BEAST_EXPECT(hashPair->parentHash == hash1);

        // Test getHashesByIndex (range)
        auto hashRange = db.getHashesByIndex(3, 4);
        BEAST_EXPECT(hashRange.size() == 2);
        BEAST_EXPECT(hashRange[3].ledgerHash == hash1);
        BEAST_EXPECT(hashRange[4].ledgerHash == hash2);
        BEAST_EXPECT(hashRange[4].parentHash == hash1);

        // Test invalid hash queries
        auto invalidHash = db.getHashByIndex(999);
        BEAST_EXPECT(invalidHash == uint256());

        auto invalidHashPair = db.getHashesByIndex(999);
        BEAST_EXPECT(!invalidHashPair.has_value());

        auto emptyRange = db.getHashesByIndex(10, 5);  // max < min
        BEAST_EXPECT(emptyRange.empty());
    }

    void
    testTransactionOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Transaction storage and retrieval - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        Account alice("alice");
        Account bob("bob");

        env.fund(XRP(10000), alice, bob);
        env.close();

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);

        if (!sqliteDb)
            return;

        // Test initial transaction counts after funding
        auto initialTxCount = sqliteDb->getTransactionCount();
        auto initialAcctTxCount = sqliteDb->getAccountTransactionCount();

        BEAST_EXPECT(initialTxCount == 4);
        BEAST_EXPECT(initialAcctTxCount == 6);

        // Create transactions
        env(pay(alice, bob, XRP(1000)));
        env.close();

        env(pay(bob, alice, XRP(500)));
        env.close();

        // Test transaction counts after creation
        auto txCount = sqliteDb->getTransactionCount();
        auto acctTxCount = sqliteDb->getAccountTransactionCount();

        BEAST_EXPECT(txCount == 6);
        BEAST_EXPECT(acctTxCount == 10);

        // Test transaction retrieval
        uint256 invalidTxId;
        error_code_i ec;
        auto invalidTxResult =
            sqliteDb->getTransaction(invalidTxId, std::nullopt, ec);
        BEAST_EXPECT(std::holds_alternative<TxSearched>(invalidTxResult));

        // Test transaction history
        auto txHistory = db.getTxHistory(0);

        BEAST_EXPECT(!txHistory.empty());
        BEAST_EXPECT(txHistory.size() == 6);

        // Test with valid transaction range
        auto minSeq = sqliteDb->getTransactionsMinLedgerSeq();
        auto maxSeq = db.getMaxLedgerSeq();

        if (minSeq && maxSeq)
        {
            ClosedInterval<std::uint32_t> range(*minSeq, *maxSeq);
            auto rangeResult = sqliteDb->getTransaction(invalidTxId, range, ec);
            auto searched = std::get<TxSearched>(rangeResult);
            BEAST_EXPECT(
                searched == TxSearched::all || searched == TxSearched::some);
        }
    }

    void
    testAccountTransactionOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Account transaction operations - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        Account alice("alice");
        Account bob("bob");
        Account carol("carol");

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);

        if (!sqliteDb)
            return;

        // Create multiple transactions involving alice
        env(pay(alice, bob, XRP(1000)));
        env.close();

        env(pay(bob, alice, XRP(500)));
        env.close();

        env(pay(alice, carol, XRP(250)));
        env.close();

        auto minSeq = db.getMinLedgerSeq();
        auto maxSeq = db.getMaxLedgerSeq();

        if (!minSeq || !maxSeq)
            return;

        // Test getOldestAccountTxs
        RelationalDatabase::AccountTxOptions options{
            alice.id(), *minSeq, *maxSeq, 0, 10, false};

        auto oldestTxs = sqliteDb->getOldestAccountTxs(options);
        BEAST_EXPECT(oldestTxs.size() == 5);

        // Test getNewestAccountTxs
        auto newestTxs = sqliteDb->getNewestAccountTxs(options);
        BEAST_EXPECT(newestTxs.size() == 5);

        // Test binary format versions
        auto oldestTxsB = sqliteDb->getOldestAccountTxsB(options);
        BEAST_EXPECT(oldestTxsB.size() == 5);

        auto newestTxsB = sqliteDb->getNewestAccountTxsB(options);
        BEAST_EXPECT(newestTxsB.size() == 5);

        // Test with limit
        options.limit = 1;
        auto limitedTxs = sqliteDb->getOldestAccountTxs(options);
        BEAST_EXPECT(limitedTxs.size() == 1);

        // Test with offset
        options.limit = 10;
        options.offset = 1;
        auto offsetTxs = sqliteDb->getOldestAccountTxs(options);
        BEAST_EXPECT(offsetTxs.size() == 4);

        // Test with invalid account
        {
            Account invalidAccount("invalid");
            RelationalDatabase::AccountTxOptions invalidOptions{
                invalidAccount.id(), *minSeq, *maxSeq, 0, 10, false};
            auto invalidAccountTxs =
                sqliteDb->getOldestAccountTxs(invalidOptions);
            BEAST_EXPECT(invalidAccountTxs.empty());
        }
    }

    void
    testAccountTransactionPaging(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Account transaction paging operations - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        Account alice("alice");
        Account bob("bob");

        env.fund(XRP(10000), alice, bob);
        env.close();

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);
        if (!sqliteDb)
            return;

        // Create multiple transactions for paging
        for (int i = 0; i < 5; ++i)
        {
            env(pay(alice, bob, XRP(100 + i)));
            env.close();
        }

        auto minSeq = db.getMinLedgerSeq();
        auto maxSeq = db.getMaxLedgerSeq();

        if (!minSeq || !maxSeq)
            return;

        RelationalDatabase::AccountTxPageOptions pageOptions{
            alice.id(), *minSeq, *maxSeq, std::nullopt, 2, false};

        // Test oldestAccountTxPage
        auto [oldestPage, oldestMarker] =
            sqliteDb->oldestAccountTxPage(pageOptions);

        BEAST_EXPECT(oldestPage.size() == 2);
        BEAST_EXPECT(oldestMarker.has_value() == true);

        // Test newestAccountTxPage
        auto [newestPage, newestMarker] =
            sqliteDb->newestAccountTxPage(pageOptions);

        BEAST_EXPECT(newestPage.size() == 2);
        BEAST_EXPECT(newestMarker.has_value() == true);

        // Test binary versions
        auto [oldestPageB, oldestMarkerB] =
            sqliteDb->oldestAccountTxPageB(pageOptions);
        BEAST_EXPECT(oldestPageB.size() == 2);

        auto [newestPageB, newestMarkerB] =
            sqliteDb->newestAccountTxPageB(pageOptions);
        BEAST_EXPECT(newestPageB.size() == 2);

        // Test with marker continuation
        if (oldestMarker.has_value())
        {
            pageOptions.marker = oldestMarker;
            auto [continuedPage, continuedMarker] =
                sqliteDb->oldestAccountTxPage(pageOptions);
            BEAST_EXPECT(continuedPage.size() == 2);
        }
    }

    void
    testDeletionOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Deletion operations - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        Account alice("alice");
        Account bob("bob");

        env.fund(XRP(10000), alice, bob);
        env.close();

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);
        if (!sqliteDb)
            return;

        // Create multiple ledgers and transactions
        for (int i = 0; i < 3; ++i)
        {
            env(pay(alice, bob, XRP(100 + i)));
            env.close();
        }

        auto initialTxCount = sqliteDb->getTransactionCount();
        BEAST_EXPECT(initialTxCount == 7);
        auto initialAcctTxCount = sqliteDb->getAccountTransactionCount();
        BEAST_EXPECT(initialAcctTxCount == 12);
        auto initialLedgerCount = sqliteDb->getLedgerCountMinMax();
        BEAST_EXPECT(initialLedgerCount.numberOfRows == 5);

        auto maxSeq = db.getMaxLedgerSeq();
        if (!maxSeq || *maxSeq <= 2)
            return;

        // Test deleteTransactionByLedgerSeq
        sqliteDb->deleteTransactionByLedgerSeq(*maxSeq);
        auto txCountAfterDelete = sqliteDb->getTransactionCount();
        BEAST_EXPECT(txCountAfterDelete == 6);

        // Test deleteTransactionsBeforeLedgerSeq
        sqliteDb->deleteTransactionsBeforeLedgerSeq(*maxSeq - 1);
        auto txCountAfterBulkDelete = sqliteDb->getTransactionCount();
        BEAST_EXPECT(txCountAfterBulkDelete == 1);

        // Test deleteAccountTransactionsBeforeLedgerSeq
        sqliteDb->deleteAccountTransactionsBeforeLedgerSeq(*maxSeq - 1);
        auto acctTxCountAfterDelete = sqliteDb->getAccountTransactionCount();
        BEAST_EXPECT(acctTxCountAfterDelete == 4);

        // Test deleteBeforeLedgerSeq
        auto minSeq = db.getMinLedgerSeq();
        if (minSeq)
        {
            sqliteDb->deleteBeforeLedgerSeq(*minSeq + 1);
            auto ledgerCountAfterDelete = sqliteDb->getLedgerCountMinMax();
            BEAST_EXPECT(ledgerCountAfterDelete.numberOfRows == 4);
        }
    }

    void
    testDatabaseSpaceOperations(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Database space and size operations - " + backend);

        using namespace test::jtx;
        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);
        if (!sqliteDb)
            return;

        // Test size queries
        auto allKB = sqliteDb->getKBUsedAll();
        auto ledgerKB = sqliteDb->getKBUsedLedger();
        auto txKB = sqliteDb->getKBUsedTransaction();

        if (backend == "rwdb")
        {
            // RWDB reports actual data memory (rounded down to KB)
            // Initially should be < 1KB, so rounds down to 0
            // Note: These are 0 due to rounding, not because there's literally
            // no data
            BEAST_EXPECT(allKB == 0);     // < 1024 bytes rounds to 0 KB
            BEAST_EXPECT(ledgerKB == 0);  // < 1024 bytes rounds to 0 KB
            BEAST_EXPECT(txKB == 0);      // < 1024 bytes rounds to 0 KB
        }
        else
        {
            // SQLite reports cache/engine memory which has overhead even when
            // empty Just verify the functions return reasonable values
            BEAST_EXPECT(allKB >= 0);
            BEAST_EXPECT(ledgerKB >= 0);
            BEAST_EXPECT(txKB >= 0);
        }

        // Create some data and verify size increases
        Account alice("alice");
        env.fund(XRP(10000), alice);
        env.close();

        auto newAllKB = sqliteDb->getKBUsedAll();
        auto newLedgerKB = sqliteDb->getKBUsedLedger();
        auto newTxKB = sqliteDb->getKBUsedTransaction();

        if (backend == "rwdb")
        {
            // RWDB reports actual data memory
            // After adding data, should see some increase
            BEAST_EXPECT(newAllKB >= 1);  // Should have at least 1KB total
            BEAST_EXPECT(
                newTxKB >= 0);  // Transactions added (might still be < 1KB)
            BEAST_EXPECT(
                newLedgerKB >= 0);  // Ledger data (might still be < 1KB)

            // Key relationships
            BEAST_EXPECT(newAllKB >= newLedgerKB + newTxKB);  // Total >= parts
            BEAST_EXPECT(newAllKB >= allKB);  // Should increase or stay same
            BEAST_EXPECT(newTxKB >= txKB);    // Should increase or stay same
        }
        else
        {
            // SQLite: Memory usage should not decrease after adding data
            // Values might increase due to cache growth
            BEAST_EXPECT(newAllKB >= allKB);
            BEAST_EXPECT(newLedgerKB >= ledgerKB);
            BEAST_EXPECT(newTxKB >= txKB);

            // SQLite's getKBUsedAll is global memory, should be >= parts
            BEAST_EXPECT(newAllKB >= newLedgerKB);
            BEAST_EXPECT(newAllKB >= newTxKB);
        }

        // Test space availability
        // Both SQLite and RWDB use in-memory databases in standalone mode,
        // so file-based space checks don't apply to either backend.
        // Skip these checks for both.

        // if (backend == "rwdb")
        // {
        //     BEAST_EXPECT(db.ledgerDbHasSpace(env.app().config()));
        //     BEAST_EXPECT(db.transactionDbHasSpace(env.app().config()));
        // }

        // Test database closure operations (should not throw)
        try
        {
            sqliteDb->closeLedgerDB();
            sqliteDb->closeTransactionDB();
        }
        catch (std::exception const& e)
        {
            BEAST_EXPECT(false);  // Should not throw
        }
    }

    void
    testTransactionMinLedgerSeq(
        std::string const& backend,
        std::unique_ptr<Config> config)
    {
        testcase("Transaction minimum ledger sequence tracking - " + backend);

        using namespace test::jtx;
        config->LEDGER_HISTORY = 1000;

        Env env(*this, std::move(config));
        auto& db = env.app().getRelationalDatabase();

        auto* sqliteDb = getInterface(db);
        BEAST_EXPECT(sqliteDb != nullptr);
        if (!sqliteDb)
            return;

        // Initially should have no transactions
        BEAST_EXPECT(!sqliteDb->getTransactionsMinLedgerSeq().has_value());
        BEAST_EXPECT(
            !sqliteDb->getAccountTransactionsMinLedgerSeq().has_value());

        Account alice("alice");
        Account bob("bob");

        env.fund(XRP(10000), alice, bob);
        env.close();

        // Create first transaction
        env(pay(alice, bob, XRP(1000)));
        env.close();

        auto txMinSeq = sqliteDb->getTransactionsMinLedgerSeq();
        auto acctTxMinSeq = sqliteDb->getAccountTransactionsMinLedgerSeq();
        BEAST_EXPECT(txMinSeq.has_value());
        BEAST_EXPECT(acctTxMinSeq.has_value());
        BEAST_EXPECT(*txMinSeq == 3);
        BEAST_EXPECT(*acctTxMinSeq == 3);

        // Create more transactions
        env(pay(bob, alice, XRP(500)));
        env.close();

        env(pay(alice, bob, XRP(250)));
        env.close();

        // Min sequences should remain the same (first transaction ledger)
        auto newTxMinSeq = sqliteDb->getTransactionsMinLedgerSeq();
        auto newAcctTxMinSeq = sqliteDb->getAccountTransactionsMinLedgerSeq();
        BEAST_EXPECT(newTxMinSeq == txMinSeq);
        BEAST_EXPECT(newAcctTxMinSeq == acctTxMinSeq);
    }

    std::vector<std::string> static getBackends(std::string const& unittest_arg)
    {
        // Valid backends
        static const std::set<std::string> validBackends = {"sqlite", "rwdb"};

        // Default to all valid backends if no arg specified
        if (unittest_arg.empty())
            return {validBackends.begin(), validBackends.end()};

        std::set<std::string> backends;  // Use set to avoid duplicates
        std::stringstream ss(unittest_arg);
        std::string backend;

        while (std::getline(ss, backend, ','))
        {
            if (!backend.empty())
            {
                // Validate backend
                if (validBackends.contains(backend))
                {
                    backends.insert(backend);
                }
            }
        }

        // Return as vector (sorted due to set)
        return {backends.begin(), backends.end()};
    }

    void
    run() override
    {
        auto backends = getBackends(arg());

        if (backends.empty())
        {
            fail("no valid backend specified: '" + arg() + "'");
        }

        for (auto const& backend : backends)
        {
            testBasicInitialization(backend, makeConfig(backend));
            testLedgerSequenceOperations(backend, makeConfig(backend));
            testLedgerInfoOperations(backend, makeConfig(backend));
            testHashOperations(backend, makeConfig(backend));
            testTransactionOperations(backend, makeConfig(backend));
            testAccountTransactionOperations(backend, makeConfig(backend));
            testAccountTransactionPaging(backend, makeConfig(backend));
            testDeletionOperations(backend, makeConfig(backend));
            testDatabaseSpaceOperations(backend, makeConfig(backend));
            testTransactionMinLedgerSeq(backend, makeConfig(backend));
        }
    }
};

BEAST_DEFINE_TESTSUITE(RelationalDatabase, rdb, ripple);

}  // namespace test
}  // namespace ripple
