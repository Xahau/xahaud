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

#ifndef RIPPLE_NODESTORE_DATABASEPINNEDIMP_H_INCLUDED
#define RIPPLE_NODESTORE_DATABASEPINNEDIMP_H_INCLUDED

#include <xrpld/app/main/Application.h>
#include <xrpld/nodestore/Backend.h>
#include <xrpld/nodestore/DatabaseRotating.h>
#include <xrpld/nodestore/NodeObject.h>
#include <xrpld/nodestore/detail/DatabaseRotatingImp.h>
#include <xrpl/basics/RangeSet.h>
#include <atomic>
#include <chrono>
#include <memory>

namespace ripple {
namespace NodeStore {

/**
 * DatabasePinned routes nodes to either rotating memory storage (via
 * DatabaseRotatingImp) or persistent storage (NuDB) based on NodeObjectType
 * values.
 *
 * Uses composition: DatabaseRotatingImp handles all rotation logic for hot
 * nodes, while this class adds a persistent layer for pinned nodes.
 *
 * Pinned types (pinnedACCOUNT_NODE, pinnedTRANSACTION_NODE, pinnedLEDGER) go to
 * persistent storage and stay forever. Hot types go through the normal
 * rotation.
 */
class DatabasePinnedImp : public DatabaseRotating
{
private:
    Application& app_;  // For accessing LedgerMaster's pinned ranges
    DatabaseRotatingImp rotating_;         // Handles rotation for hot nodes
    std::shared_ptr<Backend> persistent_;  // NuDB for pinned nodes

    // Debug counters for store routing
    std::atomic<uint64_t> pinnedStoreCount_{0};
    std::atomic<uint64_t> hotStoreCount_{0};

    // Lock-free cached pinned ranges for backend selection hint
    // Use std::atomic_load/store free functions for thread-safe access
    mutable std::shared_ptr<RangeSet<std::uint32_t>> cachedPinnedRanges_;
    mutable std::atomic<std::chrono::steady_clock::time_point::rep>
        lastRefresh_{0};

public:
    static constexpr auto JournalName = "DatabasePinned";

    DatabasePinnedImp(
        Application& app,
        Scheduler& scheduler,
        int readThreads,
        std::shared_ptr<Backend> writableBackend,
        std::shared_ptr<Backend> archiveBackend,
        std::shared_ptr<Backend> persistent,
        Section const& config,
        beast::Journal j);

    ~DatabasePinnedImp() override
    {
        stop();
    }

    // Total count of store() calls routed to the persistent backend
    // (pinned types). Exposed for tests and diagnostics.
    uint64_t
    pinnedStoreCount() const
    {
        return pinnedStoreCount_.load(std::memory_order_relaxed);
    }

    // Total count of store() calls routed to the rotating backend
    // (hot types). Exposed for tests and diagnostics.
    uint64_t
    hotStoreCount() const
    {
        return hotStoreCount_.load(std::memory_order_relaxed);
    }

    // DatabaseRotating interface - delegates to rotating_
    void
    rotate(
        std::unique_ptr<NodeStore::Backend>&& newBackend,
        std::function<void(
            std::string const& writableName,
            std::string const& archiveName)> const& f) override;

    // Database interface implementation
    std::string
    getName() const override;
    std::int32_t
    getWriteLoad() const override;
    void
    importDatabase(Database& source) override;
    bool
    isSameDB(std::uint32_t s1, std::uint32_t s2) override;
    void
    store(
        NodeObjectType type,
        Blob&& data,
        uint256 const& hash,
        std::uint32_t ledgerSeq) override;
    void
    sync() override;
    bool
    storeLedger(std::shared_ptr<Ledger const> const& srcLedger) override;
    void
    sweep() override;

private:
    std::shared_ptr<NodeObject>
    fetchNodeObject(
        uint256 const& hash,
        std::uint32_t ledgerSeq,
        FetchReport& fetchReport,
        bool duplicate) override;

    // Helper methods for backend selection optimization
    void
    refreshPinnedRangesCache() const;

    bool
    likelyPinned(std::uint32_t ledgerSeq) const;

    std::shared_ptr<NodeObject>
    tryPersistent(uint256 const& hash, FetchReport& fetchReport);

    void
    for_each(std::function<void(std::shared_ptr<NodeObject>)> f) override;
};

}  // namespace NodeStore
}  // namespace ripple

#endif