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
#include <xrpld/app/paths/PathRequests.h>
#include <xrpld/core/JobQueue.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>
#include <algorithm>
#include <future>

namespace ripple {

/** Get the current RippleLineCache, updating it if necessary.
    Get the correct ledger to use.
*/
std::shared_ptr<RippleLineCache>
PathRequests::getLineCache(
    std::shared_ptr<ReadView const> const& ledger,
    bool authoritative)
{
    std::lock_guard sl(mLock);

    auto lineCache = lineCache_.lock();

    std::uint32_t const lineSeq = lineCache ? lineCache->getLedger()->seq() : 0;
    std::uint32_t const lgrSeq = ledger->seq();
    JLOG(mJournal.debug()) << "getLineCache has cache for " << lineSeq
                           << ", considering " << lgrSeq;

    if ((lineSeq == 0) ||                         // no ledger
        (authoritative && (lgrSeq > lineSeq)) ||  // newer authoritative ledger
        (authoritative &&
         ((lgrSeq + 8) < lineSeq)) ||  // we jumped way back for some reason
        (lgrSeq > (lineSeq + 8)))      // we jumped way forward for some reason
    {
        JLOG(mJournal.debug())
            << "getLineCache creating new cache for " << lgrSeq;
        // Assign to the local before the member, because the member is a
        // weak_ptr, and will immediately discard it if there are no other
        // references.
        lineCache_ = lineCache = std::make_shared<RippleLineCache>(
            ledger, app_.journal("RippleLineCache"));
    }
    return lineCache;
}

void
PathRequests::updateAll(std::shared_ptr<ReadView const> const& inLedger)
{
    auto event =
        app_.getJobQueue().makeLoadEvent(jtPATH_FIND, "PathRequest::updateAll");

    std::vector<PathRequest::wptr> requests;
    std::shared_ptr<RippleLineCache> cache;

    // Get the ledger and cache we should be using
    {
        std::lock_guard sl(mLock);
        requests = requests_;
        cache = getLineCache(inLedger, true);
    }

    bool newRequests = app_.getLedgerMaster().isNewPathRequest();
    bool mustBreak = false;

    JLOG(mJournal.trace()) << "updateAll seq=" << cache->getLedger()->seq()
                           << ", " << requests.size() << " requests";

    int processed = 0, removed = 0;

    auto getSubscriber =
        [](PathRequest::pointer const& request) -> InfoSub::pointer {
        if (auto ipSub = request->getSubscriber();
            ipSub && ipSub->getRequest() == request)
        {
            return ipSub;
        }
        request->doAborting();
        return nullptr;
    };

    do
    {
        JLOG(mJournal.trace()) << "updateAll looping";

        struct WorkItem
        {
            PathRequest::pointer request;
            bool isOneShot;
        };
        std::vector<WorkItem> workItems;
        std::vector<PathRequest::pointer> toRemove;
        bool hasExpired = false;

        if (!app_.getJobQueue().isStopping())
        {
            for (auto const& wr : requests)
            {
                auto request = wr.lock();
                JLOG(mJournal.trace()) << "updateAll request "
                                       << (request ? "" : "not ") << "found";

                if (!request)
                {
                    hasExpired = true;
                    continue;
                }

                if (!request->needsUpdate(
                        newRequests, cache->getLedger()->seq()))
                    continue;

                if (auto ipSub = getSubscriber(request))
                {
                    if (!ipSub->getConsumer().warn())
                        workItems.push_back({request, false});
                    else
                        toRemove.push_back(request);
                }
                else if (request->hasCompletion())
                {
                    workItems.push_back({request, true});
                }
                else
                {
                    toRemove.push_back(request);
                }
            }
        }

        struct WorkResult
        {
            PathRequest::pointer request;
            Json::Value update;
            bool isOneShot;
        };

        // Use PATH_WORKERS config to cap parallel pathfinding threads.
        std::size_t const maxParallel = std::max(2, app_.config().PATH_WORKERS);

        JLOG(mJournal.trace()) << "updateAll processing " << workItems.size()
                               << " requests, parallelism=" << maxParallel;

        std::vector<WorkResult> allResults;
        allResults.reserve(workItems.size());

        for (std::size_t batchStart = 0; batchStart < workItems.size();
             batchStart += maxParallel)
        {
            std::size_t const batchEnd =
                std::min(batchStart + maxParallel, workItems.size());

            std::vector<std::future<WorkResult>> futures;
            futures.reserve(batchEnd - batchStart);

            for (std::size_t i = batchStart; i < batchEnd; ++i)
            {
                auto& item = workItems[i];
                futures.push_back(
                    std::async(
                        std::launch::async,
                        [req = item.request,
                         oneShot = item.isOneShot,
                         &cache,
                         &getSubscriber]() -> WorkResult {
                            std::function<bool(void)> cb;
                            if (!oneShot)
                            {
                                cb = [&getSubscriber, req]() {
                                    return (bool)getSubscriber(req);
                                };
                            }
                            Json::Value update =
                                req->doUpdate(cache, false, cb);
                            req->updateComplete();
                            return {req, std::move(update), oneShot};
                        }));
            }

            for (auto& f : futures)
                allResults.push_back(f.get());
        }

        for (auto& result : allResults)
        {
            if (result.isOneShot)
            {
                ++processed;
            }
            else
            {
                result.update[jss::type] = "path_find";
                if (auto ipSub = getSubscriber(result.request))
                {
                    ipSub->send(result.update, false);
                    ++processed;
                }
                else
                {
                    toRemove.push_back(result.request);
                }
            }
        }

        if (!toRemove.empty() || hasExpired)
        {
            std::lock_guard sl(mLock);

            auto ret = std::remove_if(
                requests_.begin(),
                requests_.end(),
                [&removed, &toRemove](auto const& wl) {
                    auto r = wl.lock();
                    if (!r)
                    {
                        ++removed;
                        return true;
                    }
                    if (std::find(toRemove.begin(), toRemove.end(), r) !=
                        toRemove.end())
                    {
                        ++removed;
                        return true;
                    }
                    return false;
                });

            requests_.erase(ret, requests_.end());
        }

        mustBreak = !newRequests && app_.getLedgerMaster().isNewPathRequest();

        if (mustBreak)
        {  // a new request came in while we were working
            newRequests = true;
        }
        else if (newRequests)
        {  // we only did new requests, so we always need a last pass
            newRequests = app_.getLedgerMaster().isNewPathRequest();
        }
        else
        {  // if there are no new requests, we are done
            newRequests = app_.getLedgerMaster().isNewPathRequest();
            if (!newRequests)
                break;
        }

        // Hold on to the line cache until after the lock is released, so it can
        // be destroyed outside of the lock
        std::shared_ptr<RippleLineCache> lastCache;
        {
            // Get the latest requests, cache, and ledger for next pass
            std::lock_guard sl(mLock);

            if (requests_.empty())
                break;
            requests = requests_;
            lastCache = cache;
            cache = getLineCache(cache->getLedger(), false);
        }
    } while (!app_.getJobQueue().isStopping());

    JLOG(mJournal.debug()) << "updateAll complete: " << processed
                           << " processed and " << removed << " removed";
}

bool
PathRequests::requestsPending() const
{
    std::lock_guard sl(mLock);
    return !requests_.empty();
}

void
PathRequests::insertPathRequest(PathRequest::pointer const& req)
{
    std::lock_guard sl(mLock);

    // Insert after any older unserviced requests but before
    // any serviced requests
    auto ret =
        std::find_if(requests_.begin(), requests_.end(), [](auto const& wl) {
            auto r = wl.lock();

            // We come before handled requests
            return r && !r->isNew();
        });

    requests_.emplace(ret, req);
}

// Make a new-style path_find request
Json::Value
PathRequests::makePathRequest(
    std::shared_ptr<InfoSub> const& subscriber,
    std::shared_ptr<ReadView const> const& inLedger,
    Json::Value const& requestJson)
{
    auto req = std::make_shared<PathRequest>(
        app_, subscriber, ++mLastIdentifier, *this, mJournal);

    auto [valid, jvRes] =
        req->doCreate(getLineCache(inLedger, false), requestJson);

    if (valid)
    {
        subscriber->setRequest(req);
        insertPathRequest(req);
        app_.getLedgerMaster().newPathRequest();
    }
    return std::move(jvRes);
}

// Make an old-style ripple_path_find request
Json::Value
PathRequests::makeLegacyPathRequest(
    PathRequest::pointer& req,
    std::function<void(void)> completion,
    Resource::Consumer& consumer,
    std::shared_ptr<ReadView const> const& inLedger,
    Json::Value const& request)
{
    // This assignment must take place before the
    // completion function is called
    req = std::make_shared<PathRequest>(
        app_, completion, consumer, ++mLastIdentifier, *this, mJournal);

    auto [valid, jvRes] = req->doCreate(getLineCache(inLedger, false), request);

    if (!valid)
    {
        req.reset();
    }
    else
    {
        insertPathRequest(req);
        if (!app_.getLedgerMaster().newPathRequest())
        {
            // The newPathRequest failed.  Tell the caller.
            jvRes = rpcError(rpcTOO_BUSY);
            req.reset();
        }
    }

    return std::move(jvRes);
}

Json::Value
PathRequests::doLegacyPathRequest(
    Resource::Consumer& consumer,
    std::shared_ptr<ReadView const> const& inLedger,
    Json::Value const& request)
{
    auto cache = std::make_shared<RippleLineCache>(
        inLedger, app_.journal("RippleLineCache"));

    auto req = std::make_shared<PathRequest>(
        app_, [] {}, consumer, ++mLastIdentifier, *this, mJournal);

    auto [valid, jvRes] = req->doCreate(cache, request);
    if (valid)
        jvRes = req->doUpdate(cache, false);
    return std::move(jvRes);
}

}  // namespace ripple
