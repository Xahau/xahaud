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

namespace ripple {

// Subscription object for JSON-RPC
class RPCSubImp : public RPCSub
{
public:
    RPCSubImp(
        InfoSub::Source& source,
        JobQueue& jobQueue,
        std::string const& strUrl,
        std::string const& strUsername,
        std::string const& strPassword,
        Logs& logs)
        : RPCSub(source)
        , m_jobQueue(jobQueue)
        , mUrl(strUrl)
        , mSSL(false)
        , mUsername(strUsername)
        , mPassword(strPassword)
        , mSending(false)
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

        if (mDeque.size() >= maxQueueSize)
        {
            JLOG(j_.warn())
                << "RPCCall::fromNetwork drop: queue full (" << mDeque.size()
                << "), seq=" << mSeq << ", endpoint=" << mIp;
            ++mSeq;
            return;
        }

        auto jm = broadcast ? j_.debug() : j_.info();
        JLOG(jm) << "RPCCall::fromNetwork push: " << jvObj;

        mDeque.push_back(std::make_pair(mSeq++, jvObj));

        if (!mSending)
        {
            // Start a sending thread.
            JLOG(j_.info()) << "RPCCall::fromNetwork start";

            mSending = m_jobQueue.addJob(
                jtCLIENT_SUBSCRIBE, "RPCSub::sendThread", [this]() {
                    sendThread();
                });
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

    // Maximum queued events before dropping. At ~5-10KB per event
    // this is ~80-160MB worst case — trivial memory-wise. The real
    // purpose is detecting a hopelessly behind endpoint: at 100+
    // events per ledger (every ~4s), 16384 events is ~10 minutes
    // of buffer. Consumers detect gaps via the seq field.
    static constexpr std::size_t maxQueueSize = 16384;

    void
    sendThread()
    {
        bool bSend;

        do
        {
            // Local io_service per batch — cheap to create (just an
            // internal event queue, no threads, no syscalls). Using a
            // local io_service is what makes .run() block until exactly
            // this batch completes, giving us flow control. Same
            // pattern used by rpcClient() in RPCCall.cpp for CLI
            // commands.
            boost::asio::io_service io_service;
            int dispatched = 0;

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

                if (dispatched == 0)
                    mSending = false;
            }

            bSend = dispatched > 0;

            if (bSend)
            {
                try
                {
                    JLOG(j_.info())
                        << "RPCCall::fromNetwork: " << mIp << " dispatching "
                        << dispatched << " events";
                    io_service.run();
                }
                catch (const std::exception& e)
                {
                    JLOG(j_.warn())
                        << "RPCCall::fromNetwork exception: " << e.what();
                }
                catch (...)
                {
                    JLOG(j_.warn()) << "RPCCall::fromNetwork unknown exception";
                }
            }
        } while (bSend);
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

    bool mSending;  // Sending threead is active.

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
    Logs& logs)
{
    return std::make_shared<RPCSubImp>(
        std::ref(source),
        std::ref(jobQueue),
        strUrl,
        strUsername,
        strPassword,
        logs);
}

}  // namespace ripple
