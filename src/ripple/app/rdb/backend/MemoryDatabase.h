#ifndef RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED
#define RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED

#include <ripple/app/ledger/AcceptedLedger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/rdb/RelationalDatabase.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <boost/unordered/concurrent_flat_map.hpp>
#include <algorithm>
#include <optional>

namespace ripple {

struct base_uint_hasher
{
    using result_type = std::size_t;

    result_type
    operator()(base_uint<256> const& value) const
    {
        return hardened_hash<>{}(value);
    }

    result_type
    operator()(AccountID const& value) const
    {
        return hardened_hash<>{}(value);
    }
};

class MemoryDatabase : public SQLiteDatabase
{
private:
    struct LedgerData
    {
        LedgerInfo info;
        boost::unordered::
            concurrent_flat_map<uint256, AccountTx, base_uint_hasher>
                transactions;
    };

    struct AccountTxData
    {
        boost::unordered::
            concurrent_flat_map<std::pair<uint32_t, uint32_t>, AccountTx>
                transactions;
    };

    Application& app_;

    boost::unordered::concurrent_flat_map<LedgerIndex, LedgerData> ledgers_;
    boost::unordered::
        concurrent_flat_map<uint256, LedgerIndex, base_uint_hasher>
            ledgerHashToSeq_;
    boost::unordered::concurrent_flat_map<uint256, AccountTx, base_uint_hasher>
        transactionMap_;
    boost::unordered::
        concurrent_flat_map<AccountID, AccountTxData, base_uint_hasher>
            accountTxMap_;

public:
    MemoryDatabase(Application& app, Config const&, JobQueue&) : app_(app)
    {
    }

    std::optional<LedgerIndex>
    getMinLedgerSeq() override
    {
        std::optional<LedgerIndex> minSeq;
        ledgers_.visit_all([&minSeq](auto const& pair) {
            if (!minSeq || pair.first < *minSeq)
            {
                minSeq = pair.first;
            }
        });
        return minSeq;
    }

    std::optional<LedgerIndex>
    getTransactionsMinLedgerSeq() override
    {
        std::optional<LedgerIndex> minSeq;
        transactionMap_.visit_all([&minSeq](auto const& pair) {
            LedgerIndex seq = pair.second.second->getLgrSeq();
            if (!minSeq || seq < *minSeq)
            {
                minSeq = seq;
            }
        });
        return minSeq;
    }

    std::optional<LedgerIndex>
    getAccountTransactionsMinLedgerSeq() override
    {
        std::optional<LedgerIndex> minSeq;
        accountTxMap_.visit_all([&minSeq](auto const& pair) {
            pair.second.transactions.visit_all([&minSeq](auto const& tx) {
                if (!minSeq || tx.first.first < *minSeq)
                {
                    minSeq = tx.first.first;
                }
            });
        });
        return minSeq;
    }

    std::optional<LedgerIndex>
    getMaxLedgerSeq() override
    {
        std::optional<LedgerIndex> maxSeq;
        ledgers_.visit_all([&maxSeq](auto const& pair) {
            if (!maxSeq || pair.first > *maxSeq)
            {
                maxSeq = pair.first;
            }
        });
        return maxSeq;
    }

    void
    deleteTransactionByLedgerSeq(LedgerIndex ledgerSeq) override
    {
        ledgers_.visit(ledgerSeq, [this](auto& item) {
            item.second.transactions.visit_all([this](auto const& txPair) {
                transactionMap_.erase(txPair.first);
            });
            item.second.transactions.clear();
        });

        accountTxMap_.visit_all([ledgerSeq](auto& item) {
            item.second.transactions.erase_if([ledgerSeq](auto const& tx) {
                return tx.first.first == ledgerSeq;
            });
        });
    }

    void
    deleteBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        ledgers_.erase_if([this, ledgerSeq](auto const& item) {
            if (item.first < ledgerSeq)
            {
                item.second.transactions.visit_all([this](auto const& txPair) {
                    transactionMap_.erase(txPair.first);
                });
                ledgerHashToSeq_.erase(item.second.info.hash);
                return true;
            }
            return false;
        });

        accountTxMap_.visit_all([ledgerSeq](auto& item) {
            item.second.transactions.erase_if([ledgerSeq](auto const& tx) {
                return tx.first.first < ledgerSeq;
            });
        });
    }

    void
    deleteTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        ledgers_.visit_all([this, ledgerSeq](auto& item) {
            if (item.first < ledgerSeq)
            {
                item.second.transactions.visit_all([this](auto const& txPair) {
                    transactionMap_.erase(txPair.first);
                });
                item.second.transactions.clear();
            }
        });

        accountTxMap_.visit_all([ledgerSeq](auto& item) {
            item.second.transactions.erase_if([ledgerSeq](auto const& tx) {
                return tx.first.first < ledgerSeq;
            });
        });
    }

    void
    deleteAccountTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        accountTxMap_.visit_all([ledgerSeq](auto& item) {
            item.second.transactions.erase_if([ledgerSeq](auto const& tx) {
                return tx.first.first < ledgerSeq;
            });
        });
    }

    std::size_t
    getTransactionCount() override
    {
        return transactionMap_.size();
    }

    std::size_t
    getAccountTransactionCount() override
    {
        std::size_t count = 0;
        accountTxMap_.visit_all([&count](auto const& item) {
            count += item.second.transactions.size();
        });
        return count;
    }

    CountMinMax
    getLedgerCountMinMax() override
    {
        CountMinMax result{0, 0, 0};
        ledgers_.visit_all([&result](auto const& item) {
            result.numberOfRows++;
            if (result.minLedgerSequence == 0 ||
                item.first < result.minLedgerSequence)
            {
                result.minLedgerSequence = item.first;
            }
            if (item.first > result.maxLedgerSequence)
            {
                result.maxLedgerSequence = item.first;
            }
        });
        return result;
    }

    bool
    saveValidatedLedger(
        std::shared_ptr<Ledger const> const& ledger,
        bool current) override
    {
        LedgerData ledgerData;
        ledgerData.info = ledger->info();

        auto aLedger = std::make_shared<AcceptedLedger>(ledger, app_);
        for (auto const& acceptedLedgerTx : *aLedger)
        {
            auto const& txn = acceptedLedgerTx->getTxn();
            auto const& meta = acceptedLedgerTx->getMeta();
            auto const& id = txn->getTransactionID();

            std::string reason;
            auto accTx = std::make_pair(
                std::make_shared<ripple::Transaction>(txn, reason, app_),
                std::make_shared<ripple::TxMeta>(meta));

            ledgerData.transactions.emplace(id, accTx);
            transactionMap_.emplace(id, accTx);

            for (auto const& account : meta.getAffectedAccounts())
            {
                accountTxMap_.visit(account, [&](auto& data) {
                    data.second.transactions.emplace(
                        std::make_pair(
                            ledger->info().seq, acceptedLedgerTx->getTxnSeq()),
                        accTx);
                });
            }
        }

        ledgers_.emplace(ledger->info().seq, std::move(ledgerData));
        ledgerHashToSeq_.emplace(ledger->info().hash, ledger->info().seq);

        // Only perform cleanup when saving the current ledger
        if (current)
        {
            // Calculate the cutoff sequence for old ledgers
            auto const cutoffSeq = ledger->info().seq > app_.config().LEDGER_HISTORY
                ? ledger->info().seq - app_.config().LEDGER_HISTORY
                : 0;

            // Amortized deletion of old ledgers and their associated data
            if (cutoffSeq > 0)
            {
                // We'll delete a batch of old ledgers at a time
                const std::size_t BATCH_SIZE = 128;
                std::size_t deleted = 0;

                ledgers_.erase_if([&](auto const& item) {
                    if (deleted >= BATCH_SIZE)
                        return false;

                    if (item.first < cutoffSeq)
                    {
                        // Delete associated transactions from the transaction map
                        item.second.transactions.visit_all(
                            [this](auto const& txPair) {
                                transactionMap_.erase(txPair.first);
                            });

                        // Remove the ledger hash mapping
                        ledgerHashToSeq_.erase(item.second.info.hash);

                        deleted++;
                        return true;
                    }
                    return false;
                });

                // Clean up associated account transactions
                if (deleted > 0)
                {
                    accountTxMap_.visit_all([cutoffSeq](auto& item) {
                        item.second.transactions.erase_if(
                            [cutoffSeq](auto const& tx) {
                                return tx.first.first < cutoffSeq;
                            });
                    });
                }

                // update ledger master
                app_.getLedgerMaster().clearPriorLedgers(cutoffSeq);
            }
        }

        return true;
    }


    std::optional<LedgerInfo>
    getLedgerInfoByIndex(LedgerIndex ledgerSeq) override
    {
        std::optional<LedgerInfo> result;
        ledgers_.visit(ledgerSeq, [&result](auto const& item) {
            result = item.second.info;
        });
        return result;
    }

    std::optional<LedgerInfo>
    getNewestLedgerInfo() override
    {
        std::optional<LedgerInfo> result;
        ledgers_.visit_all([&result](auto const& item) {
            if (!result || item.second.info.seq > result->seq)
            {
                result = item.second.info;
            }
        });
        return result;
    }

    std::optional<LedgerInfo>
    getLimitedOldestLedgerInfo(LedgerIndex ledgerFirstIndex) override
    {
        std::optional<LedgerInfo> result;
        ledgers_.visit_all([&](auto const& item) {
            if (item.first >= ledgerFirstIndex &&
                (!result || item.first < result->seq))
            {
                result = item.second.info;
            }
        });
        return result;
    }

    std::optional<LedgerInfo>
    getLimitedNewestLedgerInfo(LedgerIndex ledgerFirstIndex) override
    {
        std::optional<LedgerInfo> result;
        ledgers_.visit_all([&](auto const& item) {
            if (item.first >= ledgerFirstIndex &&
                (!result || item.first > result->seq))
            {
                result = item.second.info;
            }
        });
        return result;
    }

    std::optional<LedgerInfo>
    getLedgerInfoByHash(uint256 const& ledgerHash) override
    {
        std::optional<LedgerInfo> result;
        ledgerHashToSeq_.visit(ledgerHash, [this, &result](auto const& item) {
            ledgers_.visit(item.second, [&result](auto const& item) {
                result = item.second.info;
            });
        });
        return result;
    }

    uint256
    getHashByIndex(LedgerIndex ledgerIndex) override
    {
        uint256 result;
        ledgers_.visit(ledgerIndex, [&result](auto const& item) {
            result = item.second.info.hash;
        });
        return result;
    }

    std::optional<LedgerHashPair>
    getHashesByIndex(LedgerIndex ledgerIndex) override
    {
        std::optional<LedgerHashPair> result;
        ledgers_.visit(ledgerIndex, [&result](auto const& item) {
            result = LedgerHashPair{
                item.second.info.hash, item.second.info.parentHash};
        });
        return result;
    }

    std::map<LedgerIndex, LedgerHashPair>
    getHashesByIndex(LedgerIndex minSeq, LedgerIndex maxSeq) override
    {
        std::map<LedgerIndex, LedgerHashPair> result;
        ledgers_.visit_all([&](auto const& item) {
            if (item.first >= minSeq && item.first <= maxSeq)
            {
                result[item.first] = LedgerHashPair{
                    item.second.info.hash, item.second.info.parentHash};
            }
        });
        return result;
    }

    std::vector<std::shared_ptr<Transaction>>
    getTxHistory(LedgerIndex startIndex) override
    {
        std::vector<std::shared_ptr<Transaction>> result;
        transactionMap_.visit_all([&](auto const& item) {
            if (item.second.second->getLgrSeq() >= startIndex)
            {
                result.push_back(item.second.first);
            }
        });
        std::sort(
            result.begin(), result.end(), [](auto const& a, auto const& b) {
                return a->getLedger() > b->getLedger();
            });
        if (result.size() > 20)
        {
            result.resize(20);
        }
        return result;
    }

    AccountTxs
    getOldestAccountTxs(AccountTxOptions const& options) override
    {
        AccountTxs result;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    result.push_back(tx.second);
                }
            });
        });
        std::sort(
            result.begin(), result.end(), [](auto const& a, auto const& b) {
                return a.second->getLgrSeq() < b.second->getLgrSeq();
            });
        if (!options.bUnlimited && result.size() > options.limit)
        {
            result.resize(options.limit);
        }
        return result;
    }

    AccountTxs
    getNewestAccountTxs(AccountTxOptions const& options) override
    {
        AccountTxs result;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    result.push_back(tx.second);
                }
            });
        });
        std::sort(
            result.begin(), result.end(), [](auto const& a, auto const& b) {
                return a.second->getLgrSeq() > b.second->getLgrSeq();
            });
        if (!options.bUnlimited && result.size() > options.limit)
        {
            result.resize(options.limit);
        }
        return result;
    }

    MetaTxsList
    getOldestAccountTxsB(AccountTxOptions const& options) override
    {
        MetaTxsList result;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    result.emplace_back(
                        tx.second.first->getSTransaction()
                            ->getSerializer()
                            .peekData(),
                        tx.second.second->getAsObject()
                            .getSerializer()
                            .peekData(),
                        tx.first.first);
                }
            });
        });
        std::sort(
            result.begin(), result.end(), [](auto const& a, auto const& b) {
                return std::get<2>(a) < std::get<2>(b);
            });
        if (!options.bUnlimited && result.size() > options.limit)
        {
            result.resize(options.limit);
        }
        return result;
    }

    MetaTxsList
    getNewestAccountTxsB(AccountTxOptions const& options) override
    {
        MetaTxsList result;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    result.emplace_back(
                        tx.second.first->getSTransaction()
                            ->getSerializer()
                            .peekData(),
                        tx.second.second->getAsObject()
                            .getSerializer()
                            .peekData(),
                        tx.first.first);
                }
            });
        });
        std::sort(
            result.begin(), result.end(), [](auto const& a, auto const& b) {
                return std::get<2>(a) > std::get<2>(b);
            });
        if (!options.bUnlimited && result.size() > options.limit)
        {
            result.resize(options.limit);
        }
        return result;
    }

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    oldestAccountTxPage(AccountTxPageOptions const& options) override
    {
        AccountTxs result;
        std::optional<AccountTxMarker> marker;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, AccountTx>>
                txs;
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    txs.emplace_back(tx);
                }
            });
            std::sort(txs.begin(), txs.end(), [](auto const& a, auto const& b) {
                return a.first < b.first;
            });

            auto it = txs.begin();
            if (options.marker)
            {
                it = std::find_if(txs.begin(), txs.end(), [&](auto const& tx) {
                    return tx.first.first == options.marker->ledgerSeq &&
                        tx.first.second == options.marker->txnSeq;
                });
                if (it != txs.end())
                    ++it;
            }

            for (; it != txs.end() && result.size() < options.limit; ++it)
            {
                result.push_back(it->second);
            }

            if (it != txs.end())
            {
                marker = AccountTxMarker{it->first.first, it->first.second};
            }
        });
        return {result, marker};
    }

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    newestAccountTxPage(AccountTxPageOptions const& options) override
    {
        AccountTxs result;
        std::optional<AccountTxMarker> marker;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, AccountTx>>
                txs;
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    txs.emplace_back(tx);
                }
            });
            std::sort(txs.begin(), txs.end(), [](auto const& a, auto const& b) {
                return a.first > b.first;
            });

            auto it = txs.begin();
            if (options.marker)
            {
                it = std::find_if(txs.begin(), txs.end(), [&](auto const& tx) {
                    return tx.first.first == options.marker->ledgerSeq &&
                        tx.first.second == options.marker->txnSeq;
                });
                if (it != txs.end())
                    ++it;
            }

            for (; it != txs.end() && result.size() < options.limit; ++it)
            {
                result.push_back(it->second);
            }

            if (it != txs.end())
            {
                marker = AccountTxMarker{it->first.first, it->first.second};
            }
        });
        return {result, marker};
    }

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    oldestAccountTxPageB(AccountTxPageOptions const& options) override
    {
        MetaTxsList result;
        std::optional<AccountTxMarker> marker;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, AccountTx>>
                txs;
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    txs.emplace_back(tx);
                }
            });
            std::sort(txs.begin(), txs.end(), [](auto const& a, auto const& b) {
                return a.first < b.first;
            });

            auto it = txs.begin();
            if (options.marker)
            {
                it = std::find_if(txs.begin(), txs.end(), [&](auto const& tx) {
                    return tx.first.first == options.marker->ledgerSeq &&
                        tx.first.second == options.marker->txnSeq;
                });
                if (it != txs.end())
                    ++it;
            }

            for (; it != txs.end() && result.size() < options.limit; ++it)
            {
                result.emplace_back(
                    it->second.first->getSTransaction()
                        ->getSerializer()
                        .peekData(),
                    it->second.second->getAsObject().getSerializer().peekData(),
                    it->first.first);
            }

            if (it != txs.end())
            {
                marker = AccountTxMarker{it->first.first, it->first.second};
            }
        });
        return {result, marker};
    }

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    newestAccountTxPageB(AccountTxPageOptions const& options) override
    {
        MetaTxsList result;
        std::optional<AccountTxMarker> marker;
        accountTxMap_.visit(options.account, [&](auto const& item) {
            std::vector<std::pair<std::pair<uint32_t, uint32_t>, AccountTx>>
                txs;
            item.second.transactions.visit_all([&](auto const& tx) {
                if (tx.first.first >= options.minLedger &&
                    tx.first.first <= options.maxLedger)
                {
                    txs.emplace_back(tx);
                }
            });
            std::sort(txs.begin(), txs.end(), [](auto const& a, auto const& b) {
                return a.first > b.first;
            });

            auto it = txs.begin();
            if (options.marker)
            {
                it = std::find_if(txs.begin(), txs.end(), [&](auto const& tx) {
                    return tx.first.first == options.marker->ledgerSeq &&
                        tx.first.second == options.marker->txnSeq;
                });
                if (it != txs.end())
                    ++it;
            }

            for (; it != txs.end() && result.size() < options.limit; ++it)
            {
                result.emplace_back(
                    it->second.first->getSTransaction()
                        ->getSerializer()
                        .peekData(),
                    it->second.second->getAsObject().getSerializer().peekData(),
                    it->first.first);
            }

            if (it != txs.end())
            {
                marker = AccountTxMarker{it->first.first, it->first.second};
            }
        });
        return {result, marker};
    }

    std::variant<AccountTx, TxSearched>
    getTransaction(
        uint256 const& id,
        std::optional<ClosedInterval<std::uint32_t>> const& range,
        error_code_i& ec) override
    {
        std::variant<AccountTx, TxSearched> result = TxSearched::unknown;
        transactionMap_.visit(id, [&](auto const& item) {
            auto const& tx = item.second;
            if (!range ||
                (range->lower() <= tx.second->getLgrSeq() &&
                 tx.second->getLgrSeq() <= range->upper()))
            {
                result = tx;
            }
            else
            {
                result = TxSearched::all;
            }
        });
        return result;
    }

    bool
    ledgerDbHasSpace(Config const& config) override
    {
        // In-memory database always has space
        return true;
    }

    bool
    transactionDbHasSpace(Config const& config) override
    {
        // In-memory database always has space
        return true;
    }

    std::uint32_t
    getKBUsedAll() override
    {
        // Estimate memory usage (this is a rough estimate)
        std::uint32_t size = sizeof(*this);
        size += ledgers_.size() * (sizeof(LedgerIndex) + sizeof(LedgerData));
        size +=
            ledgerHashToSeq_.size() * (sizeof(uint256) + sizeof(LedgerIndex));
        size += transactionMap_.size() * (sizeof(uint256) + sizeof(AccountTx));
        accountTxMap_.visit_all([&size](auto const& item) {
            size += sizeof(AccountID) + sizeof(AccountTxData);
            size += item.second.transactions.size() *
                (sizeof(std::pair<uint32_t, uint32_t>) + sizeof(AccountTx));
        });
        return size / 1024;  // Convert to KB
    }

    std::uint32_t
    getKBUsedLedger() override
    {
        std::uint32_t size =
            ledgers_.size() * (sizeof(LedgerIndex) + sizeof(LedgerData));
        size +=
            ledgerHashToSeq_.size() * (sizeof(uint256) + sizeof(LedgerIndex));
        return size / 1024;  // Convert to KB
    }

    std::uint32_t
    getKBUsedTransaction() override
    {
        std::uint32_t size =
            transactionMap_.size() * (sizeof(uint256) + sizeof(AccountTx));
        accountTxMap_.visit_all([&size](auto const& item) {
            size += sizeof(AccountID) + sizeof(AccountTxData);
            size += item.second.transactions.size() *
                (sizeof(std::pair<uint32_t, uint32_t>) + sizeof(AccountTx));
        });
        return size / 1024;  // Convert to KB
    }

    void
    closeLedgerDB() override
    {
        // No-op for in-memory database
    }

    void
    closeTransactionDB() override
    {
        // No-op for in-memory database
    }
};

// Factory function
std::unique_ptr<SQLiteDatabase>
getMemoryDatabase(Application& app, Config const& config, JobQueue& jobQueue)
{
    return std::make_unique<MemoryDatabase>(app, config, jobQueue);
}
}  // namespace ripple
#endif
