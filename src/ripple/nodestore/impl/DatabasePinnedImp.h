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

#include <ripple/app/main/Application.h>
#include <ripple/nodestore/DatabaseRotating.h>
#include <ripple/nodestore/Backend.h>
#include <ripple/nodestore/NodeObject.h>
#include <memory>
#include <mutex>

namespace ripple {
namespace NodeStore {

/**
 * DatabasePinned routes nodes to either memory (RWDB) or persistent storage
 * (NuDB) based on NodeObjectType values. Pinned types go to persistent
 * storage and stay forever, while hot types go to memory and get rotated.
 * 
 * This implementation inherits from DatabaseRotating to integrate with
 * SHAMapStoreImp's rotation infrastructure.
 */
class DatabasePinnedImp : public DatabaseRotating
{
private:
    Application& app_;
    std::shared_ptr<Backend> memory_;      // RWDB for hot nodes
    std::shared_ptr<Backend> persistent_;  // NuDB for pinned nodes
    Section const config_;                 // Store config for recreating memory backend
    mutable std::mutex mutex_;
    
public:
    DatabasePinnedImp(
        Application& app,
        Scheduler& scheduler,
        int readThreads,
        std::shared_ptr<Backend> memory,
        std::shared_ptr<Backend> persistent,
        Section const& config,
        beast::Journal j);
    
    ~DatabasePinnedImp() override
    {
        stop();
    }
    
    // DatabaseRotating interface - REQUIRED for SHAMapStoreImp integration
    void rotateWithLock(
        std::function<std::unique_ptr<NodeStore::Backend>(
            std::string const& writableBackendName)> const& f) override;
    
    // Database interface implementation
    std::string getName() const override;
    std::int32_t getWriteLoad() const override;
    void importDatabase(Database& source) override;
    bool isSameDB(std::uint32_t s1, std::uint32_t s2) override;
    void store(NodeObjectType type, Blob&& data, 
               uint256 const& hash, std::uint32_t ledgerSeq) override;
    void sync() override;
    bool storeLedger(std::shared_ptr<Ledger const> const& srcLedger) override;
    void sweep() override;
    
private:
    std::shared_ptr<NodeObject> fetchNodeObject(
        uint256 const& hash,
        std::uint32_t ledgerSeq,
        FetchReport& fetchReport,
        bool duplicate) override;
    
    void for_each(std::function<void(std::shared_ptr<NodeObject>)> f) override;
    
    std::shared_ptr<Backend> makeMemoryBackend();
};

}  // namespace NodeStore
}  // namespace ripple

#endif