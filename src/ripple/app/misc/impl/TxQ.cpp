//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013, 2019 Ripple Labs Inc.

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

#include <ripple/app/ledger/OpenLedger.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/tx/apply.h>
#include <ripple/basics/mulDiv.h>
#include <ripple/basics/Slice.h>
#include <ripple/protocol/jss.h>
#include <ripple/protocol/st.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/Serializer.h>
#include <ripple/beast/utility/Zero.h>
#include <ripple/core/Config.h>
#include <algorithm>
#include <limits>
#include <numeric>
#include <thread>
#include <future>

namespace ripple {

void
TxQ::debugTxInject(STTx const& txn)
{
    const std::lock_guard<std::mutex> _(debugTxInjectMutex_);
    debugTxInjectQueue_.push_back(txn);
}

TxQ::TxQ(Setup const& setup, beast::Journal j)
    : setup_(setup), j_(j)
{
}

TxQ::~TxQ()
{
    // Ensure map is empty
    std::unique_lock<std::shared_mutex> lock(queueMutex_);
    txMap_.clear();
}

std::pair<TER, bool>
TxQ::apply(
    Application& app,
    OpenView& view,
    std::shared_ptr<STTx const> const& tx,
    ApplyFlags flags,
    beast::Journal j)
{
    uint256 const txID = tx->getTransactionID();
    size_t queueSize = 0;
    
    // Check for duplicate and queue if new
    {
        std::unique_lock<std::shared_mutex> lock(queueMutex_);
        
        // Check if transaction already exists
        if (txMap_.find(txID) != txMap_.end())
        {
            JLOG(j_.debug()) << "Transaction " << txID 
                             << " already in queue, ignoring duplicate";
            return {terQUEUED, false};
        }
        
        // Add new transaction
        txMap_.emplace(txID, QueuedTx{tx, flags});
        totalQueued_++;
        queueSize = txMap_.size();
    }
    
    JLOG(j_.debug()) << "Queued transaction " << txID 
                     << " without validation. Queue size: " << queueSize;
    
    return {terQUEUED, false};
}

std::optional<std::pair<TER, bool>>
TxQ::tryDirectApply(
    Application& app,
    OpenView& view,
    std::shared_ptr<STTx const> const& tx,
    ApplyFlags flags,
    beast::Journal j)
{
    // In simplified version, never try direct apply - always queue
    return std::nullopt;
}

bool
TxQ::accept(Application& app, OpenView& view)
{
    bool ledgerChanged = false;
    
    // Handle debug injected transactions first
    {
        std::unique_lock<std::mutex> trylock(debugTxInjectMutex_, std::try_to_lock);
        if (trylock.owns_lock() && !debugTxInjectQueue_.empty())
        {
            for (STTx const& txn : debugTxInjectQueue_)
            {
                auto txnHash = txn.getTransactionID();
                app.getHashRouter().setFlags(txnHash, SF_EMITTED | SF_PRIVATE2);

                auto const& emitted = const_cast<ripple::STTx&>(txn).downcast<STObject>();
                auto s = std::make_shared<ripple::Serializer>();
                emitted.add(*s);
                view.rawTxInsert(txnHash, std::move(s), nullptr);
                ledgerChanged = true;
            }
            debugTxInjectQueue_.clear();
        }
    }

    // Apply all queued transactions in parallel batches
    constexpr size_t BATCH_SIZE = 100;
    std::vector<QueuedTx> batch;
    batch.reserve(BATCH_SIZE);
    
    while (true)
    {
        // Get a batch of transactions
        {
            std::unique_lock<std::shared_mutex> lock(queueMutex_);
            if (txMap_.empty())
                break;
                
            // Extract up to BATCH_SIZE transactions
            auto it = txMap_.begin();
            while (it != txMap_.end() && batch.size() < BATCH_SIZE)
            {
                batch.push_back(std::move(it->second));
                it = txMap_.erase(it);
            }
        }
        
        // Apply the batch
        for (auto& qtx : batch)
        {
            try
            {
                view.getAndResetKeysTouched();
                auto const [txnResult, didApply] = 
                    ripple::apply(app, view, *qtx.tx, qtx.flags, j_);
                
                if (didApply)
                {
                    app.getHashRouter().setTouchedKeys(qtx.txID, view.getAndResetKeysTouched());
                    ledgerChanged = true;
                    totalApplied_++;
                }
                
                JLOG(j_.trace()) << "Applied queued transaction " << qtx.txID
                               << " with result " << txnResult;
            }
            catch (std::exception const& e)
            {
                JLOG(j_.warn()) << "Exception applying transaction " << qtx.txID
                              << ": " << e.what();
            }
        }
        
        batch.clear();
    }

    return ledgerChanged;
}

void
TxQ::processClosedLedger(Application& app, ReadView const& view, bool timeLeap)
{
    // Nothing to do in simplified implementation
    JLOG(j_.trace()) << "processClosedLedger called - no-op in simplified TxQ";
}

SeqProxy
TxQ::nextQueuableSeq(std::shared_ptr<SLE const> const& sleAccount) const
{
    if (!sleAccount || sleAccount->getType() != ltACCOUNT_ROOT)
        return SeqProxy::sequence(0);
    
    return SeqProxy::sequence((*sleAccount)[sfSequence]);
}

TxQ::Metrics
TxQ::getMetrics(OpenView const& view) const
{
    Metrics result;
    
    {
        std::shared_lock<std::shared_mutex> lock(queueMutex_);
        result.txCount = txMap_.size();
    }
    
    result.txQMaxSize = setup_.queueSizeMin;
    result.txInLedger = view.txCount();
    result.txPerLedger = setup_.targetTxnInLedger;
    result.referenceFeeLevel = baseLevel;
    result.minProcessingFeeLevel = baseLevel;
    result.medFeeLevel = baseLevel;
    result.openLedgerFeeLevel = baseLevel;
    
    return result;
}

TxQ::FeeAndSeq
TxQ::getTxRequiredFeeAndSeq(
    OpenView const& view,
    std::shared_ptr<STTx const> const& tx) const
{
    auto const account = (*tx)[sfAccount];
    auto const baseFee = view.fees().base;  // Use the base fee from the view
    auto const sle = view.read(keylet::account(account));
    
    std::uint32_t const accountSeq = sle ? (*sle)[sfSequence] : 0;
    
    return {baseFee, accountSeq, accountSeq};
}

std::vector<TxQ::TxDetails>
TxQ::getAccountTxs(AccountID const& account) const
{
    std::vector<TxDetails> result;
    
    std::shared_lock<std::shared_mutex> lock(queueMutex_);
    
    // Search through the map for transactions from this account
    for (auto const& [txID, qtx] : txMap_)
    {
        if (qtx.tx->getAccountID(sfAccount) == account)
        {
            TxConsequences consequences(*qtx.tx);  // Dereference the shared_ptr
            result.emplace_back(
                baseLevel,                              // feeLevel
                std::nullopt,                           // lastValid
                consequences,                           // consequences
                account,                                // account
                qtx.tx->getSeqProxy(),                  // seqProxy
                qtx.tx,                                 // txn
                0,                                      // retriesRemaining
                tesSUCCESS,                            // preflightResult
                std::nullopt                           // lastResult
            );
        }
    }
    
    return result;
}

std::vector<TxQ::TxDetails>
TxQ::getTxs() const
{
    std::vector<TxDetails> result;
    
    std::shared_lock<std::shared_mutex> lock(queueMutex_);
    result.reserve(txMap_.size());
    
    // Create stub TxDetails for each queued transaction
    for (auto const& [txID, qtx] : txMap_)
    {
        TxConsequences consequences(*qtx.tx);  // Dereference the shared_ptr
        result.emplace_back(
            baseLevel,                              // feeLevel
            std::nullopt,                           // lastValid
            consequences,                           // consequences
            qtx.tx->getAccountID(sfAccount),        // account
            qtx.tx->getSeqProxy(),                  // seqProxy
            qtx.tx,                                 // txn
            0,                                      // retriesRemaining
            tesSUCCESS,                            // preflightResult
            std::nullopt                           // lastResult
        );
    }
    
    return result;
}

Json::Value
TxQ::doRPC(Application& app, std::optional<XRPAmount> hookFeeUnits) const
{
    auto const view = app.openLedger().current();
    if (!view)
        return {};

    auto const metrics = getMetrics(*view);

    Json::Value ret(Json::objectValue);
    auto& levels = ret[jss::levels] = Json::objectValue;

    ret[jss::ledger_current_index] = view->info().seq;
    ret[jss::expected_ledger_size] = std::to_string(metrics.txPerLedger);
    ret[jss::current_ledger_size] = std::to_string(metrics.txInLedger);
    ret[jss::current_queue_size] = std::to_string(metrics.txCount);
    ret[jss::max_queue_size] = std::to_string(*metrics.txQMaxSize);

    levels[jss::reference_level] = std::to_string(metrics.referenceFeeLevel.fee());
    levels[jss::minimum_level] = std::to_string(metrics.minProcessingFeeLevel.fee());
    levels[jss::median_level] = std::to_string(metrics.medFeeLevel.fee());
    levels[jss::open_ledger_level] = std::to_string(metrics.openLedgerFeeLevel.fee());

    auto const baseFee = hookFeeUnits ? XRPAmount{hookFeeUnits->drops()} : view->fees().base;
    auto& drops = ret[jss::drops] = Json::Value();

    drops[jss::base_fee_no_hooks] = std::to_string(view->fees().base.drops());
    drops[jss::base_fee] = std::to_string(baseFee.drops());
    drops[jss::median_fee] = std::to_string(baseFee.drops());
    drops[jss::minimum_fee] = std::to_string(baseFee.drops());
    drops[jss::open_ledger_fee] = std::to_string(baseFee.drops());

    return ret;
}

TxQ::Setup
setup_TxQ(Config const& config)
{
    TxQ::Setup setup;
    
    // Just use default values - in a simplified TxQ most of these aren't used anyway
    setup.standAlone = config.standalone();
    
    return setup;
}

}  // namespace ripple
