//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/ledger/InboundTransactions.h>
#include <ripple/app/ledger/impl/TransactionAcquire.h>
#include <ripple/app/main/Application.h>
#include <ripple/resource/Fees.h>
#include <cassert>
#include <memory>
#include <mutex>

namespace ripple {

struct InboundTransactionSet
{
    std::shared_ptr<TransactionAcquire> acquire;
    std::shared_ptr<SHAMap> txset;
    std::uint32_t seq = 0;

    InboundTransactionSet() = default;
    InboundTransactionSet(InboundTransactionSet&&) = default;

    InboundTransactionSet&
    operator=(InboundTransactionSet&&) = delete;
    InboundTransactionSet(InboundTransactionSet const&) = delete;
    InboundTransactionSet&
    operator=(InboundTransactionSet const&) = delete;
};

class InboundTransactionsImp : public InboundTransactions
{
    static constexpr int startPeers = 2;
    static constexpr std::uint32_t setKeepRounds = 3;

    Application& app_;
    std::mutex lock_;
    hash_map<uint256, InboundTransactionSet> map_;
    std::shared_ptr<SHAMap> emptyMap_;
    std::function<void(std::shared_ptr<SHAMap> const&, bool)> gotSet_;
    std::unique_ptr<PeerSetBuilder> peerSetBuilder_;
    std::uint32_t seq_ = 0;
    std::atomic<bool> stopping_ = false;

    auto
    find(uint256 const& hash)
    {
        if (stopping_)
            return map_.end();

        return map_.find(hash);
    }

public:
    InboundTransactionsImp(
        Application& app,
        beast::insight::Collector::ptr const& collector,
        std::function<void(std::shared_ptr<SHAMap> const&, bool)> gotSet,
        std::unique_ptr<PeerSetBuilder> peerSetBuilder)
        : app_(app)
        , gotSet_(std::move(gotSet))
        , peerSetBuilder_(std::move(peerSetBuilder))
    {
        emptyMap_ = std::make_shared<SHAMap>(
            SHAMapType::TRANSACTION, uint256(), app_.getNodeFamily());
        emptyMap_->setUnbacked();
    }

    std::shared_ptr<SHAMap>
    get(uint256 const& hash) override
    {
        if (hash.isZero())
            return emptyMap_;

        std::lock_guard sl(lock_);

        if (auto it = find(hash); it != map_.end())
            return it->second.txset;

        return {};
    }

    std::shared_ptr<SHAMap>
    acquire(uint256 const& hash) override
    {
        if (hash.isZero())
            return emptyMap_;

        std::shared_ptr<TransactionAcquire> ta;

        {
            std::lock_guard sl(lock_);

            if (auto it = find(hash); it != map_.end())
            {
                it->second.seq = seq_;

                if (it->second.acquire)
                    it->second.acquire->stillNeed();

                return it->second.txset;
            }

            if (stopping_)
                return {};

            ta = std::make_shared<TransactionAcquire>(
                app_, hash, peerSetBuilder_->build());

            auto& obj = map_[hash];
            obj.acquire = ta;
            obj.seq = seq_;
        }

        assert(ta != nullptr);
        ta->init(startPeers);

        return {};
    }

    void
    gotData(
        uint256 const& hash,
        std::shared_ptr<Peer> peer,
        std::vector<std::pair<SHAMapNodeID, Slice>> const& data) override
    {
        assert(!data.empty());

        if (hash.isZero())
            return;

        auto ta = [this, &hash]() -> std::shared_ptr<TransactionAcquire> {
            std::lock_guard sl(lock_);
            if (auto it = find(hash); it != map_.end())
                return it->second.acquire;
            return {};
        }();

        if (!ta || !ta->takeNodes(data, peer).isUseful())
            peer->charge(Resource::feeUnwantedData);
    }

    void
    giveSet(
        uint256 const& hash,
        std::shared_ptr<SHAMap> const& set,
        bool fromAcquire) override
    {
        if (hash.isZero())
            return;

        bool isNew = true;

        {
            std::lock_guard sl(lock_);

            if (stopping_)
                return;

            auto& inboundSet = map_[hash];

            if (inboundSet.seq < seq_)
                inboundSet.seq = seq_;

            if (inboundSet.txset)
                isNew = false;
            else
                inboundSet.txset = set;

            inboundSet.acquire.reset();
        }

        if (isNew)
            gotSet_(set, fromAcquire);
    }

    void
    newRound(std::uint32_t seq) override
    {
        std::lock_guard lock(lock_);

        if (stopping_ || seq_ == seq)
            return;

        seq_ = seq;

        auto const maxSeq = seq +
            std::min(setKeepRounds,
                     std::numeric_limits<std::uint32_t>::max() - seq);
        auto const minSeq = seq - std::min(seq, setKeepRounds);

        std::erase_if(map_, [minSeq, maxSeq](auto const& entry) {
            return (entry.second.seq < minSeq) || (entry.second.seq > maxSeq);
        });
    }

    void
    stop() override
    {
        if (stopping_.exchange(true))
            return;

        std::lock_guard lock(lock_);
        map_.clear();
    }
};

//------------------------------------------------------------------------------

std::unique_ptr<InboundTransactions>
make_InboundTransactions(
    Application& app,
    beast::insight::Collector::ptr const& collector,
    std::function<void(std::shared_ptr<SHAMap> const&, bool)> gotSet)
{
    return std::make_unique<InboundTransactionsImp>(
        app, collector, std::move(gotSet), make_PeerSetBuilder(app));
}

}  // namespace ripple
