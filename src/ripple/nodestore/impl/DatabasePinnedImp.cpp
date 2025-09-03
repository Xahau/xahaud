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

#include <ripple/nodestore/impl/DatabasePinnedImp.h>
#include <ripple/app/ledger/Ledger.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/nodestore/Manager.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/basics/Slice.h>

namespace ripple {
namespace NodeStore {

DatabasePinnedImp::DatabasePinnedImp(
    Application& app,
    Scheduler& scheduler,
    int readThreads,
    std::shared_ptr<Backend> memory,
    std::shared_ptr<Backend> persistent,
    Section const& config,
    beast::Journal j)
    : DatabaseRotating(scheduler, readThreads, config, j)
    , app_(app)
    , memory_(std::move(memory))
    , persistent_(std::move(persistent))
    , config_(config)
{
    if (memory_)
        fdRequired_ += memory_->fdRequired();
    if (persistent_)
        fdRequired_ += persistent_->fdRequired();
}

void DatabasePinnedImp::store(
    NodeObjectType type,
    Blob&& data,
    uint256 const& hash,
    std::uint32_t ledgerSeq)
{
    auto nObj = NodeObject::createObject(type, std::move(data), hash);
    
    // Critical routing logic with thread safety
    auto [memBackend, persBackend] = [&] {
        std::lock_guard lock(mutex_);
        return std::make_pair(memory_, persistent_);
    }();
    
    if (type == pinnedACCOUNT_NODE || 
        type == pinnedTRANSACTION_NODE || 
        type == pinnedLEDGER)
    {
        // Pinned types go ONLY to persistent storage
        persBackend->store(nObj);
    }
    else
    {
        // Hot types go to memory
        memBackend->store(nObj);
    }
    
    storeStats(1, nObj->getData().size());
}

std::shared_ptr<NodeObject>
DatabasePinnedImp::fetchNodeObject(
    uint256 const& hash,
    std::uint32_t,
    FetchReport& fetchReport,
    bool duplicate)
{
    auto fetch = [&](std::shared_ptr<Backend> const& backend) {
        std::shared_ptr<NodeObject> nodeObject;
        Status status;
        try
        {
            status = backend->fetch(hash.data(), &nodeObject);
        }
        catch (std::exception const& e)
        {
            JLOG(j_.fatal()) << "Exception, " << e.what();
            Rethrow();
        }
        
        if (status == ok)
            return nodeObject;
        return std::shared_ptr<NodeObject>{};
    };
    
    auto [memBackend, persBackend] = [&] {
        std::lock_guard lock(mutex_);
        return std::make_pair(memory_, persistent_);
    }();
    
    // Try memory first (hot data)
    if (auto obj = fetch(memBackend))
    {
        fetchReport.wasFound = true;
        return obj;
    }
    
    // Try persistent (pinned data)
    if (auto obj = fetch(persBackend))
    {
        fetchReport.wasFound = true;
        // Do NOT copy to memory - pinned data stays pinned
        return obj;
    }
    
    return nullptr;
}

void DatabasePinnedImp::rotateWithLock(
    std::function<std::unique_ptr<NodeStore::Backend>(
        std::string const& writableBackendName)> const& f)
{
    std::lock_guard lock(mutex_);
    
    JLOG(j_.info()) << "DatabasePinned rotating memory backend";
    
    // For DatabasePinned, we only rotate the memory backend
    // The persistent backend (for pinned data) never rotates
    
    // Create new memory backend
    auto newMemory = makeMemoryBackend();
    
    // Execute the callback to update state database
    // Pass the current memory backend name (even though we won't use the result)
    f(memory_->getName());
    
    // Copy current ledger to new memory backend
    auto currentLedger = app_.getLedgerMaster().getValidatedLedger();
    if (currentLedger)
    {
        std::uint64_t nodeCount = 0;
        
        // Copy state map nodes
        currentLedger->stateMap().snapShot(false)->visitNodes(
            [&](SHAMapTreeNode const& node) {
                // Get the node's data
                auto nodeData = currentLedger->stateMap().getNodeObject(
                    node.getNodeHash(), hotACCOUNT_NODE);
                if (nodeData)
                {
                    newMemory->store(nodeData);
                    ++nodeCount;
                }
                return true;
            });
        
        // Copy transaction map nodes
        currentLedger->txMap().snapShot(false)->visitNodes(
            [&](SHAMapTreeNode const& node) {
                auto nodeData = currentLedger->txMap().getNodeObject(
                    node.getNodeHash(), hotTRANSACTION_NODE);
                if (nodeData)
                {
                    newMemory->store(nodeData);
                    ++nodeCount;
                }
                return true;
            });
        
        // Store the current ledger header
        Serializer s(128);
        s.add32(HashPrefix::ledgerMaster);
        addRaw(currentLedger->info(), s);
        auto ledgerObj = NodeObject::createObject(
            hotLEDGER, std::move(s.modData()), currentLedger->info().hash);
        newMemory->store(ledgerObj);
        
        JLOG(j_.debug()) << "Copied " << nodeCount 
                         << " nodes to new memory backend";
    }
    
    // Swap the memory backend
    memory_->close();
    memory_ = newMemory;
    memory_->open();
    
    // Persistent backend remains unchanged - pinned data stays forever
}

std::shared_ptr<Backend> 
DatabasePinnedImp::makeMemoryBackend()
{
    Section memoryConfig = config_;
    return NodeStore::Manager::instance().make_Backend(
        memoryConfig, 
        scheduler_, 
        app_.logs().journal("NodeStore"));
}

std::string DatabasePinnedImp::getName() const
{
    std::lock_guard lock(mutex_);
    return "Pinned:" + memory_->getName() + "+" + persistent_->getName();
}

std::int32_t DatabasePinnedImp::getWriteLoad() const
{
    std::lock_guard lock(mutex_);
    return memory_->getWriteLoad() + persistent_->getWriteLoad();
}

void DatabasePinnedImp::importDatabase(Database& source)
{
    Throw<std::runtime_error>(
        "DatabasePinned does not support import operations");
}

bool DatabasePinnedImp::isSameDB(std::uint32_t, std::uint32_t)
{
    // All ledgers are in same logical database
    return true;
}

void DatabasePinnedImp::sync()
{
    std::lock_guard lock(mutex_);
    memory_->sync();
    persistent_->sync();
}

bool DatabasePinnedImp::storeLedger(
    std::shared_ptr<Ledger const> const& srcLedger)
{
    // Use Database base class implementation
    // This will call our store() method with appropriate types
    return Database::storeLedger(*srcLedger, persistent_);
}

void DatabasePinnedImp::sweep()
{
    // No cache to sweep
}

void DatabasePinnedImp::for_each(
    std::function<void(std::shared_ptr<NodeObject>)> f)
{
    std::lock_guard lock(mutex_);
    // Visit memory backend first
    memory_->for_each(f);
    // Then persistent backend
    persistent_->for_each(f);
}

}  // namespace NodeStore
}  // namespace ripple