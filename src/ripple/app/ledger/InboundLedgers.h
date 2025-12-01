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

#ifndef RIPPLE_APP_LEDGER_INBOUNDLEDGERS_H_INCLUDED
#define RIPPLE_APP_LEDGER_INBOUNDLEDGERS_H_INCLUDED

#include <ripple/app/ledger/InboundLedger.h>
#include <ripple/protocol/RippleLedgerHash.h>
#include <memory>
#include <optional>

namespace ripple {

/** Manages the lifetime of inbound ledgers.

    @see InboundLedger
*/
class InboundLedgers
{
public:
    using clock_type = beast::abstract_clock<std::chrono::steady_clock>;

    virtual ~InboundLedgers() = default;

    // VFALCO TODO Should this be called findOrAdd ?
    // Callers should use this if they possibly need an authoritative
    // response immediately.
    //
    virtual std::shared_ptr<Ledger const>
    acquire(uint256 const& hash, std::uint32_t seq, InboundLedger::Reason) = 0;

    // Callers should use this if they are known to be executing on the Job
    // Queue. TODO review whether all callers of acquire() can use this
    // instead. Inbound ledger acquisition is asynchronous anyway.
    virtual void
    acquireAsync(
        uint256 const& hash,
        std::uint32_t seq,
        InboundLedger::Reason reason) = 0;

    virtual std::shared_ptr<InboundLedger>
    find(LedgerHash const& hash) = 0;

    /** Get a partial ledger (has header but may be incomplete).
        Used for partial sync mode - allows RPC queries against
        ledgers that are still being acquired.
        @return The ledger if header exists and not failed, nullptr otherwise.
    */
    virtual std::shared_ptr<Ledger const>
    getPartialLedger(uint256 const& hash) = 0;

    /** Find which partial ledger contains a transaction.
        Used by submit_and_wait to locate transactions as they appear
        in incoming ledgers' txMaps.
        @param txHash The transaction hash to search for
        @return The ledger hash if found, nullopt otherwise
    */
    virtual std::optional<uint256>
    findTxLedger(uint256 const& txHash) = 0;

    /** Add a priority node hash for immediate fetching.
        Used by partial sync mode to prioritize specific nodes
        needed by queries.
        @param ledgerSeq The ledger sequence being acquired
        @param nodeHash The specific node hash to prioritize
    */
    virtual void
    addPriorityNode(std::uint32_t ledgerSeq, uint256 const& nodeHash) = 0;

    /** Add a ledger range where TX fetching should be prioritized.
        Ledgers in this range will fetch TX nodes BEFORE state nodes.
        Used by submit_and_wait to quickly detect transactions.
        @param start First ledger sequence (inclusive)
        @param end Last ledger sequence (inclusive)
    */
    virtual void
    prioritizeTxForLedgers(std::uint32_t start, std::uint32_t end) = 0;

    /** Check if TX fetching should be prioritized for a ledger sequence. */
    virtual bool
    isTxPrioritized(std::uint32_t seq) const = 0;

    // VFALCO TODO Remove the dependency on the Peer object.
    //
    virtual bool
    gotLedgerData(
        LedgerHash const& ledgerHash,
        std::shared_ptr<Peer>,
        std::shared_ptr<protocol::TMLedgerData>) = 0;

    virtual void
    gotStaleData(std::shared_ptr<protocol::TMLedgerData> packet) = 0;

    virtual void
    logFailure(uint256 const& h, std::uint32_t seq) = 0;

    virtual bool
    isFailure(uint256 const& h) = 0;

    virtual void
    clearFailures() = 0;

    virtual Json::Value
    getInfo() = 0;

    /** Returns the rate of historical ledger fetches per minute. */
    virtual std::size_t
    fetchRate() = 0;

    /** Called when a complete ledger is obtained. */
    virtual void
    onLedgerFetched() = 0;

    virtual void
    gotFetchPack() = 0;
    virtual void
    sweep() = 0;

    virtual void
    stop() = 0;
};

std::unique_ptr<InboundLedgers>
make_InboundLedgers(
    Application& app,
    InboundLedgers::clock_type& clock,
    beast::insight::Collector::ptr const& collector);

}  // namespace ripple

#endif
