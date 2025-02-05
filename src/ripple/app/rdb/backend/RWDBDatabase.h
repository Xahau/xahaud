#ifndef RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED
#define RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED

#include <ripple/app/ledger/AcceptedLedger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/ledger/LedgerToJson.h>
#include <ripple/app/ledger/PendingSaves.h>
#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/misc/impl/AccountTxPaging.h>
#include <ripple/app/rdb/backend/SQLiteDatabase.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <vector>

namespace ripple {

class RWDBDatabase : public SQLiteDatabase
{
private:
    struct LedgerData
    {
        LedgerInfo info;
        std::map<uint256, AccountTx> transactions;
    };

    struct AccountTxData
    {
        AccountTxs transactions;
        std::map<uint32_t, std::map<uint32_t, size_t>>
            ledgerTxMap;  // ledgerSeq -> txSeq -> index in transactions
    };

    Application& app_;
    bool const useTxTables_;

    mutable std::shared_mutex mutex_;

    std::map<LedgerIndex, LedgerData> ledgers_;
    std::map<uint256, LedgerIndex> ledgerHashToSeq_;
    std::map<uint256, AccountTx> transactionMap_;
    std::map<AccountID, AccountTxData> accountTxMap_;

public:
    RWDBDatabase(Application& app, Config const& config, JobQueue& jobQueue)
        : app_(app), useTxTables_(config.useTxTables())
    {
    }

    std::optional<LedgerIndex>
    getMinLedgerSeq() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return std::nullopt;
        return ledgers_.begin()->first;
    }

    std::optional<LedgerIndex>
    getTransactionsMinLedgerSeq() override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (transactionMap_.empty())
            return std::nullopt;
        return transactionMap_.begin()->second.second->getLgrSeq();
    }

    std::optional<LedgerIndex>
    getAccountTransactionsMinLedgerSeq() override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (accountTxMap_.empty())
            return std::nullopt;
        LedgerIndex minSeq = std::numeric_limits<LedgerIndex>::max();
        for (const auto& [_, accountData] : accountTxMap_)
        {
            if (!accountData.ledgerTxMap.empty())
                minSeq =
                    std::min(minSeq, accountData.ledgerTxMap.begin()->first);
        }
        return minSeq == std::numeric_limits<LedgerIndex>::max()
            ? std::nullopt
            : std::optional<LedgerIndex>(minSeq);
    }

    std::optional<LedgerIndex>
    getMaxLedgerSeq() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return std::nullopt;
        return ledgers_.rbegin()->first;
    }
    void
    deleteTransactionByLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (!useTxTables_)
            return;

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.find(ledgerSeq);
        if (it != ledgers_.end())
        {
            for (const auto& [txHash, _] : it->second.transactions)
            {
                transactionMap_.erase(txHash);
            }
            it->second.transactions.clear();
        }
    }

    void
    deleteBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.begin();
        while (it != ledgers_.end() && it->first < ledgerSeq)
        {
            ledgerHashToSeq_.erase(it->second.info.hash);
            it = ledgers_.erase(it);
        }
    }

    void
    deleteTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (!useTxTables_)
            return;

        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.begin();
        while (it != ledgers_.end() && it->first < ledgerSeq)
        {
            for (const auto& [txHash, _] : it->second.transactions)
            {
                transactionMap_.erase(txHash);
            }
            it->second.transactions.clear();
            ++it;
        }
    }

    void
    deleteAccountTransactionsBeforeLedgerSeq(LedgerIndex ledgerSeq) override
    {
        if (!useTxTables_)
            return;

        std::unique_lock<std::shared_mutex> lock(mutex_);
        for (auto& [_, accountData] : accountTxMap_)
        {
            auto txIt = accountData.ledgerTxMap.begin();
            while (txIt != accountData.ledgerTxMap.end() &&
                   txIt->first < ledgerSeq)
            {
                txIt = accountData.ledgerTxMap.erase(txIt);
            }
            accountData.transactions.erase(
                std::remove_if(
                    accountData.transactions.begin(),
                    accountData.transactions.end(),
                    [ledgerSeq](const AccountTx& tx) {
                        return tx.second->getLgrSeq() < ledgerSeq;
                    }),
                accountData.transactions.end());
        }
    }
    std::size_t
    getTransactionCount() override
    {
        if (!useTxTables_)
            return 0;

        std::shared_lock<std::shared_mutex> lock(mutex_);
        return transactionMap_.size();
    }

    std::size_t
    getAccountTransactionCount() override
    {
        if (!useTxTables_)
            return 0;

        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::size_t count = 0;
        for (const auto& [_, accountData] : accountTxMap_)
        {
            count += accountData.transactions.size();
        }
        return count;
    }

    CountMinMax
    getLedgerCountMinMax() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return {0, 0, 0};
        return {
            ledgers_.size(), ledgers_.begin()->first, ledgers_.rbegin()->first};
    }

    bool
    saveValidatedLedger(
        std::shared_ptr<Ledger const> const& ledger,
        bool current) override
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        LedgerData ledgerData;
        ledgerData.info = ledger->info();
        auto j = app_.journal("Ledger");
        auto seq = ledger->info().seq;

        JLOG(j.trace()) << "saveValidatedLedger "
                        << (current ? "" : "fromAcquire ") << seq;

        if (!ledger->info().accountHash.isNonZero())
        {
            JLOG(j.fatal()) << "AH is zero: " << getJson({*ledger, {}});
            assert(false);
        }

        if (ledger->info().accountHash !=
            ledger->stateMap().getHash().as_uint256())
        {
            JLOG(j.fatal()) << "sAL: " << ledger->info().accountHash
                            << " != " << ledger->stateMap().getHash();
            JLOG(j.fatal())
                << "saveAcceptedLedger: seq=" << seq << ", current=" << current;
            assert(false);
        }

        assert(ledger->info().txHash == ledger->txMap().getHash().as_uint256());

        // Save the ledger header in the hashed object store
        {
            Serializer s(128);
            s.add32(HashPrefix::ledgerMaster);
            addRaw(ledger->info(), s);
            app_.getNodeStore().store(
                hotLEDGER, std::move(s.modData()), ledger->info().hash, seq);
        }

        std::shared_ptr<AcceptedLedger> aLedger;
        try
        {
            aLedger = app_.getAcceptedLedgerCache().fetch(ledger->info().hash);
            if (!aLedger)
            {
                aLedger = std::make_shared<AcceptedLedger>(ledger, app_);
                app_.getAcceptedLedgerCache().canonicalize_replace_client(
                    ledger->info().hash, aLedger);
            }
        }
        catch (std::exception const&)
        {
            JLOG(j.warn()) << "An accepted ledger was missing nodes";
            app_.getLedgerMaster().failedSave(seq, ledger->info().hash);
            // Clients can now trust the database for information about this
            // ledger sequence.
            app_.pendingSaves().finishWork(seq);
            return false;
        }

        // Overwrite Current Ledger Transactions
        if (useTxTables_)
        {
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
                    if (accountTxMap_.find(account) == accountTxMap_.end())
                        accountTxMap_[account] = AccountTxData();

                    auto& accountData = accountTxMap_[account];
                    accountData.transactions.push_back(accTx);
                    accountData
                        .ledgerTxMap[seq][acceptedLedgerTx->getTxnSeq()] =
                        accountData.transactions.size() - 1;
                }

                app_.getMasterTransaction().inLedger(
                    id,
                    seq,
                    acceptedLedgerTx->getTxnSeq(),
                    app_.config().NETWORK_ID);
            }
        }

        // Overwrite Current Ledger
        ledgers_[seq] = std::move(ledgerData);
        ledgerHashToSeq_[ledger->info().hash] = seq;
        return true;
    }

    std::optional<LedgerInfo>
    getLedgerInfoByIndex(LedgerIndex ledgerSeq) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.find(ledgerSeq);
        if (it != ledgers_.end())
            return it->second.info;
        return std::nullopt;
    }

    std::optional<LedgerInfo>
    getNewestLedgerInfo() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return std::nullopt;
        return ledgers_.rbegin()->second.info;
    }

    std::optional<LedgerInfo>
    getLimitedOldestLedgerInfo(LedgerIndex ledgerFirstIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.lower_bound(ledgerFirstIndex);
        if (it != ledgers_.end())
            return it->second.info;
        return std::nullopt;
    }

    std::optional<LedgerInfo>
    getLimitedNewestLedgerInfo(LedgerIndex ledgerFirstIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.lower_bound(ledgerFirstIndex);
        if (it == ledgers_.end())
            return std::nullopt;
        return ledgers_.rbegin()->second.info;
    }

    std::optional<LedgerInfo>
    getLedgerInfoByHash(uint256 const& ledgerHash) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgerHashToSeq_.find(ledgerHash);
        if (it != ledgerHashToSeq_.end())
            return ledgers_.at(it->second).info;
        return std::nullopt;
    }
    uint256
    getHashByIndex(LedgerIndex ledgerIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.find(ledgerIndex);
        if (it != ledgers_.end())
            return it->second.info.hash;
        return uint256();
    }

    std::optional<LedgerHashPair>
    getHashesByIndex(LedgerIndex ledgerIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.find(ledgerIndex);
        if (it != ledgers_.end())
        {
            return LedgerHashPair{
                it->second.info.hash, it->second.info.parentHash};
        }
        return std::nullopt;
    }

    std::map<LedgerIndex, LedgerHashPair>
    getHashesByIndex(LedgerIndex minSeq, LedgerIndex maxSeq) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::map<LedgerIndex, LedgerHashPair> result;
        auto it = ledgers_.lower_bound(minSeq);
        auto end = ledgers_.upper_bound(maxSeq);
        for (; it != end; ++it)
        {
            result[it->first] = LedgerHashPair{
                it->second.info.hash, it->second.info.parentHash};
        }
        return result;
    }

    std::variant<AccountTx, TxSearched>
    getTransaction(
        uint256 const& id,
        std::optional<ClosedInterval<std::uint32_t>> const& range,
        error_code_i& ec) override
    {
        if (!useTxTables_)
            return TxSearched::unknown;

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = transactionMap_.find(id);
        if (it != transactionMap_.end())
        {
            const auto& [txn, txMeta] = it->second;
            std::uint32_t inLedger =
                rangeCheckedCast<std::uint32_t>(txMeta->getLgrSeq());
            it->second.first->setStatus(COMMITTED);
            it->second.first->setLedger(inLedger);
            return it->second;
        }

        if (range)
        {
            std::size_t count = 0;
            for (LedgerIndex seq = range->first(); seq <= range->last(); ++seq)
            {
                if (ledgers_.find(seq) != ledgers_.end())
                {
                    if (ledgers_[seq].transactions.size() > 0)
                        ++count;
                }
            }
            return (count == (range->last() - range->first() + 1))
                ? TxSearched::all
                : TxSearched::some;
        }

        return TxSearched::unknown;
    }

    bool
    ledgerDbHasSpace(Config const& config) override
    {
        return true;  // In-memory database always has space
    }

    bool
    transactionDbHasSpace(Config const& config) override
    {
        return true;  // In-memory database always has space
    }

    std::uint32_t
    getKBUsedAll() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::uint32_t size = sizeof(*this);
        size += ledgers_.size() * (sizeof(LedgerIndex) + sizeof(LedgerData));
        size +=
            ledgerHashToSeq_.size() * (sizeof(uint256) + sizeof(LedgerIndex));
        size += transactionMap_.size() * (sizeof(uint256) + sizeof(AccountTx));
        for (const auto& [_, accountData] : accountTxMap_)
        {
            size += sizeof(AccountID) + sizeof(AccountTxData);
            size += accountData.transactions.size() * sizeof(AccountTx);
            for (const auto& [_, innerMap] : accountData.ledgerTxMap)
            {
                size += sizeof(uint32_t) +
                    innerMap.size() * (sizeof(uint32_t) + sizeof(size_t));
            }
        }
        return size / 1024;
    }

    std::uint32_t
    getKBUsedLedger() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::uint32_t size = 0;
        size += ledgers_.size() * (sizeof(LedgerIndex) + sizeof(LedgerData));
        size +=
            ledgerHashToSeq_.size() * (sizeof(uint256) + sizeof(LedgerIndex));
        return size / 1024;
    }

    std::uint32_t
    getKBUsedTransaction() override
    {
        if (!useTxTables_)
            return 0;

        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::uint32_t size = 0;
        size += transactionMap_.size() * (sizeof(uint256) + sizeof(AccountTx));
        for (const auto& [_, accountData] : accountTxMap_)
        {
            size += sizeof(AccountID) + sizeof(AccountTxData);
            size += accountData.transactions.size() * sizeof(AccountTx);
            for (const auto& [_, innerMap] : accountData.ledgerTxMap)
            {
                size += sizeof(uint32_t) +
                    innerMap.size() * (sizeof(uint32_t) + sizeof(size_t));
            }
        }
        return size / 1024;
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

    ~RWDBDatabase()
    {
        // Regular maps can use standard clear
        accountTxMap_.clear();
        transactionMap_.clear();
        for (auto& ledger : ledgers_)
        {
            ledger.second.transactions.clear();
        }
        ledgers_.clear();
        ledgerHashToSeq_.clear();
    }

    std::vector<std::shared_ptr<Transaction>>
    getTxHistory(LedgerIndex startIndex) override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<std::shared_ptr<Transaction>> result;

        int skipped = 0;
        int collected = 0;

        for (auto it = ledgers_.rbegin(); it != ledgers_.rend(); ++it)
        {
            const auto& transactions = it->second.transactions;
            for (const auto& [txHash, accountTx] : transactions)
            {
                if (skipped < startIndex)
                {
                    ++skipped;
                    continue;
                }

                if (collected >= 20)
                {
                    break;
                }

                std::uint32_t const inLedger = rangeCheckedCast<std::uint32_t>(
                    accountTx.second->getLgrSeq());
                accountTx.first->setStatus(COMMITTED);
                accountTx.first->setLedger(inLedger);
                result.push_back(accountTx.first);
                ++collected;
            }

            if (collected >= 20)
                break;
        }
        return result;
    }

    // Helper function to handle limits
    template <typename Container>
    void
    applyLimit(Container& container, std::size_t limit, bool bUnlimited)
    {
        if (!bUnlimited && limit > 0 && container.size() > limit)
        {
            container.resize(limit);
        }
    }

    AccountTxs
    getOldestAccountTxs(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = accountTxMap_.find(options.account);
        if (it == accountTxMap_.end())
            return {};

        AccountTxs result;
        const auto& accountData = it->second;
        auto txIt = accountData.ledgerTxMap.lower_bound(options.minLedger);
        auto txEnd = accountData.ledgerTxMap.upper_bound(options.maxLedger);

        std::size_t skipped = 0;
        for (; txIt != txEnd &&
             (options.bUnlimited || result.size() < options.limit);
             ++txIt)
        {
            for (const auto& [txSeq, txIndex] : txIt->second)
            {
                if (skipped < options.offset)
                {
                    ++skipped;
                    continue;
                }
                AccountTx const accountTx = accountData.transactions[txIndex];
                std::uint32_t const inLedger = rangeCheckedCast<std::uint32_t>(
                    accountTx.second->getLgrSeq());
                accountTx.first->setStatus(COMMITTED);
                accountTx.first->setLedger(inLedger);
                result.push_back(accountTx);
                if (!options.bUnlimited && result.size() >= options.limit)
                    break;
            }
        }

        return result;
    }

    AccountTxs
    getNewestAccountTxs(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = accountTxMap_.find(options.account);
        if (it == accountTxMap_.end())
            return {};

        AccountTxs result;
        const auto& accountData = it->second;
        auto txIt = accountData.ledgerTxMap.lower_bound(options.minLedger);
        auto txEnd = accountData.ledgerTxMap.upper_bound(options.maxLedger);

        std::size_t skipped = 0;
        for (auto rIt = std::make_reverse_iterator(txEnd);
             rIt != std::make_reverse_iterator(txIt) &&
             (options.bUnlimited || result.size() < options.limit);
             ++rIt)
        {
            for (auto innerRIt = rIt->second.rbegin();
                 innerRIt != rIt->second.rend();
                 ++innerRIt)
            {
                if (skipped < options.offset)
                {
                    ++skipped;
                    continue;
                }
                AccountTx const accountTx =
                    accountData.transactions[innerRIt->second];
                std::uint32_t const inLedger = rangeCheckedCast<std::uint32_t>(
                    accountTx.second->getLgrSeq());
                accountTx.first->setLedger(inLedger);
                result.push_back(accountTx);
                if (!options.bUnlimited && result.size() >= options.limit)
                    break;
            }
        }

        return result;
    }

    MetaTxsList
    getOldestAccountTxsB(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = accountTxMap_.find(options.account);
        if (it == accountTxMap_.end())
            return {};

        MetaTxsList result;
        const auto& accountData = it->second;
        auto txIt = accountData.ledgerTxMap.lower_bound(options.minLedger);
        auto txEnd = accountData.ledgerTxMap.upper_bound(options.maxLedger);

        std::size_t skipped = 0;
        for (; txIt != txEnd &&
             (options.bUnlimited || result.size() < options.limit);
             ++txIt)
        {
            for (const auto& [txSeq, txIndex] : txIt->second)
            {
                if (skipped < options.offset)
                {
                    ++skipped;
                    continue;
                }
                const auto& [txn, txMeta] = accountData.transactions[txIndex];
                result.emplace_back(
                    txn->getSTransaction()->getSerializer().peekData(),
                    txMeta->getAsObject().getSerializer().peekData(),
                    txIt->first);
                if (!options.bUnlimited && result.size() >= options.limit)
                    break;
            }
        }

        return result;
    }

    MetaTxsList
    getNewestAccountTxsB(AccountTxOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = accountTxMap_.find(options.account);
        if (it == accountTxMap_.end())
            return {};

        MetaTxsList result;
        const auto& accountData = it->second;
        auto txIt = accountData.ledgerTxMap.lower_bound(options.minLedger);
        auto txEnd = accountData.ledgerTxMap.upper_bound(options.maxLedger);

        std::size_t skipped = 0;
        for (auto rIt = std::make_reverse_iterator(txEnd);
             rIt != std::make_reverse_iterator(txIt) &&
             (options.bUnlimited || result.size() < options.limit);
             ++rIt)
        {
            for (auto innerRIt = rIt->second.rbegin();
                 innerRIt != rIt->second.rend();
                 ++innerRIt)
            {
                if (skipped < options.offset)
                {
                    ++skipped;
                    continue;
                }
                const auto& [txn, txMeta] =
                    accountData.transactions[innerRIt->second];
                result.emplace_back(
                    txn->getSTransaction()->getSerializer().peekData(),
                    txMeta->getAsObject().getSerializer().peekData(),
                    rIt->first);
                if (!options.bUnlimited && result.size() >= options.limit)
                    break;
            }
        }

        return result;
    }

    std::pair<std::optional<RelationalDatabase::AccountTxMarker>, int>
    accountTxPage(
        std::function<void(std::uint32_t)> const& onUnsavedLedger,
        std::function<
            void(std::uint32_t, std::string const&, Blob&&, Blob&&)> const&
            onTransaction,
        RelationalDatabase::AccountTxPageOptions const& options,
        int limit_used,
        std::uint32_t page_length,
        bool forward)
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = accountTxMap_.find(options.account);
        if (it == accountTxMap_.end())
            return {std::nullopt, 0};

        int total = 0;

        bool lookingForMarker = options.marker.has_value();

        std::uint32_t numberOfResults;

        if (options.limit == 0 || options.limit == UINT32_MAX ||
            (options.limit > page_length && !options.bAdmin))
            numberOfResults = page_length;
        else
            numberOfResults = options.limit;

        if (numberOfResults < limit_used)
            return {options.marker, -1};
        numberOfResults -= limit_used;

        // As an account can have many thousands of transactions, there is a
        // limit placed on the amount of transactions returned. If the limit is
        // reached before the result set has been exhausted (we always query for
        // one more than the limit), then we return an opaque marker that can be
        // supplied in a subsequent query.
        std::uint32_t queryLimit = numberOfResults + 1;
        std::uint32_t findLedger = 0, findSeq = 0;

        if (lookingForMarker)
        {
            findLedger = options.marker->ledgerSeq;
            findSeq = options.marker->txnSeq;
        }

        std::optional<RelationalDatabase::AccountTxMarker> newmarker;
        if (limit_used > 0)
            newmarker = options.marker;

        if (forward)
        {
            // Oldest (forward = true)
            const auto& accountData = it->second;
            auto txIt = accountData.ledgerTxMap.lower_bound(
                findLedger == 0 ? options.minLedger : findLedger);
            auto txEnd = accountData.ledgerTxMap.upper_bound(options.maxLedger);
            for (; txIt != txEnd; ++txIt)
            {
                std::uint32_t const ledgerSeq = txIt->first;
                for (auto seqIt = txIt->second.begin();
                     seqIt != txIt->second.end();
                     ++seqIt)
                {
                    const auto& [txnSeq, index] = *seqIt;
                    if (lookingForMarker)
                    {
                        if (findLedger == ledgerSeq && findSeq == txnSeq)
                        {
                            lookingForMarker = false;
                        }
                        else
                            continue;
                    }
                    else if (numberOfResults == 0)
                    {
                        newmarker = {
                            rangeCheckedCast<std::uint32_t>(ledgerSeq), txnSeq};
                        return {newmarker, total};
                    }

                    Blob rawTxn = accountData.transactions[index]
                                      .first->getSTransaction()
                                      ->getSerializer()
                                      .peekData();
                    Blob rawMeta = accountData.transactions[index]
                                       .second->getAsObject()
                                       .getSerializer()
                                       .peekData();

                    if (rawMeta.size() == 0)
                        onUnsavedLedger(ledgerSeq);

                    onTransaction(
                        rangeCheckedCast<std::uint32_t>(ledgerSeq),
                        "COMMITTED",
                        std::move(rawTxn),
                        std::move(rawMeta));
                    --numberOfResults;
                    ++total;
                }
            }
        }
        else
        {
            // Newest (forward = false)
            const auto& accountData = it->second;
            auto txIt = accountData.ledgerTxMap.lower_bound(options.minLedger);
            auto txEnd = accountData.ledgerTxMap.upper_bound(
                findLedger == 0 ? options.maxLedger : findLedger);
            auto rtxIt = std::make_reverse_iterator(txEnd);
            auto rtxEnd = std::make_reverse_iterator(txIt);
            for (; rtxIt != rtxEnd; ++rtxIt)
            {
                std::uint32_t const ledgerSeq = rtxIt->first;
                for (auto innerRIt = rtxIt->second.rbegin();
                     innerRIt != rtxIt->second.rend();
                     ++innerRIt)
                {
                    const auto& [txnSeq, index] = *innerRIt;
                    if (lookingForMarker)
                    {
                        if (findLedger == ledgerSeq && findSeq == txnSeq)
                        {
                            lookingForMarker = false;
                        }
                        else
                            continue;
                    }
                    else if (numberOfResults == 0)
                    {
                        newmarker = {
                            rangeCheckedCast<std::uint32_t>(ledgerSeq), txnSeq};
                        return {newmarker, total};
                    }

                    Blob rawTxn = accountData.transactions[index]
                                      .first->getSTransaction()
                                      ->getSerializer()
                                      .peekData();
                    Blob rawMeta = accountData.transactions[index]
                                       .second->getAsObject()
                                       .getSerializer()
                                       .peekData();

                    if (rawMeta.size() == 0)
                        onUnsavedLedger(ledgerSeq);

                    onTransaction(
                        rangeCheckedCast<std::uint32_t>(ledgerSeq),
                        "COMMITTED",
                        std::move(rawTxn),
                        std::move(rawMeta));
                    --numberOfResults;
                    ++total;
                }
            }
        }
        return {newmarker, total};
    }

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    oldestAccountTxPage(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        static std::uint32_t const page_length(200);
        auto onUnsavedLedger =
            std::bind(saveLedgerAsync, std::ref(app_), std::placeholders::_1);
        AccountTxs ret;
        Application& app = app_;
        auto onTransaction = [&ret, &app](
                                 std::uint32_t ledger_index,
                                 std::string const& status,
                                 Blob&& rawTxn,
                                 Blob&& rawMeta) {
            convertBlobsToTxResult(
                ret, ledger_index, status, rawTxn, rawMeta, app);
        };

        auto newmarker =
            accountTxPage(
                onUnsavedLedger, onTransaction, options, 0, page_length, true)
                .first;
        return {ret, newmarker};
    }

    std::pair<AccountTxs, std::optional<AccountTxMarker>>
    newestAccountTxPage(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        static std::uint32_t const page_length(200);
        auto onUnsavedLedger =
            std::bind(saveLedgerAsync, std::ref(app_), std::placeholders::_1);
        AccountTxs ret;
        Application& app = app_;
        auto onTransaction = [&ret, &app](
                                 std::uint32_t ledger_index,
                                 std::string const& status,
                                 Blob&& rawTxn,
                                 Blob&& rawMeta) {
            convertBlobsToTxResult(
                ret, ledger_index, status, rawTxn, rawMeta, app);
        };

        auto newmarker =
            accountTxPage(
                onUnsavedLedger, onTransaction, options, 0, page_length, false)
                .first;
        return {ret, newmarker};
    }

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    oldestAccountTxPageB(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        static std::uint32_t const page_length(500);
        auto onUnsavedLedger =
            std::bind(saveLedgerAsync, std::ref(app_), std::placeholders::_1);
        MetaTxsList ret;
        auto onTransaction = [&ret](
                                 std::uint32_t ledgerIndex,
                                 std::string const& status,
                                 Blob&& rawTxn,
                                 Blob&& rawMeta) {
            ret.emplace_back(
                std::move(rawTxn), std::move(rawMeta), ledgerIndex);
        };
        auto newmarker =
            accountTxPage(
                onUnsavedLedger, onTransaction, options, 0, page_length, true)
                .first;
        return {ret, newmarker};
    }

    std::pair<MetaTxsList, std::optional<AccountTxMarker>>
    newestAccountTxPageB(AccountTxPageOptions const& options) override
    {
        if (!useTxTables_)
            return {};

        static std::uint32_t const page_length(500);
        auto onUnsavedLedger =
            std::bind(saveLedgerAsync, std::ref(app_), std::placeholders::_1);
        MetaTxsList ret;
        auto onTransaction = [&ret](
                                 std::uint32_t ledgerIndex,
                                 std::string const& status,
                                 Blob&& rawTxn,
                                 Blob&& rawMeta) {
            ret.emplace_back(
                std::move(rawTxn), std::move(rawMeta), ledgerIndex);
        };
        auto newmarker =
            accountTxPage(
                onUnsavedLedger, onTransaction, options, 0, page_length, false)
                .first;
        return {ret, newmarker};
    }
};

// Factory function
std::unique_ptr<SQLiteDatabase>
getRWDBDatabase(Application& app, Config const& config, JobQueue& jobQueue)
{
    return std::make_unique<RWDBDatabase>(app, config, jobQueue);
}

}  // namespace ripple
#endif  // RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED
