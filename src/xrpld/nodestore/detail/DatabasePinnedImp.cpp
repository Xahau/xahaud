//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 Ripple Labs Inc.

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

#include <xrpld/app/ledger/Ledger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/nodestore/detail/DatabasePinnedImp.h>
#include <atomic>
#include <iostream>

namespace ripple {
namespace NodeStore {

DatabasePinnedImp::DatabasePinnedImp(
    Application& app,
    Scheduler& scheduler,
    int readThreads,
    std::shared_ptr<Backend> writableBackend,
    std::shared_ptr<Backend> archiveBackend,
    std::shared_ptr<Backend> persistent,
    Section const& config,
    beast::Journal j)
    : DatabaseRotating(scheduler, readThreads, config, j)
    , app_(app)
    , rotating_(
          app,
          scheduler,
          readThreads,
          std::move(writableBackend),
          std::move(archiveBackend),
          config,
          j)
    , persistent_(std::move(persistent))
{
    // Update fdRequired to include all backends
    fdRequired_ = rotating_.fdRequired();
    if (persistent_)
        fdRequired_ += persistent_->fdRequired();
}

void
DatabasePinnedImp::store(
    NodeObjectType type,
    Blob&& data,
    uint256 const& hash,
    std::uint32_t ledgerSeq)
{
    // Route based on type
    if (isPinnedType(type))
    {
        // Reduce to serializable hot type before storage
        type = toHotType(type);

        // Pinned types go to persistent storage
        auto count = ++pinnedStoreCount_;
        if (count % 1000 == 0)
        {
            JLOG(j_.trace())
                << "Pinned stores: " << count << " (type=" << type << ")";
        }

        auto nObj = NodeObject::createObject(type, std::move(data), hash);
        persistent_->store(nObj);
        storeStats(1, nObj->getData().size());
    }
    else
    {
        // Hot types go through rotating storage
        auto count = ++hotStoreCount_;
        if (count % 10000 == 0)
        {
            JLOG(j_.trace())
                << "Hot stores: " << count << " (type=" << type << ")";
        }
        rotating_.store(type, std::move(data), hash, ledgerSeq);
    }
}

void
DatabasePinnedImp::storePinnedBatch(Batch const& batch)
{
    if (batch.empty())
        return;

    std::uint64_t bytes = 0;
    for (auto const& obj : batch)
    {
        XRPL_ASSERT(
            obj && !isPinnedType(obj->getType()),
            "ripple::NodeStore::DatabasePinnedImp::storePinnedBatch : "
            "serializable type");
        bytes += obj->getData().size();
    }

    // The persistent backends allowed for pinned_type (NuDB, RocksDB, and
    // rwdb/memory in standalone) are safe to write from several threads,
    // so this may overlap a BatchWriter flush of single store() calls.
    persistent_->storeBatch(batch);
    pinnedStoreCount_ += batch.size();
    storeStats(batch.size(), bytes);
}

bool
DatabasePinnedImp::syncPinned()
{
    return persistent_->syncDurable();
}

std::shared_ptr<NodeObject>
DatabasePinnedImp::fetchNodeObject(
    uint256 const& hash,
    std::uint32_t ledgerSeq,
    FetchReport& fetchReport,
    bool duplicate)
{
    // Optimization: Use ledgerSeq to predict backend order
    // Most callers (13/18) provide reliable ledgerSeq: SHAMap node fetches,
    // ledger headers, tx metadata, ledger copying operations. For these cases,
    // checking the likely backend first reduces unnecessary lookups.
    // When ledgerSeq=0 (unknown), falls back to default order.
    // IMPORTANT: Always checks both backends for correctness.
    if (ledgerSeq != 0)
    {
        refreshPinnedRangesCache();

        if (likelyPinned(ledgerSeq))
        {
            // Try persistent backend first (pinned data)
            if (auto obj = tryPersistent(hash, fetchReport))
                return obj;

            // Fallback: try rotating backends
            return rotating_.fetchNodeObject(
                hash, ledgerSeq, fetchReport, duplicate);
        }
        else
        {
            // Not pinned: fall through to default path below
        }
    }

    // Default path: try rotating backends first (hot data)
    if (auto obj =
            rotating_.fetchNodeObject(hash, ledgerSeq, fetchReport, duplicate))
        return obj;

    // Fallback: try persistent backend (pinned data)
    auto obj = tryPersistent(hash, fetchReport);

    // `duplicate` is set by online_delete's copy of the current validated
    // state into the new writable backend. A hot ledger can share nodes
    // that exist only in persistent storage (e.g. after catalogue_load
    // switched the LCL to its last ledger); copy those too, so the hot
    // store becomes self-contained after one rotation and pinned history
    // can be unpinned and its store discarded without breaking live state.
    if (obj && duplicate)
    {
        Blob data(obj->getData());
        rotating_.store(obj->getType(), std::move(data), hash, ledgerSeq);
    }

    return obj;
}

void
DatabasePinnedImp::rotate(
    std::unique_ptr<NodeStore::Backend>&& newBackend,
    std::function<void(
        std::string const& writableName,
        std::string const& archiveName)> const& f)
{
    // Delegate to the inner rotating database. Pinned data lives in
    // persistent_ and is never touched by rotation — only hot data
    // in rotating_ participates in the rotate/archive/delete cycle.
    rotating_.rotate(std::move(newBackend), f);
}

std::string
DatabasePinnedImp::getName() const
{
    return "Pinned:" + rotating_.getName() + "+" + persistent_->getName();
}

std::int32_t
DatabasePinnedImp::getWriteLoad() const
{
    return rotating_.getWriteLoad() + persistent_->getWriteLoad();
}

void
DatabasePinnedImp::importDatabase(Database& source)
{
    // Import is not supported with DatabasePinned. The dual-backend
    // routing relies on knowing which ledger ranges are pinned, which
    // is only possible with catalogue packs where the ranges are known
    // quantities. A generic import would need to scan for ledger
    // headers and walk SHAMap trees to determine routing — not
    // impossible, but not currently implemented.
    Throw<std::runtime_error>(
        "--import is not supported when [node_db] pinned_type is configured. "
        "DatabasePinned uses separate rotating and persistent backends, and a "
        "generic node import cannot determine pinned ledger ranges or update "
        "state.db pinned range metadata. Use catalogue_load for pinned "
        "history import, or run --import with a non-pinned node store.");
}

bool
DatabasePinnedImp::isSameDB(std::uint32_t s1, std::uint32_t s2)
{
    // Used by async read threads to determine if a fetched object can
    // be reused for requests at different sequences. Since rotating and
    // persistent are both part of the same logical database (same hash
    // space), objects are always reusable regardless of which backend
    // they came from.
    return true;
}

void
DatabasePinnedImp::sync()
{
    rotating_.sync();
    persistent_->sync();
}

bool
DatabasePinnedImp::storeLedger(std::shared_ptr<Ledger const> const& srcLedger)
{
    // Store to persistent since ledger headers should be pinned
    return Database::storeLedger(*srcLedger, persistent_);
}

void
DatabasePinnedImp::sweep()
{
    // Delegate to rotating - persistent has no cache
    rotating_.sweep();
}

void
DatabasePinnedImp::for_each(std::function<void(std::shared_ptr<NodeObject>)> f)
{
    // Visit both rotating and persistent backends
    rotating_.for_each(f);
    persistent_->for_each(f);
}

void
DatabasePinnedImp::refreshPinnedRangesCache() const
{
    using namespace std::chrono;

    auto now = steady_clock::now();
    auto lastRefreshTime =
        steady_clock::time_point(steady_clock::duration(lastRefresh_.load()));

    // Only refresh if cache is stale (>1 second old)
    if (now - lastRefreshTime < seconds(1))
        return;

    // Create new RangeSet snapshot from LedgerMaster
    auto newRanges = std::make_shared<RangeSet<std::uint32_t>>(
        app_.getLedgerMaster().getPinnedLedgersRangeSet());

    // Atomic swap - all concurrent readers see old or new, never torn state
    std::atomic_store(&cachedPinnedRanges_, newRanges);
    lastRefresh_.store(now.time_since_epoch().count());
}

bool
DatabasePinnedImp::likelyPinned(std::uint32_t ledgerSeq) const
{
    auto ranges = std::atomic_load(&cachedPinnedRanges_);
    return ranges && boost::icl::contains(*ranges, ledgerSeq);
}

std::shared_ptr<NodeObject>
DatabasePinnedImp::tryPersistent(uint256 const& hash, FetchReport& fetchReport)
{
    std::shared_ptr<NodeObject> nodeObject;
    Status status;
    try
    {
        status = persistent_->fetch(hash.data(), &nodeObject);
    }
    catch (std::exception const& e)
    {
        JLOG(j_.fatal()) << "Exception fetching from persistent: " << e.what();
        Rethrow();
    }

    if (status == ok && nodeObject)
    {
        fetchReport.wasFound = true;
        // Copying into rotating storage for duplicate=true is handled by
        // the caller (see fetchNodeObject's default path).
    }
    return nodeObject;
}

}  // namespace NodeStore
}  // namespace ripple
