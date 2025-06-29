//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-19 Ripple Labs Inc.

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

#ifndef RIPPLE_TXQ_H_INCLUDED
#define RIPPLE_TXQ_H_INCLUDED

#include <ripple/app/tx/applySteps.h>
#include <ripple/ledger/ApplyView.h>
#include <ripple/ledger/OpenView.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/protocol/RippleLedgerHash.h>
#include <ripple/protocol/STTx.h>
#include <ripple/protocol/SeqProxy.h>
#include <ripple/protocol/TER.h>
#include <ripple/basics/XRPAmount.h>
#include <ripple/protocol/AccountID.h>
#include <ripple/basics/base_uint.h>
#include <ripple/basics/FeeUnits.h>
#include <ripple/beast/hash/uhash.h>
#include <ripple/beast/utility/Journal.h>
#include <ripple/json/json_value.h>
#include <optional>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>

namespace ripple {

class Application;
class Config;

/**
    Simplified Transaction Queue - Fast multithreaded implementation
    that just queues transactions and applies them without validation.
    Uses a map to detect and prevent duplicate transactions.
*/
class TxQ
{
private:
    // Simple transaction holder
    struct QueuedTx
    {
        std::shared_ptr<STTx const> tx;
        uint256 txID;
        ApplyFlags flags;
        
        QueuedTx(std::shared_ptr<STTx const> const& tx_, ApplyFlags flags_)
            : tx(tx_), txID(tx_->getTransactionID()), flags(flags_)
        {}
    };

    // Thread-safe map to detect duplicates
    mutable std::shared_mutex queueMutex_;
    std::unordered_map<uint256, QueuedTx, beast::uhash<>> txMap_;
    
    // Debug injection queue
    std::mutex debugTxInjectMutex_;
    std::vector<STTx> debugTxInjectQueue_;
    
    // Metrics tracking (atomic for lock-free reads)
    std::atomic<size_t> totalQueued_{0};
    std::atomic<size_t> totalApplied_{0};

public:
    void debugTxInject(STTx const& txn);

    /// Fee level for single-signed reference transaction.
    static constexpr FeeLevel64 baseLevel{256};

    /**
        Structure used to customize @ref TxQ behavior.
    */
    struct Setup
    {
        explicit Setup() = default;
        std::size_t ledgersInQueue = 20;
        std::size_t queueSizeMin = 2000;
        std::uint32_t retrySequencePercent = 25;
        FeeLevel64 minimumEscalationMultiplier = baseLevel * 500;
        std::uint32_t minimumTxnInLedger = 5000;
        std::uint32_t minimumTxnInLedgerSA = 1000;
        std::uint32_t targetTxnInLedger = 10000;
        std::optional<std::uint32_t> maximumTxnInLedger;
        std::uint32_t normalConsensusIncreasePercent = 20;
        std::uint32_t slowConsensusDecreasePercent = 50;
        std::uint32_t maximumTxnPerAccount = 10;
        std::uint32_t minimumLastLedgerBuffer = 2;
        bool standAlone = false;
    };

    struct Metrics
    {
        explicit Metrics() = default;
        std::size_t txCount;
        std::optional<std::size_t> txQMaxSize;
        std::size_t txInLedger;
        std::size_t txPerLedger;
        FeeLevel64 referenceFeeLevel;
        FeeLevel64 minProcessingFeeLevel;
        FeeLevel64 medFeeLevel;
        FeeLevel64 openLedgerFeeLevel;
    };

    struct TxDetails
    {
        TxDetails(
            FeeLevel64 feeLevel_,
            std::optional<LedgerIndex> const& lastValid_,
            TxConsequences const& consequences_,
            AccountID const& account_,
            SeqProxy seqProxy_,
            std::shared_ptr<STTx const> const& txn_,
            int retriesRemaining_,
            TER preflightResult_,
            std::optional<TER> lastResult_)
            : feeLevel(feeLevel_)
            , lastValid(lastValid_)
            , firstValid(std::nullopt)
            , consequences(consequences_)
            , account(account_)
            , seqProxy(seqProxy_)
            , txn(txn_)
            , retriesRemaining(retriesRemaining_)
            , preflightResult(preflightResult_)
            , lastResult(lastResult_)
        {
        }

        FeeLevel64 feeLevel;
        std::optional<LedgerIndex> lastValid;
        std::optional<LedgerIndex> firstValid;
        TxConsequences consequences;
        AccountID account;
        SeqProxy seqProxy;
        std::shared_ptr<STTx const> txn;
        int retriesRemaining;
        TER preflightResult;
        std::optional<TER> lastResult;
    };

    TxQ(Setup const& setup, beast::Journal j);
    virtual ~TxQ();

    std::pair<TER, bool>
    apply(
        Application& app,
        OpenView& view,
        std::shared_ptr<STTx const> const& tx,
        ApplyFlags flags,
        beast::Journal j);

    bool
    accept(Application& app, OpenView& view);

    void
    processClosedLedger(Application& app, ReadView const& view, bool timeLeap);

    SeqProxy
    nextQueuableSeq(std::shared_ptr<SLE const> const& sleAccount) const;

    Metrics
    getMetrics(OpenView const& view) const;

    struct FeeAndSeq
    {
        XRPAmount fee;
        std::uint32_t accountSeq;
        std::uint32_t availableSeq;
    };

    FeeAndSeq
    getTxRequiredFeeAndSeq(
        OpenView const& view,
        std::shared_ptr<STTx const> const& tx) const;

    std::vector<TxDetails>
    getAccountTxs(AccountID const& account) const;

    std::vector<TxDetails>
    getTxs() const;

    Json::Value
    doRPC(
        Application& app,
        std::optional<XRPAmount> hookFeeUnits = std::nullopt) const;

    // Helper function for TxQ::apply - always returns empty optional in simplified version
    std::optional<std::pair<TER, bool>>
    tryDirectApply(
        Application& app,
        OpenView& view,
        std::shared_ptr<STTx const> const& tx,
        ApplyFlags flags,
        beast::Journal j);

private:
    Setup const setup_;
    beast::Journal const j_;
};

TxQ::Setup
setup_TxQ(Config const&);

template <class T>
XRPAmount
toDrops(FeeLevel<T> const& level, XRPAmount baseFee)
{
    if (auto const drops = mulDiv(level, baseFee, TxQ::baseLevel); drops.first)
        return drops.second;
    return XRPAmount(STAmount::cMaxNativeN);
}

inline FeeLevel64
toFeeLevel(XRPAmount const& drops, XRPAmount const& baseFee)
{
    if (auto const feeLevel = mulDiv(drops, TxQ::baseLevel, baseFee);
        feeLevel.first)
        return feeLevel.second;
    return FeeLevel64(std::numeric_limits<std::uint64_t>::max());
}

}  // namespace ripple

#endif
