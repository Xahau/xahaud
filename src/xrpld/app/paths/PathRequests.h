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

#ifndef RIPPLE_APP_PATHS_PATHREQUESTS_H_INCLUDED
#define RIPPLE_APP_PATHS_PATHREQUESTS_H_INCLUDED

#include <xrpld/app/main/Application.h>
#include <xrpld/app/paths/PathRequest.h>
#include <xrpld/app/paths/PayGraph.h>
#include <xrpld/app/paths/RippleLineCache.h>
#include <xrpld/core/Job.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace ripple {

class PathRequests
{
public:
    /** A collection of all PathRequest instances. */
    PathRequests(
        Application& app,
        beast::Journal journal,
        beast::insight::Collector::ptr const& collector)
        : app_(app), mJournal(journal), mLastIdentifier(0)
    {
        mFast = collector->make_event("pathfind_fast");
        mFull = collector->make_event("pathfind_full");
    }

    /** Update all of the contained PathRequest instances.

        @param ledger Ledger we are pathfinding in.
     */
    void
    updateAll(std::shared_ptr<ReadView const> const& ledger);

    bool
    requestsPending() const;

    std::shared_ptr<RippleLineCache>
    getLineCache(
        std::shared_ptr<ReadView const> const& ledger,
        bool authoritative);

    /** Warm in-memory PayGraph for Dijkstra path_find (thread-safe).

        Full rebuild only when missing or empty (built before OrderBookDB
        finished scanning).  Per-ledger edge updates are applyLedgerDelta from
        updateAll — not a full rebuild every ~3s.
     */
    std::shared_ptr<PayGraph>
    ensurePayGraph(std::shared_ptr<ReadView const> const& inLedger);

    /** Convenience: same as ensurePayGraph (keeps PathRequest call sites). */
    std::shared_ptr<PayGraph>
    getPayGraph(std::shared_ptr<ReadView const> const& ledger)
    {
        return ensurePayGraph(ledger);
    }

    /** OrderBookDB finished a full scan and swapped allBooks_ in.
        Rebuild once from the scanned set so path_find is not stuck empty. */
    void
    signalOrderBookReady(std::shared_ptr<ReadView const> const& ledger);

    // Create a new-style path request that pushes
    // updates to a subscriber
    Json::Value
    makePathRequest(
        std::shared_ptr<InfoSub> const& subscriber,
        std::shared_ptr<ReadView const> const& ledger,
        Json::Value const& request);

    // Create an old-style path request that is
    // managed by a coroutine and updated by
    // the path engine
    Json::Value
    makeLegacyPathRequest(
        PathRequest::pointer& req,
        std::function<void(void)> completion,
        Resource::Consumer& consumer,
        std::shared_ptr<ReadView const> const& inLedger,
        Json::Value const& request);

    // Execute an old-style path request immediately
    // with the ledger specified by the caller
    Json::Value
    doLegacyPathRequest(
        Resource::Consumer& consumer,
        std::shared_ptr<ReadView const> const& inLedger,
        Json::Value const& request);

    void
    reportFast(std::chrono::milliseconds ms)
    {
        mFast.notify(ms);
    }

    void
    reportFull(std::chrono::milliseconds ms)
    {
        mFull.notify(ms);
    }

private:
    void
    insertPathRequest(PathRequest::pointer const&);

    Application& app_;
    beast::Journal mJournal;

    beast::insight::Event mFast;
    beast::insight::Event mFull;

    // Track all requests
    std::vector<PathRequest::wptr> requests_;

    // Use a RippleLineCache
    std::weak_ptr<RippleLineCache> lineCache_;

    // Persistent asset-exchange graph.  Built once (after orderBookReady_ or
    // standalone); mutated incrementally by applyLedgerDelta() each ledger.
    std::shared_ptr<PayGraph> payGraph_;
    std::uint32_t payGraphSeq_{0};

    // Set by signalOrderBookReady() when OrderBookDB finishes its first full
    // ledger scan.  Prevents building against an empty allBooks_ on networked
    // nodes where the scan is async.
    std::atomic<bool> orderBookReady_{false};

    std::atomic<int> mLastIdentifier;

    std::recursive_mutex mutable mLock;
};

}  // namespace ripple

#endif
