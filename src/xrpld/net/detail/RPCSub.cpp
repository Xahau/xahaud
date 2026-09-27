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

#include <xrpld/net/RPCCall.h>
#include <xrpld/net/RPCSub.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/contract.h>
#include <xrpl/json/to_string.h>
#include <deque>
#include <memory>

namespace ripple {

// Subscription object for JSON-RPC
class RPCSubImp : public RPCSub, public std::enable_shared_from_this<RPCSubImp>
{
public:
    RPCSubImp(
        InfoSub::Source& source,
        JobQueue& jobQueue,
        std::string const& strUrl,
        std::string const& strUsername,
        std::string const& strPassword,
        Logs& logs,
        std::size_t maxQueueSize)
        : RPCSub(source)
        , m_jobQueue(jobQueue)
        , mUrl(strUrl)
        , mSSL(false)
        , mUsername(strUsername)
        , mPassword(strPassword)
        , mSending(false)
        , maxQueueSize_(maxQueueSize)
        , j_(logs.journal("RPCSub"))
        , logs_(logs)
    {
        parsedURL pUrl;

        if (!parseUrl(pUrl, strUrl))
            Throw<std::runtime_error>("Failed to parse url.");
        else if (pUrl.scheme == "https")
            mSSL = true;
        else if (pUrl.scheme != "http")
            Throw<std::runtime_error>("Only http and https is supported.");

        mSeq = 1;

        mIp = pUrl.domain;
        mPort = (!pUrl.port) ? (mSSL ? 443 : 80) : *pUrl.port;
        mPath = pUrl.path;

        JLOG(j_.info()) << "RPCCall::fromNetwork sub: ip=" << mIp
                        << " port=" << mPort
                        << " ssl= " << (mSSL ? "yes" : "no") << " path='"
                        << mPath << "'";
    }

    ~RPCSubImp() = default;

    void
    send(Json::Value const& jvObj, bool broadcast) override
    {
        std::lock_guard sl(mLock);

        if (mDeque.size() >= maxQueueSize_)
        {
            // Always advance mSeq so consumers can detect the gap, but
            // rate-limit the log: a hopelessly behind endpoint drops on
            // every send() and would otherwise flood the log. Warn on
            // the first drop of a run and then once per dropLogInterval.
            if (mDropped++ % dropLogInterval == 0)
            {
                JLOG(j_.warn())
                    << "RPCCall::fromNetwork drop: queue full ("
                    << mDeque.size() << "), seq=" << mSeq
                    << ", endpoint=" << mIp << ", dropped=" << mDropped;
            }
            ++mSeq;
            return;
        }

        // Endpoint caught up enough to accept again; reset so the next
        // overflow burst logs its first drop immediately.
        mDropped = 0;

        auto jm = broadcast ? j_.debug() : j_.info();
        JLOG(jm) << "RPCCall::fromNetwork push: " << jvObj;

        mDeque.push_back(std::make_pair(mSeq++, jvObj));

        if (!mSending)
        {
            // Start a sending thread.
            JLOG(j_.info()) << "RPCCall::fromNetwork start";

            startSendingJob();
        }
    }

    void
    setUsername(std::string const& strUsername) override
    {
        std::lock_guard sl(mLock);

        mUsername = strUsername;
    }

    void
    setPassword(std::string const& strPassword) override
    {
        std::lock_guard sl(mLock);

        mPassword = strPassword;
    }

private:
    // Maximum concurrent HTTP deliveries per batch. Bounds file
    // descriptor usage while still allowing parallel delivery to
    // capable endpoints. With a 1024 FD process limit shared across
    // peers, clients, and the node store, 32 per subscriber is a
    // meaningful but survivable chunk even with multiple subscribers.
    static constexpr int maxInFlight = 32;

    // Log one drop warning per this many drops while the queue stays
    // full, to avoid flooding the log on a persistently behind endpoint.
    static constexpr std::size_t dropLogInterval = 1000;

    // Schedule a sending job. Must be called under mLock. The job holds a
    // weak_ptr and re-locks it on entry, so the RPCSub is kept alive for
    // the duration of the batch even if it is unsubscribed (and would
    // otherwise be destroyed) concurrently — sendThread dereferences this
    // only via that strong ref. mDeque events are delivered until the sub
    // is gone, after which weak.lock() fails and the job is a no-op.
    void
    startSendingJob()
    {
        std::weak_ptr<RPCSubImp> weak = weak_from_this();
        mSending = m_jobQueue.addJob(
            jtCLIENT_SUBSCRIBE, "RPCSub::sendThread", [weak]() {
                if (auto self = weak.lock())
                    self->sendThread();
            });
    }

    void
    sendThread()
    {
        // Process exactly ONE batch per job, then re-queue if more events
        // remain, rather than draining the whole backlog in a single job.
        // A local io_service's .run() blocks this worker thread for the
        // batch (up to the per-request timeout), so re-queueing between
        // batches keeps one slow/hung subscriber from monopolising a
        // job-queue worker and starving consensus/ledger/RPC work.
        //
        // mSending must be cleared under the lock on every non-requeue
        // exit path; if it ever stays set without a job in flight, send()
        // sees mSending == true and never restarts us, stalling the queue
        // forever — the original bug (xrpld issue #6341).
        boost::asio::io_service io_service;
        int dispatched = 0;

        try
        {
            {
                std::lock_guard sl(mLock);

                while (!mDeque.empty() && dispatched < maxInFlight)
                {
                    auto const [seq, env] = mDeque.front();
                    mDeque.pop_front();

                    Json::Value jvEvent = env;
                    jvEvent["seq"] = seq;

                    RPCCall::fromNetwork(
                        io_service,
                        mIp,
                        mPort,
                        mUsername,
                        mPassword,
                        mPath,
                        "event",
                        jvEvent,
                        mSSL,
                        true,
                        logs_);
                    ++dispatched;
                }
            }

            // dispatched is always > 0 here (send() only starts a job
            // after enqueuing, and the re-queue below only fires with a
            // non-empty deque), but guard anyway so an empty batch can't
            // log/spin — it falls straight through to clear mSending.
            if (dispatched > 0)
            {
                JLOG(j_.info()) << "RPCCall::fromNetwork: " << mIp
                                << " dispatching " << dispatched << " events";

                io_service.run();
            }
        }
        catch (std::exception const& e)
        {
            // Bail rather than re-queue: a persistently failing endpoint
            // would otherwise spin the job queue. mSending is reset so the
            // next send() restarts delivery.
            JLOG(j_.warn()) << "RPCSub::sendThread exception: " << e.what();
            std::lock_guard sl(mLock);
            mSending = false;
            return;
        }
        catch (...)
        {
            JLOG(j_.warn()) << "RPCSub::sendThread unknown exception";
            std::lock_guard sl(mLock);
            mSending = false;
            return;
        }

        // Batch complete: re-queue for the next one (mSending stays set)
        // or clear mSending if the queue drained — both under the lock to
        // avoid a lost-wakeup race with send().
        std::lock_guard sl(mLock);
        if (mDeque.empty())
            mSending = false;
        else
            startSendingJob();
    }

private:
    JobQueue& m_jobQueue;

    std::string mUrl;
    std::string mIp;
    std::uint16_t mPort;
    bool mSSL;
    std::string mUsername;
    std::string mPassword;
    std::string mPath;

    int mSeq;  // Next id to allocate.

    std::size_t mDropped = 0;  // Consecutive drops while queue is full.

    bool mSending;  // Sending threead is active.

    // Maximum queued events before dropping. The default (16384) is a
    // ~10-minute buffer at 100+ events/ledger; a hopelessly behind
    // endpoint trips it and consumers detect the gap via the seq field.
    std::size_t const maxQueueSize_;

    std::deque<std::pair<int, Json::Value>> mDeque;

    beast::Journal const j_;
    Logs& logs_;
};

//------------------------------------------------------------------------------

RPCSub::RPCSub(InfoSub::Source& source) : InfoSub(source, Consumer())
{
}

std::shared_ptr<RPCSub>
make_RPCSub(
    InfoSub::Source& source,
    JobQueue& jobQueue,
    std::string const& strUrl,
    std::string const& strUsername,
    std::string const& strPassword,
    Logs& logs,
    std::size_t maxQueueSize)
{
    return std::make_shared<RPCSubImp>(
        std::ref(source),
        std::ref(jobQueue),
        strUrl,
        strUsername,
        strPassword,
        logs,
        maxQueueSize);
}

}  // namespace ripple
