#ifndef RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED
#define RIPPLE_APP_RDB_BACKEND_MEMORYDATABASE_H_INCLUDED
#include <ripple/app/rdb/RelationalDatabase.h>
#include <boost/container/flat_map.hpp>
#include <shared_mutex>

namespace ripple {

class MemoryDatabase : public RelationalDatabase
{
private:
    mutable std::shared_mutex mutex_;
    boost::container::flat_map<LedgerIndex, LedgerInfo> ledgers_;
    boost::container::flat_map<uint256, LedgerInfo> ledgersByHash_;
    boost::container::flat_map<LedgerIndex, LedgerHashPair> ledgerHashes_;
    std::vector<std::shared_ptr<Transaction>> txHistory_;

public:
    MemoryDatabase(ripple::Application& app, ripple::Config const& conf, ripple::JobQueue& jq)
    {
    }

    std::optional<LedgerIndex> getMinLedgerSeq() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return {};
        return ledgers_.begin()->first;
    }

    std::optional<LedgerIndex> getMaxLedgerSeq() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return {};
        return ledgers_.rbegin()->first;
    }

    std::optional<LedgerInfo> getLedgerInfoByIndex(LedgerIndex ledgerSeq) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgers_.find(ledgerSeq);
        if (it == ledgers_.end())
            return {};
        return it->second;
    }

    std::optional<LedgerInfo> getNewestLedgerInfo() override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (ledgers_.empty())
            return {};
        return ledgers_.rbegin()->second;
    }

    std::optional<LedgerInfo> getLedgerInfoByHash(uint256 const& ledgerHash) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgersByHash_.find(ledgerHash);
        if (it == ledgersByHash_.end())
            return {};
        return it->second;
    }

    uint256 getHashByIndex(LedgerIndex ledgerIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgerHashes_.find(ledgerIndex);
        if (it == ledgerHashes_.end())
            return {};
        return it->second.ledgerHash;
    }

    std::optional<LedgerHashPair> getHashesByIndex(LedgerIndex ledgerIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = ledgerHashes_.find(ledgerIndex);
        if (it == ledgerHashes_.end())
            return {};
        return it->second;
    }

    std::map<LedgerIndex, LedgerHashPair> getHashesByIndex(LedgerIndex minSeq, LedgerIndex maxSeq) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::map<LedgerIndex, LedgerHashPair> result;
        auto it = ledgerHashes_.lower_bound(minSeq);
        auto end = ledgerHashes_.upper_bound(maxSeq);
        for (; it != end; ++it)
        {
            result[it->first] = it->second;
        }
        return result;
    }

    std::vector<std::shared_ptr<Transaction>> getTxHistory(LedgerIndex startIndex) override
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (startIndex >= txHistory_.size())
            return {};
        return std::vector<std::shared_ptr<Transaction>>(
            txHistory_.begin() + startIndex,
            txHistory_.begin() + std::min(static_cast<size_t>(startIndex + 20), txHistory_.size()));
    }

    bool ledgerDbHasSpace(Config const& config) override
    {
        // In-memory database always has space
        return true;
    }

    bool transactionDbHasSpace(Config const& config) override
    {
        // In-memory database always has space
        return true;
    }

    // Add methods to insert data (these would be called when saving ledgers/transactions)
    void insertLedger(LedgerInfo const& ledgerInfo)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        ledgers_[ledgerInfo.seq] = ledgerInfo;
        ledgersByHash_[ledgerInfo.hash] = ledgerInfo;
        ledgerHashes_[ledgerInfo.seq] = {ledgerInfo.hash, ledgerInfo.parentHash};
    }

    void insertTransaction(std::shared_ptr<Transaction> const& tx)
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        txHistory_.push_back(tx);
    }
};

std::unique_ptr<RelationalDatabase>
getMemoryDatabase(Application& app, Config const& config, JobQueue& jobQueue)
{
    return std::make_unique<MemoryDatabase>(app, config, jobQueue);
}



} // namespace ripple
#endif
