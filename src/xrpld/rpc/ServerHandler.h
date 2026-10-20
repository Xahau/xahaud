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

#ifndef RIPPLE_RPC_SERVERHANDLER_H_INCLUDED
#define RIPPLE_RPC_SERVERHANDLER_H_INCLUDED

#include <xrpld/app/main/CollectorManager.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/rpc/RPCHandler.h>
#include <xrpld/rpc/detail/UDPInfoSub.h>
#include <xrpld/rpc/detail/WSInfoSub.h>
#include <xrpl/json/Output.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/server/Server.h>
#include <xrpl/server/Session.h>
#include <xrpl/server/WSSession.h>

#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/ssl/ssl_stream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/utility/string_view.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace ripple {

inline bool
operator<(Port const& lhs, Port const& rhs)
{
    return lhs.name < rhs.name;
}

/** [pwa_allowlist] and [pwa_blocklist]: every pwa port applies them. */
struct PWALists
{
    std::set<AccountID> allow;  // if non-empty, only these are served
    std::set<AccountID> block;  // never served; wins over allowlist

    bool
    operator==(PWALists const&) const = default;
};

class ServerHandler
{
public:
    /** How a pwa port answered, for the pwa_info RPC. */
    enum class PWAOutcome : std::uint8_t {
        served,            // 200
        notModified,       // 304
        redirected,        // 301
        notFound,          // 404: no such loader
        badTarget,         // 404: host and path name no document
        unknownDomain,     // 404: no usable _xahau-pwa record
        badHost,           // 400
        noForwardedFor,    // 400
        directConnection,  // 403: not from a secure_gateway address
        blocked,           // 403: allowlist / blocklist
        domainMismatch,    // 503: account's Domain does not name the host
        dnsPending,        // 429: lookup started or in flight, retry
        dnsFailed,         // 503: resolver failed
        upgradeRefused,    // 403
        badMethod,         // 405
        throttled,         // 503: resource manager
        busy,              // 503: too many in flight
        unavailable,       // 503: no validated ledger or lookup failed
        count_
    };

    struct PWAStats
    {
        std::array<std::atomic<std::uint64_t>, std::size_t(PWAOutcome::count_)>
            outcomes{};
        std::atomic<std::uint64_t> bytesServed{0};

        static char const*
        name(PWAOutcome o);

        Json::Value
        getJson(bool reset);
    };

    struct Setup
    {
        explicit Setup() = default;

        std::vector<Port> ports;

        // Memberspace
        struct client_t
        {
            explicit client_t() = default;

            bool secure = false;
            std::string ip;
            std::uint16_t port = 0;
            std::string user;
            std::string password;
            std::string admin_user;
            std::string admin_password;
        };

        // Configuration when acting in client role
        client_t client;

        // Configuration for the Overlay
        boost::asio::ip::tcp::endpoint overlay;

        // PWA lists as read at startup: those in xahaud.cfg are fixed, those
        // in pwaListsFile (empty: none) the pwa_list RPC edits.
        PWALists pwaCfgLists;
        PWALists pwaFileLists;
        boost::filesystem::path pwaListsFile;

        void
        makeContexts();
    };

private:
    using socket_type = boost::beast::tcp_stream;
    using stream_type = boost::beast::ssl_stream<socket_type>;

    Application& app_;
    Resource::Manager& m_resourceManager;
    beast::Journal m_journal;
    NetworkOPs& m_networkOPs;
    std::unique_ptr<Server> m_server;
    Setup setup_;
    Endpoints endpoints_;
    JobQueue& m_jobQueue;
    beast::insight::Counter rpc_requests_;
    beast::insight::Event rpc_size_;
    beast::insight::Event rpc_time_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_{false};
    std::map<std::reference_wrapper<Port const>, int> count_;
    PWAStats pwaStats_;

    // PWA lookups queued or running on the job queue. Requests past
    // maxPWAJobs are refused with 503 rather than queued.
    std::atomic<std::size_t> pwaJobs_{0};
    static constexpr std::size_t maxPWAJobs = 64;

    // Custom hosts: _xahau-pwa.<host> TXT -> account, looked up with
    // libresolv on jtCLIENT_PWADNS (it blocks) and cached: a record for its
    // TTL clamped to 60..3600 s, no record 15 min, a failure 60 s. At most
    // maxPWADnsInFlight lookups run at once, one per host, and a request
    // that misses the cache is answered 429 at once, so no flood of Host
    // headers can start more than a handful of queries or hold a thread.
    struct PWADns
    {
        enum class Status : std::uint8_t { found, none, failed };
        Status status = Status::failed;
        AccountID account;
        std::chrono::steady_clock::time_point expires;
    };
    std::mutex pwaDnsMutex_;
    std::map<std::string, PWADns> pwaDnsCache_;
    std::set<std::string> pwaDnsInFlight_;
    static constexpr std::size_t maxPWADnsInFlight = 4;
    static constexpr std::size_t maxPWADnsEntries = 10'000;

    // The effective PWA lists, cfg + file, replaced whole on each edit so a
    // request sees one consistent snapshot. pwaListsEditMutex_ serialises
    // edits (and so the file write); pwaListsMutex_ guards only the pointer.
    std::mutex pwaListsEditMutex_;
    PWALists pwaFileLists_;
    mutable std::mutex pwaListsMutex_;
    std::shared_ptr<PWALists const> pwaLists_ =
        std::make_shared<PWALists const>();

    void
    resolvePWADomain(
        std::string const& host,
        std::optional<beast::IP::Endpoint> const& resolver);

    // One document lookup, once the request has named an account.
    struct PWALookup
    {
        AccountID account;
        bool head = false;
        std::string ifNoneMatch;
        std::string domain;        // custom host it came in on, if any
        std::uint64_t index = 0;   // AccountIndex the host label named
        std::string redirectTail;  // non-empty: bare-zone 301 to pwa<index>
    };

    // A private type used to restrict access to the ServerHandler constructor.
    struct ServerHandlerCreator
    {
        explicit ServerHandlerCreator() = default;
    };

    // Friend declaration that allows make_ServerHandler to access the
    // private type that restricts access to the ServerHandler ctor.
    friend std::unique_ptr<ServerHandler>
    make_ServerHandler(
        Application& app,
        boost::asio::io_context&,
        JobQueue&,
        NetworkOPs&,
        Resource::Manager&,
        CollectorManager& cm);

public:
    // Must be public so make_unique can call it.
    ServerHandler(
        ServerHandlerCreator const&,
        Application& app,
        boost::asio::io_context& io_context,
        JobQueue& jobQueue,
        NetworkOPs& networkOPs,
        Resource::Manager& resourceManager,
        CollectorManager& cm);

    ~ServerHandler();

    using Output = Json::Output;

    void
    setup(Setup const& setup, beast::Journal journal);

    Setup const&
    setup() const
    {
        return setup_;
    }

    PWAStats&
    pwaStats()
    {
        return pwaStats_;
    }

    std::shared_ptr<PWALists const>
    pwaLists() const
    {
        std::lock_guard lock(pwaListsMutex_);
        return pwaLists_;
    }

    /** The pwa_list RPC: optionally reread the lists file, take `remove`
        off and then put `add` on its lists, write the file if that changed
        it, then apply the result. All or nothing. Returns the RPC result. */
    Json::Value
    pwaListsEdit(PWALists const& add, PWALists const& remove, bool reload);

    /** Drop every cached custom domain answer; returns how many. */
    std::size_t
    pwaDnsFlush()
    {
        std::lock_guard lock(pwaDnsMutex_);
        auto const n = pwaDnsCache_.size();
        pwaDnsCache_.clear();
        return n;
    }

    Endpoints const&
    endpoints() const
    {
        return endpoints_;
    }

    void
    stop();

    //
    // Handler
    //

    bool
    onAccept(Session& session, boost::asio::ip::tcp::endpoint endpoint);

    Handoff
    onHandoff(
        Session& session,
        std::unique_ptr<stream_type>&& bundle,
        http_request_type&& request,
        boost::asio::ip::tcp::endpoint const& remote_address);

    Handoff
    onHandoff(
        Session& session,
        http_request_type&& request,
        boost::asio::ip::tcp::endpoint const& remote_address)
    {
        return onHandoff(
            session,
            {},
            std::forward<http_request_type>(request),
            remote_address);
    }

    void
    onRequest(Session& session);

    void
    onWSMessage(
        std::shared_ptr<WSSession> session,
        std::vector<boost::asio::const_buffer> const& buffers);

    void
    onUDPMessage(
        std::string const& message,
        boost::asio::ip::tcp::endpoint const& remoteEndpoint,
        Port const& port,
        std::function<void(std::string const&)> sendResponse);

    void
    onClose(Session& session, boost::system::error_code const&);

    void
    onStopped(Server&);

private:
    Json::Value
    processSession(
        std::shared_ptr<WSSession> const& session,
        std::shared_ptr<JobQueue::Coro> const& coro,
        Json::Value const& jv);

    Json::Value
    processUDP(
        Json::Value const& jv,
        Role const& role,
        std::shared_ptr<JobQueue::Coro> const& coro,
        std::optional<std::function<void(std::string const&)>> sendResponse,
        boost::asio::ip::tcp::endpoint const& remoteEndpoint);

    void
    processSession(
        std::shared_ptr<Session> const&,
        std::shared_ptr<JobQueue::Coro> coro);

    /** Entry point for ports whose protocol is "pwa". Runs on the I/O
        thread: proxy and X-Forwarded-For checks, metering, routing of the
        Host header and target (document host, custom domain or bare-zone
        redirect), then
        queuePWALookup. */
    void
    onPWARequest(Session& session);

    /** Refuse what the PWA lists alone settle, then post the ledger read
        to the job queue as jtCLIENT_PWA. Callable from any thread. */
    void
    queuePWALookup(
        std::shared_ptr<Session> const& session,
        PWALookup const& lookup);

    /** Serve the document, or the bare-zone redirect, from the validated
        ledger. Runs on the job queue. Always closes the connection. */
    void
    processPWARequest(
        std::shared_ptr<Session> const& session,
        PWALookup const& lookup);

    void
    processRequest(
        Port const& port,
        std::string const& request,
        beast::IP::Endpoint const& remoteIPAddress,
        Output&&,
        std::shared_ptr<JobQueue::Coro> coro,
        std::string_view forwardedFor,
        std::string_view user);

    Handoff
    statusResponse(http_request_type const& request) const;
};

ServerHandler::Setup
setup_ServerHandler(Config const& c, std::ostream&& log);

class ReadView;

/** Whether a pwa port may serve `account`: the PWA lists, then (with a
    view and featurePWAHookLists) the port's hook list providers. */
bool
pwaPermitted(
    PWALists const& lists,
    Port const& port,
    ReadView const* view,
    AccountID const& account);

std::unique_ptr<ServerHandler>
make_ServerHandler(
    Application& app,
    boost::asio::io_context&,
    JobQueue&,
    NetworkOPs&,
    Resource::Manager&,
    CollectorManager& cm);

}  // namespace ripple

#endif
