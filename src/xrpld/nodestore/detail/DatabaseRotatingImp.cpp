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

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/nodestore/detail/DatabaseRotatingImp.h>
#include <xrpl/protocol/HashPrefix.h>

#include <shared_mutex>

namespace ripple {
namespace NodeStore {

DatabaseRotatingImp::DatabaseRotatingImp(
    Application& app,
    Scheduler& scheduler,
    int readThreads,
    std::shared_ptr<Backend> writableBackend,
    std::shared_ptr<Backend> archiveBackend,
    Section const& config,
    beast::Journal j)
    : DatabaseRotating(scheduler, readThreads, config, j)
    , app_(app)
    , writableBackend_(std::move(writableBackend))
    , archiveBackend_(std::move(archiveBackend))
{
    if (writableBackend_)
        fdRequired_ += writableBackend_->fdRequired();
    if (archiveBackend_)
        fdRequired_ += archiveBackend_->fdRequired();
}

void
DatabaseRotatingImp::rotate(
    std::unique_ptr<NodeStore::Backend>&& newBackend,
    std::function<void(
        std::string const& writableName,
        std::string const& archiveName)> const& f)
{
    // Pass these two names to the callback function
    std::string const newWritableBackendName = newBackend->getName();
    std::string newArchiveBackendName;
    // Hold on to current archive backend pointer until after the
    // callback finishes. Only then will the archive directory be
    // deleted.
    std::shared_ptr<NodeStore::Backend> oldArchiveBackend;
    {
        std::unique_lock const lock(mutex_);

        // Do NOT call setDeletePath() inside this lock. For in-memory
        // backends, setDeletePath() calls close() which destructs the entire
        // table_ map while the lock is held and starves consensus reads.
        // Pinned history is preserved by DatabasePinned, not by copying
        // ledgers into the writable backend during rotation.
        oldArchiveBackend = std::move(archiveBackend_);

        // Complete the rotation
        archiveBackend_ = std::move(writableBackend_);
        newArchiveBackendName = archiveBackend_->getName();

        writableBackend_ = std::move(newBackend);
    }

    // Lock released — clear the old archive now without blocking fetches.
    oldArchiveBackend->setDeletePath();

    f(newWritableBackendName, newArchiveBackendName);
}

std::string
DatabaseRotatingImp::getName() const
{
    std::shared_lock const lock(mutex_);
    return writableBackend_->getName();
}

std::int32_t
DatabaseRotatingImp::getWriteLoad() const
{
    std::shared_lock const lock(mutex_);
    return writableBackend_->getWriteLoad();
}

void
DatabaseRotatingImp::importDatabase(Database& source)
{
    auto const backend = [&] {
        std::shared_lock const lock(mutex_);
        return writableBackend_;
    }();

    importInternal(*backend, source);
}

bool
DatabaseRotatingImp::storeLedger(std::shared_ptr<Ledger const> const& srcLedger)
{
    auto const backend = [&] {
        std::shared_lock const lock(mutex_);
        return writableBackend_;
    }();

    return Database::storeLedger(*srcLedger, backend);
}

void
DatabaseRotatingImp::sync()
{
    std::unique_lock const lock(mutex_);
    writableBackend_->sync();
}

void
DatabaseRotatingImp::store(
    NodeObjectType type,
    Blob&& data,
    uint256 const& hash,
    std::uint32_t)
{
    auto nObj = NodeObject::createObject(type, std::move(data), hash);

    auto const backend = [&] {
        std::shared_lock const lock(mutex_);
        return writableBackend_;
    }();

    backend->store(nObj);
    storeStats(1, nObj->getData().size());
}

void
DatabaseRotatingImp::sweep()
{
    // nothing to do
}

std::shared_ptr<NodeObject>
DatabaseRotatingImp::fetchNodeObject(
    uint256 const& hash,
    std::uint32_t,
    FetchReport& fetchReport,
    bool duplicate)
{
    auto fetch = [&](std::shared_ptr<Backend> const& backend) {
        Status status;
        std::shared_ptr<NodeObject> nodeObject;
        try
        {
            status = backend->fetch(hash.data(), &nodeObject);
        }
        catch (std::exception const& e)
        {
            JLOG(j_.fatal()) << "Exception, " << e.what();
            Rethrow();
        }

        switch (status)
        {
            case ok:
            case notFound:
                break;
            case dataCorrupt:
                JLOG(j_.fatal()) << "Corrupt NodeObject #" << hash;
                break;
            default:
                JLOG(j_.warn()) << "Unknown status=" << status;
                break;
        }

        return nodeObject;
    };

    // See if the node object exists in the cache
    std::shared_ptr<NodeObject> nodeObject;

    auto [writable, archive] = [&] {
        std::shared_lock const lock(mutex_);
        return std::make_pair(writableBackend_, archiveBackend_);
    }();

    // Try to fetch from the writable backend
    nodeObject = fetch(writable);
    if (!nodeObject)
    {
        // Otherwise try to fetch from the archive backend
        nodeObject = fetch(archive);
        if (nodeObject)
        {
            {
                // Refresh the writable backend pointer
                std::shared_lock const lock(mutex_);
                writable = writableBackend_;
            }

            // Promote every archive hit into the writable backend.
            // Ordinary reads pass duplicate=false; the rotation copy
            // pass passes true. Both must copy. The next rotation
            // deletes this archive, and copyNode only walks the state
            // tree, so an unpromoted header or transaction node
            // disappears while SQL still names it.
            (void)duplicate;
            writable->store(nodeObject);
        }
    }

    if (nodeObject)
        fetchReport.wasFound = true;

    return nodeObject;
}

void
DatabaseRotatingImp::for_each(
    std::function<void(std::shared_ptr<NodeObject>)> f)
{
    auto [writable, archive] = [&] {
        std::shared_lock const lock(mutex_);
        return std::make_pair(writableBackend_, archiveBackend_);
    }();

    // Iterate the writable backend
    writable->for_each(f);

    // Iterate the archive backend
    archive->for_each(f);
}

}  // namespace NodeStore
}  // namespace ripple
