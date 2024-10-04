//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2020 Ripple Labs Inc.

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

#include <ripple/app/ledger/AcceptedLedger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/Manifest.h>
#include <ripple/app/misc/impl/AccountTxPaging.h>
#include <ripple/app/rdb/backend/LMDBDatabase.h>
#include <ripple/app/rdb/backend/detail/Node.h>
#include <ripple/app/rdb/backend/detail/Shard.h>
#include <ripple/basics/BasicConfig.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/core/DatabaseCon.h>
#include <ripple/core/SociDB.h>
#include <ripple/json/to_string.h>
#include <ripple/nodestore/DatabaseShard.h>
// #include <soci/sqlite3/soci-sqlite3.h>

namespace ripple {

class LMDBDatabaseImp final : public LMDBDatabase
{
public:
    LMDBDatabaseImp(Application& app, Config const& config, JobQueue& jobQueue)
        : app_(app)
        , useTxTables_(config.useTxTables())
        , j_(app_.journal("LMDBDatabaseImp"))
    {
        DatabaseCon::Setup const setup = setup_DatabaseCon(config, j_);
        if (!makeLedgerDBs(
                config,
                setup,
                DatabaseCon::CheckpointerSetup{&jobQueue, &app_.logs()}))
        {
            std::string_view constexpr error =
                "Failed to create ledger databases";

            JLOG(j_.fatal()) << error;
            Throw<std::runtime_error>(error.data());
        }

        if (app.getShardStore() &&
            !makeMetaDBs(
                config,
                setup,
                DatabaseCon::CheckpointerSetup{&jobQueue, &app_.logs()}))
        {
            std::string_view constexpr error =
                "Failed to create metadata databases";

            JLOG(j_.fatal()) << error;
            Throw<std::runtime_error>(error.data());
        }
    }

    std::optional<LedgerIndex>
    getMinLedgerSeq() override;

    std::optional<LedgerIndex>
    getTransactionsMinLedgerSeq() override;

    std::optional<LedgerIndex>
    getAccountTransactionsMinLedgerSeq() override;

    std::optional<LedgerIndex>
    getMaxLedgerSeq() override;

    void
    deleteTransactionByLedgerSeq(LedgerIndex ledgerSeq) override;

    void
    deleteBeforeLedgerSeq(LedgerIndex ledgerSeq) override;

    void
    deleteTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override;

    void
    deleteAccountTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override;

    std::size_t
    getTransactionCount() override;

    std::size_t
    getAccountTransactionCount() override;

    RelationalDatabase::CountMinMax
    getLedgerCountMinMax() override;

    bool
    saveValidatedLedger(
        std::shared_ptr<Ledger const> const& ledger,
        bool current) override;

    std::optional<LedgerInfo>
    getLedgerInfoByIndex(LedgerIndex ledgerSeq) override;

    std::optional<LedgerInfo>
    getNewestLedgerInfo() override;

    std::optional<LedgerInfo>
    getLimitedOldestLedgerInfo(LedgerIndex ledgerFirstIndex) override;

    std::optional<LedgerInfo>
    getLimitedNewestLedgerInfo(LedgerIndex ledgerFirstIndex) override;

    std::optional<LedgerInfo>
    getLedgerInfoByHash(uint256 const& ledgerHash) override;

    uint256
    getHashByIndex(LedgerIndex ledgerIndex) override;

    std::optional<LedgerHashPair>
    getHashesByIndex(LedgerIndex ledgerIndex) override;

    std::map<LedgerIndex, LedgerHashPair>
    getHashesByIndex(LedgerIndex minSeq, LedgerIndex maxSeq) override;

    std::vector<std::shared_ptr<Transaction>>
    getTxHistory(LedgerIndex startIndex) override;

    AccountTxs
    getOldestAccountTxs(AccountTxOptions const& options) override;

    AccountTxs
    getNewestAccountTxs(AccountTxOptions const& options) override;

    MetaTxsList
    getOldestAccountTxsB(AccountTxOptions const& options) override;

    MetaTxsList
    getNewestAccountTxsB(AccountTxOptions const& options) override;

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    oldestAccountTxPage(AccountTxPageOptions const& options) override;

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    newestAccountTxPage(AccountTxPageOptions const& options) override;

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    oldestAccountTxPageB(AccountTxPageOptions const& options) override;

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    newestAccountTxPageB(AccountTxPageOptions const& options) override;

    std::variant<AccountTx, TxSearched>
    getTransaction(
        uint256 const& id,
        std::optional<ClosedInterval<std::uint32_t>> const& range,
        error_code_i& ec) override;

    bool
    ledgerDbHasSpace(Config const& config) override;

    bool
    transactionDbHasSpace(Config const& config) override;

    std::uint32_t
    getKBUsedAll() override;

    std::uint32_t
    getKBUsedLedger() override;

    std::uint32_t
    getKBUsedTransaction() override;

    void
    closeLedgerDB() override;

    void
    closeTransactionDB() override;

private:
    Application& app_;
    bool const useTxTables_;
    beast::Journal j_;
    std::unique_ptr<DatabaseCon> lgrdb_, txdb_;
    std::unique_ptr<DatabaseCon> lgrMetaDB_, txMetaDB_;

    /**
     * @brief makeLedgerDBs Opens ledger and transaction databases for the node
     *        store, and stores their descriptors in private member variables.
     * @param config Config object.
     * @param setup Path to the databases and other opening parameters.
     * @param checkpointerSetup Checkpointer parameters.
     * @return True if node databases opened successfully.
     */
    bool
    makeLedgerDBs(
        Config const& config,
        DatabaseCon::Setup const& setup,
        DatabaseCon::CheckpointerSetup const& checkpointerSetup);

    /**
     * @brief makeMetaDBs Opens shard index lookup databases, and stores
     *        their descriptors in private member variables.
     * @param config Config object.
     * @param setup Path to the databases and other opening parameters.
     * @param checkpointerSetup Checkpointer parameters.
     * @return True if node databases opened successfully.
     */
    bool
    makeMetaDBs(
        Config const& config,
        DatabaseCon::Setup const& setup,
        DatabaseCon::CheckpointerSetup const& checkpointerSetup);

    /**
     * @brief seqToShardIndex Provides the index of the shard that stores the
     *        ledger with the given sequence.
     * @param ledgerSeq Ledger sequence.
     * @return Shard index.
     */
    std::uint32_t
    seqToShardIndex(LedgerIndex ledgerSeq)
    {
        return app_.getShardStore()->seqToShardIndex(ledgerSeq);
    }

    /**
     * @brief firstLedgerSeq Returns the sequence of the first ledger stored in
     *        the shard specified by the shard index parameter.
     * @param shardIndex Shard Index.
     * @return First ledger sequence.
     */
    LedgerIndex
    firstLedgerSeq(std::uint32_t shardIndex)
    {
        return app_.getShardStore()->firstLedgerSeq(shardIndex);
    }

    /**
     * @brief lastLedgerSeq Returns the sequence of the last ledger stored in
     *        the shard specified by the shard index parameter.
     * @param shardIndex Shard Index.
     * @return Last ledger sequence.
     */
    LedgerIndex
    lastLedgerSeq(std::uint32_t shardIndex)
    {
        return app_.getShardStore()->lastLedgerSeq(shardIndex);
    }

    /**
     * @brief existsLedger Checks if the node store ledger database exists.
     * @return True if the node store ledger database exists.
     */
    bool
    existsLedger()
    {
        return static_cast<bool>(lgrdb_);
    }

    /**
     * @brief existsTransaction Checks if the node store transaction database
     *        exists.
     * @return True if the node store transaction database exists.
     */
    bool
    existsTransaction()
    {
        return static_cast<bool>(txdb_);
    }

    /**
     * shardStoreExists Checks whether the shard store exists
     * @return True if the shard store exists
     */
    bool
    shardStoreExists()
    {
        return app_.getShardStore() != nullptr;
    }

    /**
     * @brief checkoutLedger Checks out and returns node store ledger
     *        database.
     * @return Session to the node store ledger database.
     */
    auto
    checkoutLedger()
    {
        return lgrdb_->checkoutLMDB();
    }

    /**
     * @brief checkoutTransaction Checks out and returns the node store
     *        transaction database.
     * @return Session to the node store transaction database.
     */
    auto
    checkoutTransaction()
    {
        return txdb_->checkoutLMDB();
    }
};

bool
LMDBDatabaseImp::makeLedgerDBs(
    Config const& config,
    DatabaseCon::Setup const& setup,
    DatabaseCon::CheckpointerSetup const& checkpointerSetup)
{
    auto [lgr, tx, res] =
        detail::makeLedgerDBs(config, setup, checkpointerSetup);
    txdb_ = std::move(tx);
    lgrdb_ = std::move(lgr);
    return res;
}

bool
LMDBDatabaseImp::makeMetaDBs(
    Config const& config,
    DatabaseCon::Setup const& setup,
    DatabaseCon::CheckpointerSetup const& checkpointerSetup)
{
    auto [lgrMetaDB, txMetaDB] =
        detail::makeMetaDBs(config, setup, checkpointerSetup);

    txMetaDB_ = std::move(txMetaDB);
    lgrMetaDB_ = std::move(lgrMetaDB);

    return true;
}

std::optional<LedgerIndex>
LMDBDatabaseImp::getMinLedgerSeq()
{
    /* if databases exists, use it */
    if (existsLedger())
    {
        auto db = checkoutLedger();
        return detail::getMinLedgerSeq(db.get(), detail::TableType::Ledgers);
    }

    /* else return empty value */
    return {};
}

std::optional<LedgerIndex>
LMDBDatabaseImp::getTransactionsMinLedgerSeq()
{
    if (!useTxTables_)
        return {};

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        return detail::getMinLedgerSeq(
            db.get(), detail::TableType::Transactions);
    }

    return {};
}

std::optional<LedgerIndex>
LMDBDatabaseImp::getAccountTransactionsMinLedgerSeq()
{
    if (!useTxTables_)
        return {};

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        return detail::getMinLedgerSeq(
            db.get(), detail::TableType::AccountTransactions);
    }

    return {};
}

std::optional<LedgerIndex>
LMDBDatabaseImp::getMaxLedgerSeq()
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        return detail::getMaxLedgerSeq(db.get(), detail::TableType::Ledgers);
    }

    return {};
}

void
LMDBDatabaseImp::deleteTransactionByLedgerSeq(LedgerIndex ledgerSeq)
{
    if (!useTxTables_)
        return;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        detail::deleteByLedgerSeq(
            db.get(), detail::TableType::Transactions, ledgerSeq);
        return;
    }
}

void
LMDBDatabaseImp::deleteBeforeLedgerSeq(LedgerIndex ledgerSeq)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        detail::deleteBeforeLedgerSeq(
            db.get(), detail::TableType::Ledgers, ledgerSeq);
        return;
    }
}

void
LMDBDatabaseImp::deleteTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq)
{
    if (!useTxTables_)
        return;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        detail::deleteBeforeLedgerSeq(
            db.get(), detail::TableType::Transactions, ledgerSeq);
        return;
    }
}

void
LMDBDatabaseImp::deleteAccountTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq)
{
    if (!useTxTables_)
        return;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        detail::deleteBeforeLedgerSeq(
            db.get(), detail::TableType::AccountTransactions, ledgerSeq);
        return;
    }
}

std::size_t
LMDBDatabaseImp::getTransactionCount()
{
    if (!useTxTables_)
        return 0;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        return detail::getRows(db.get(), detail::TableType::Transactions);
    }

    return 0;
}

std::size_t
LMDBDatabaseImp::getAccountTransactionCount()
{
    if (!useTxTables_)
        return 0;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        return detail::getRows(
            db.get(), detail::TableType::AccountTransactions);
    }

    return 0;
}

RelationalDatabase::CountMinMax
LMDBDatabaseImp::getLedgerCountMinMax()
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        return detail::getRowsMinMax(db.get(), detail::TableType::Ledgers);
    }

    return {0, 0, 0};
}

bool
LMDBDatabaseImp::saveValidatedLedger(
    std::shared_ptr<Ledger const> const& ledger,
    bool current)
{
    if (existsLedger())
    {
        if (!detail::saveValidatedLedger(
                *lgrdb_, *txdb_, app_, ledger, current))
            return false;
    }

    return true;
}

std::optional<LedgerInfo>
LMDBDatabaseImp::getLedgerInfoByIndex(LedgerIndex ledgerSeq)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        auto const res = detail::getLedgerInfoByIndex(db.get(), ledgerSeq, j_);

        if (res.has_value())
            return res;
    }

    return {};
}

std::optional<LedgerInfo>
LMDBDatabaseImp::getNewestLedgerInfo()
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        auto const res = detail::getNewestLedgerInfo(db.get(), j_);

        if (res.has_value())
            return res;
    }

    return {};
}

std::optional<LedgerInfo>
LMDBDatabaseImp::getLimitedOldestLedgerInfo(LedgerIndex ledgerFirstIndex)
{
    return {};
}

std::optional<LedgerInfo>
LMDBDatabaseImp::getLimitedNewestLedgerInfo(LedgerIndex ledgerFirstIndex)
{
    return {};
}

std::optional<LedgerInfo>
LMDBDatabaseImp::getLedgerInfoByHash(uint256 const& ledgerHash)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        auto const res = detail::getLedgerInfoByHash(db.get(), ledgerHash, j_);

        if (res.has_value())
            return res;
    }

    return {};
}

uint256
LMDBDatabaseImp::getHashByIndex(LedgerIndex ledgerIndex)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        auto const res = detail::getHashByIndex(db.get(), ledgerIndex);

        if (res.isNonZero())
            return res;
    }

    return uint256();
}

std::optional<LedgerHashPair>
LMDBDatabaseImp::getHashesByIndex(LedgerIndex ledgerIndex)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        auto const res = detail::getHashesByIndex(db.get(), ledgerIndex, j_);

        if (res.has_value())
            return res;
    }

    return {};
}

std::map<LedgerIndex, LedgerHashPair>
LMDBDatabaseImp::getHashesByIndex(LedgerIndex minSeq, LedgerIndex maxSeq)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        auto const res = detail::getHashesByIndex(db.get(), minSeq, maxSeq, j_);

        if (!res.empty())
            return res;
    }

    return {};
}

std::vector<std::shared_ptr<Transaction>>
LMDBDatabaseImp::getTxHistory(LedgerIndex startIndex)
{
    return {};
}

RelationalDatabase::AccountTxs
LMDBDatabaseImp::getOldestAccountTxs(AccountTxOptions const& options)
{
    return {};
}

RelationalDatabase::AccountTxs
LMDBDatabaseImp::getNewestAccountTxs(AccountTxOptions const& options)
{
    return {};
}

RelationalDatabase::MetaTxsList
LMDBDatabaseImp::getOldestAccountTxsB(AccountTxOptions const& options)
{
    return {};
}

RelationalDatabase::MetaTxsList
LMDBDatabaseImp::getNewestAccountTxsB(AccountTxOptions const& options)
{
    return {};
}

std::pair<
    RelationalDatabase::AccountTxs,
    std::optional<RelationalDatabase::AccountTxMarker>>
LMDBDatabaseImp::oldestAccountTxPage(AccountTxPageOptions const& options)
{
    return {};
}

std::pair<
    RelationalDatabase::AccountTxs,
    std::optional<RelationalDatabase::AccountTxMarker>>
LMDBDatabaseImp::newestAccountTxPage(AccountTxPageOptions const& options)
{
    return {};
}

std::pair<
    RelationalDatabase::MetaTxsList,
    std::optional<RelationalDatabase::AccountTxMarker>>
LMDBDatabaseImp::oldestAccountTxPageB(AccountTxPageOptions const& options)
{
    return {};
}

std::pair<
    RelationalDatabase::MetaTxsList,
    std::optional<RelationalDatabase::AccountTxMarker>>
LMDBDatabaseImp::newestAccountTxPageB(AccountTxPageOptions const& options)
{
    return {};
}

std::variant<RelationalDatabase::AccountTx, TxSearched>
LMDBDatabaseImp::getTransaction(
    uint256 const& id,
    std::optional<ClosedInterval<std::uint32_t>> const& range,
    error_code_i& ec)
{
    if (!useTxTables_)
        return TxSearched::unknown;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        return detail::getTransaction(db.get(), app_, id, range, ec);
    }

    return TxSearched::unknown;
}

bool
LMDBDatabaseImp::ledgerDbHasSpace(Config const& config)
{
    if (existsLedger())
    {
        auto db = checkoutLedger();
        return detail::dbHasSpace(db.get(), config, j_);
    }

    return true;
}

bool
LMDBDatabaseImp::transactionDbHasSpace(Config const& config)
{
    if (!useTxTables_)
        return true;

    if (existsTransaction())
    {
        auto db = checkoutTransaction();
        return detail::dbHasSpace(db.get(), config, j_);
    }

    return true;
}

std::uint32_t
LMDBDatabaseImp::getKBUsedAll()
{
    // if (existsLedger())
    // {
    //     return ripple::getKBUsedAll(lgrdb_->getLMDB());
    // }

    return 0;
}

std::uint32_t
LMDBDatabaseImp::getKBUsedLedger()
{
    // if (existsLedger())
    // {
    //     return ripple::getKBUsedDB(lgrdb_->getLMDB());
    // }

    return 0;
}

std::uint32_t
LMDBDatabaseImp::getKBUsedTransaction()
{
    if (!useTxTables_)
        return 0;

    // if (existsTransaction())
    // {
    //     return ripple::getKBUsedDB(txdb_->getLMDB());
    // }

    return 0;
}

void
LMDBDatabaseImp::closeLedgerDB()
{
    lgrdb_.reset();
}

void
LMDBDatabaseImp::closeTransactionDB()
{
    txdb_.reset();
}

std::unique_ptr<RelationalDatabase>
getLMDBDatabase(Application& app, Config const& config, JobQueue& jobQueue)
{
    return std::make_unique<LMDBDatabaseImp>(app, config, jobQueue);
}

}  // namespace ripple
