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

#include <ripple/app/ledger/TransactionMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/basics/chrono.h>
#include <ripple/protocol/STTx.h>

namespace ripple {

TransactionMaster::TransactionMaster(Application& app)
    : mApp(app)
    , mCache(
          "TransactionCache",
          65536,
          std::chrono::minutes{30},
          stopwatch(),
          mApp.journal("TaggedCache"))
{
}

bool
TransactionMaster::inLedger(uint256 const& hash, std::uint32_t ledger)
{
    auto txn = mCache.fetch(hash);

    if (!txn)
        return false;

    txn->setStatus(COMMITTED, ledger);
    return true;
}

std::shared_ptr<Transaction>
TransactionMaster::fetch_from_cache(uint256 const& txnID)
{
    return mCache.fetch(txnID);
}

std::variant<
    std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>,
    TxSearched>
TransactionMaster::fetch(uint256 const& txnID, error_code_i& ec)
{
    using TxPair =
        std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>;

    if (auto txn = fetch_from_cache(txnID); txn && !txn->isValidated())
        return std::pair{std::move(txn), nullptr};

    auto v = Transaction::load(txnID, mApp, ec);

    if (std::holds_alternative<TxSearched>(v))
        return v;

    auto [txn, txnMeta] = std::get<TxPair>(v);

    if (txn)
        mCache.retrieve_or_insert(txnID, txn);

    return std::pair{std::move(txn), std::move(txnMeta)};
}

std::variant<
    std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>,
    TxSearched>
TransactionMaster::fetch(
    uint256 const& txnID,
    ClosedInterval<uint32_t> const& range,
    error_code_i& ec)
{
    using TxPair =
        std::pair<std::shared_ptr<Transaction>, std::shared_ptr<TxMeta>>;

    if (auto txn = fetch_from_cache(txnID); txn && !txn->isValidated())
        return std::pair{std::move(txn), nullptr};

    auto v = Transaction::load(txnID, mApp, range, ec);

    if (std::holds_alternative<TxSearched>(v))
        return v;

    auto [txn, txnMeta] = std::get<TxPair>(v);

    if (txn)
        mCache.retrieve_or_insert(txnID, txn);

    return std::pair{std::move(txn), std::move(txnMeta)};
}

std::shared_ptr<STTx const>
TransactionMaster::fetch(
    boost::intrusive_ptr<SHAMapItem const> const& item,
    SHAMapNodeType type,
    std::uint32_t uCommitLedger)
{
    std::shared_ptr<STTx const> txn;
    auto iTx = fetch_from_cache(item->key());

    if (!iTx)
    {
        if (type == SHAMapNodeType::tnTRANSACTION_NM)
            txn = std::make_shared<STTx const>(item->slice());
        else if (type == SHAMapNodeType::tnTRANSACTION_MD)
            txn =
                std::make_shared<STTx const>(SerialIter{item->slice()}.getVL());
    }
    else
    {
        if (uCommitLedger)
            iTx->setStatus(COMMITTED, uCommitLedger);

        txn = iTx->getSTransaction();
    }

    return txn;
}

std::shared_ptr<Transaction>
TransactionMaster::canonicalize(std::shared_ptr<Transaction> tx)
{
    // Note that retrieve_or_insert can change the value of tx
    if (uint256 const tid = tx->getID(); tid != beast::zero)
        mCache.retrieve_or_insert(tid, tx);

    return tx;
}

void
TransactionMaster::sweep()
{
    mCache.sweep();
}

Json::Value
TransactionMaster::info() const
{
    return mCache.info();
}

}  // namespace ripple
