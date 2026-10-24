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

#include <xrpld/rpc/ServerHandler.h>

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/NetworkOPs.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/overlay/Overlay.h>
#include <xrpld/rpc/RPCHandler.h>
#include <xrpld/rpc/Role.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/Tuning.h>
#include <xrpld/rpc/json_body.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/base64.h>
#include <xrpl/basics/contract.h>
#include <xrpl/basics/make_SSLContext.h>
#include <xrpl/beast/core/LexicalCast.h>
#include <xrpl/beast/net/IPAddressConversion.h>
#include <xrpl/beast/rfc2616.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/resource/Fees.h>
#include <xrpl/resource/ResourceManager.h>
#include <xrpl/server/Server.h>
#include <xrpl/server/SimpleWriter.h>
#include <xrpl/server/detail/JSONRPCUtil.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/beast/core/string.hpp>
#include <boost/beast/http/fields.hpp>
#include <boost/beast/http/string_body.hpp>

#include <arpa/inet.h>
#include <arpa/nameser.h>
#include <netinet/in.h>
#include <resolv.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ripple {

static bool
isStatusRequest(http_request_type const& request)
{
    return request.version() >= 11 && request.target() == "/" &&
        request.body().size() == 0 &&
        request.method() == boost::beast::http::verb::get;
}

static Handoff
statusRequestResponse(
    http_request_type const& request,
    boost::beast::http::status status)
{
    using namespace boost::beast::http;
    Handoff handoff;
    response<string_body> msg;
    msg.version(request.version());
    msg.result(status);
    msg.insert("Server", BuildInfo::getFullVersionString());
    msg.insert("Content-Type", "text/html");
    msg.insert("Connection", "close");
    msg.body() = "Invalid protocol.";
    msg.prepare_payload();
    handoff.response = std::make_shared<SimpleWriter>(msg);
    return handoff;
}

// VFALCO TODO Rewrite to use boost::beast::http::fields
static bool
authorized(Port const& port, std::map<std::string, std::string> const& h)
{
    if (port.user.empty() || port.password.empty())
        return true;

    auto const it = h.find("authorization");
    if ((it == h.end()) || (it->second.substr(0, 6) != "Basic "))
        return false;
    std::string strUserPass64 = it->second.substr(6);
    boost::trim(strUserPass64);
    std::string strUserPass = base64_decode(strUserPass64);
    std::string::size_type nColon = strUserPass.find(":");
    if (nColon == std::string::npos)
        return false;
    std::string strUser = strUserPass.substr(0, nColon);
    std::string strPassword = strUserPass.substr(nColon + 1);
    return strUser == port.user && strPassword == port.password;
}

ServerHandler::ServerHandler(
    ServerHandlerCreator const&,
    Application& app,
    boost::asio::io_context& io_context,
    JobQueue& jobQueue,
    NetworkOPs& networkOPs,
    Resource::Manager& resourceManager,
    CollectorManager& cm)
    : app_(app)
    , m_resourceManager(resourceManager)
    , m_journal(app_.journal("Server"))
    , m_networkOPs(networkOPs)
    , m_server(make_Server(*this, io_context, app_.journal("Server")))
    , m_jobQueue(jobQueue)
{
    auto const& group(cm.group("rpc"));
    rpc_requests_ = group->make_counter("requests");
    rpc_size_ = group->make_event("size");
    rpc_time_ = group->make_event("time");
}

ServerHandler::~ServerHandler()
{
    m_server = nullptr;
}

void
ServerHandler::setup(Setup const& setup, beast::Journal journal)
{
    setup_ = setup;
    endpoints_ = m_server->ports(setup.ports);

    // fix auto ports
    for (auto& port : setup_.ports)
    {
        if (auto it = endpoints_.find(port.name); it != endpoints_.end())
        {
            auto const endpointPort = it->second.port();
            if (!port.port)
                port.port = endpointPort;

            if (!setup_.client.port &&
                (port.protocol.count("http") > 0 ||
                 port.protocol.count("https") > 0))
                setup_.client.port = endpointPort;

            if (!setup_.overlay.port() && (port.protocol.count("peer") > 0))
                setup_.overlay.port(endpointPort);
        }
    }
}

//------------------------------------------------------------------------------

void
ServerHandler::stop()
{
    m_server->close();
    {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopped_; });
    }
}

//------------------------------------------------------------------------------

bool
ServerHandler::onAccept(
    Session& session,
    boost::asio::ip::tcp::endpoint endpoint)
{
    auto const& port = session.port();

    auto const c = [this, &port]() {
        std::lock_guard lock(mutex_);
        return ++count_[port];
    }();

    if (port.limit && c >= port.limit)
    {
        JLOG(m_journal.trace())
            << port.name << " is full; dropping " << endpoint;
        return false;
    }

    return true;
}

Handoff
ServerHandler::onHandoff(
    Session& session,
    std::unique_ptr<stream_type>&& bundle,
    http_request_type&& request,
    boost::asio::ip::tcp::endpoint const& remote_address)
{
    using namespace boost::beast;
    auto const& p{session.port().protocol};
    bool const is_ws{
        p.count("ws") > 0 || p.count("ws2") > 0 || p.count("wss") > 0 ||
        p.count("wss2") > 0};

    // PWA ports: no websockets, no peers, no status page. Every request,
    // upgrade requests included, goes to onRequest and from there to
    // onPWARequest, so that it meets the proxy rules, is metered and counted,
    // and is answered without the full version string statusRequestResponse
    // would put in the Server header.
    if (p.count("pwa") > 0)
        return {};

    if (websocket::is_upgrade(request))
    {
        if (!is_ws)
            return statusRequestResponse(request, http::status::unauthorized);

        std::shared_ptr<WSSession> ws;
        try
        {
            ws = session.websocketUpgrade();
        }
        catch (std::exception const& e)
        {
            JLOG(m_journal.error())
                << "Exception upgrading websocket: " << e.what() << "\n";
            return statusRequestResponse(
                request, http::status::internal_server_error);
        }

        auto is{std::make_shared<WSInfoSub>(m_networkOPs, ws)};
        auto const beast_remote_address =
            beast::IPAddressConversion::from_asio(remote_address);
        is->getConsumer() = requestInboundEndpoint(
            m_resourceManager,
            beast_remote_address,
            requestRole(
                Role::GUEST,
                session.port(),
                Json::Value(),
                beast_remote_address,
                is->user()),
            is->user(),
            is->forwarded_for());
        ws->appDefined = std::move(is);
        ws->run();

        Handoff handoff;
        handoff.moved = true;
        return handoff;
    }

    if (bundle && p.count("peer") > 0)
        return app_.overlay().onHandoff(
            std::move(bundle), std::move(request), remote_address);

    if (is_ws && isStatusRequest(request))
        return statusResponse(request);

    // Otherwise pass to legacy onRequest or websocket
    return {};
}

static inline Json::Output
makeOutput(Session& session)
{
    return [&](boost::beast::string_view const& b) {
        session.write(b.data(), b.size());
    };
}

static std::map<std::string, std::string>
build_map(boost::beast::http::fields const& h)
{
    std::map<std::string, std::string> c;
    for (auto const& e : h)
    {
        // key cannot be a std::string_view because it needs to be used in
        // map and along with iterators
        std::string key(e.name_string());
        std::transform(key.begin(), key.end(), key.begin(), [](auto kc) {
            return std::tolower(static_cast<unsigned char>(kc));
        });
        c[key] = std::string(e.value());
    }
    return c;
}

template <class ConstBufferSequence>
static std::string
buffers_to_string(ConstBufferSequence const& bs)
{
    using boost::asio::buffer_size;
    std::string s;
    s.reserve(buffer_size(bs));
    // Use auto&& so the right thing happens whether bs returns a copy or
    // a reference
    for (auto&& b : bs)
        s.append(static_cast<char const*>(b.data()), buffer_size(b));
    return s;
}

//------------------------------------------------------------------------------
//
// PWA ports (protocol = pwa): serve accounts' on-ledger AppLoader documents
// as HTML, one host per account, behind a reverse proxy only. See the "pwa
// ports" section of xahaud-example.cfg.
//
//------------------------------------------------------------------------------

namespace {

using Outcome = ServerHandler::PWAOutcome;

// The client a secure_gateway proxy vouches for: the RIGHTMOST entry of the
// (last) X-Forwarded-For field, the only one the proxy itself wrote. Not
// forwardedFor(), which takes the leftmost entry and trusts a client-supplied
// Forwarded header. IPv4-mapped IPv6 is returned as IPv4.
std::optional<boost::asio::ip::address>
pwaClientAddress(http_request_type const& request)
{
    std::string_view value;
    bool found = false;
    for (auto const& field : request)
        if (boost::beast::iequals(field.name_string(), "X-Forwarded-For"))
        {
            value = {field.value().data(), field.value().size()};
            found = true;
        }
    if (!found)
        return std::nullopt;
    if (auto const comma = value.rfind(','); comma != std::string_view::npos)
        value.remove_prefix(comma + 1);
    auto const isSpace = [](char c) { return c == ' ' || c == '\t'; };
    while (!value.empty() && isSpace(value.front()))
        value.remove_prefix(1);
    while (!value.empty() && isSpace(value.back()))
        value.remove_suffix(1);
    if (value.empty())
        return std::nullopt;

    boost::system::error_code ec;
    auto addr = boost::asio::ip::make_address(std::string(value), ec);
    if (ec)
        return std::nullopt;
    if (addr.is_v6() && addr.to_v6().is_v4_mapped())
        addr = boost::asio::ip::make_address_v4(
            boost::asio::ip::v4_mapped, addr.to_v6());
    if (addr.is_unspecified())
        return std::nullopt;
    return addr;
}

// Bill IPv4 per address and IPv6 per /64: a subscriber usually holds a /64.
boost::asio::ip::address
pwaBillingAddress(boost::asio::ip::address const& client)
{
    if (!client.is_v6())
        return client;
    auto bytes = client.to_v6().to_bytes();
    std::fill(bytes.begin() + 8, bytes.end(), 0);
    return boost::asio::ip::address_v6(bytes);
}

// What a request's Host header and target name. Every account has a host of
// its own so that loaders never share an origin with each other or with the
// operator's site:
//
//   pwa<account-index>.<zone>/<r-address>    the label is the AccountIndex
//                                            (decimal) of the account the
//                                            path names
//   <zone>/<r-address>                       301 to the above, keeping :port
//                                            and query, scheme-relative
//   <custom host>/                           with custom_domains: served as
//                                            the account its _xahau-pwa TXT
//                                            names, if that account's Domain
//                                            names the host (or a parent)
//
// One trailing slash and a query string are ignored. Anything else is 404.
struct PWARoute
{
    enum class Kind { account, domain, redirect, badHost, badTarget };
    Kind kind = Kind::badTarget;
    AccountID account;
    std::uint64_t index = 0;   // account: what the label said
    std::string redirectTail;  // redirect: Location after "//pwa<index>"
    std::string domain;        // domain: the normalised Host, no port
};

PWARoute
parsePWATarget(Port const& port, http_request_type const& request)
{
    using Kind = PWARoute::Kind;
    PWARoute route;
    auto badHost = [&] {
        route.kind = Kind::badHost;
        return route;
    };

    // Exactly one Host, no IP literal; an optional :port (digits, since it
    // is echoed into a redirect) and trailing dot are dropped.
    auto const range = request.equal_range(boost::beast::http::field::host);
    if (range.first == request.end() || std::next(range.first) != range.second)
        return badHost();
    std::string host(range.first->value().data(), range.first->value().size());
    if (host.empty() || host.front() == '[')
        return badHost();
    std::string hostPort;
    if (auto const colon = host.find(':'); colon != std::string::npos)
    {
        hostPort = host.substr(colon);
        host.erase(colon);
        if (hostPort.size() < 2 || hostPort.size() > 6 ||
            hostPort.find_first_not_of("0123456789", 1) != std::string::npos)
            return badHost();
    }
    boost::algorithm::to_lower(host);
    if (!host.empty() && host.back() == '.')
        host.pop_back();
    if (host.empty() ||
        host.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") !=
            std::string::npos)
        return badHost();

    // Path less query (kept for a redirect; beast has refused any target
    // with control characters) and one trailing slash.
    auto const target = request.target();
    if (target.empty() || target.front() != '/')
        return route;
    std::string path{target.substr(1)};
    std::string query;
    if (auto const cut = path.find_first_of("?#"); cut != std::string::npos)
    {
        if (path[cut] == '?')
            query = path.substr(cut, path.find('#', cut) - cut);
        path.erase(cut);
    }
    if (!path.empty() && path.back() == '/')
        path.pop_back();
    if (path.find('/') != std::string::npos)
        return route;

    // Anything under one of our zones is a wrong URL, never a custom domain.
    bool const ours = port.pwa_domains.count(host) > 0 ||
        std::any_of(port.pwa_domains.begin(),
                    port.pwa_domains.end(),
                    [&](auto const& z) {
                        return boost::algorithm::ends_with(host, "." + z);
                    });
    if (!ours)
    {
        if (port.pwa_custom_domains && path.empty())
        {
            route.kind = Kind::domain;
            route.domain = std::move(host);
        }
        return route;
    }

    auto const account = parseBase58<AccountID>(path);
    if (!account || account->isZero())
        return route;
    route.account = *account;

    if (port.pwa_domains.count(host) > 0)
    {
        // Scheme-relative: the browser keeps the scheme it used. The index
        // comes from the ledger later.
        route.kind = Kind::redirect;
        route.redirectTail = "." + host + hostPort + "/" + path + query;
        return route;
    }

    // pwa<decimal AccountIndex>.<zone>; whether it is the path's account's
    // index is checked against the ledger later.
    auto const dot = host.find('.');
    std::string label;
    if (dot != std::string::npos && dot > 3 && host.compare(0, 3, "pwa") == 0)
        label = host.substr(3, dot - 3);
    if (!label.empty() && port.pwa_domains.count(host.substr(dot + 1)) > 0 &&
        label.size() <= 20 && (label == "0" || label.front() != '0') &&
        label.find_first_not_of("0123456789") == std::string::npos &&
        beast::lexicalCastChecked(route.index, label))
        route.kind = Kind::account;
    return route;
}

// Does an account's Domain field name `host`, or a parent of it?
bool
pwaEndorses(std::shared_ptr<SLE const> const& root, std::string const& host)
{
    if (!root || !root->isFieldPresent(sfDomain))
        return false;
    auto const d = root->getFieldVL(sfDomain);
    auto const endorsed =
        boost::algorithm::to_lower_copy(std::string(d.begin(), d.end()));
    return !endorsed.empty() &&
        (host == endorsed || boost::algorithm::ends_with(host, "." + endorsed));
}

std::string
pwaReason(int status)
{
    return std::string(boost::beast::http::obsolete_reason(
        static_cast<boost::beast::http::status>(status)));
}

// A complete HTTP/1.1 response, then close: one request per connection.
// HEAD gets the GET's headers, Content-Length included, and no body.
void
pwaReply(
    Session& session,
    int status,
    bool head,
    std::vector<std::string> const& headers = {},
    std::string const& body = {})
{
    std::string out =
        "HTTP/1.1 " + std::to_string(status) + " " + pwaReason(status) + "\r\n";
    out += "Server: " + std::string(systemName) +
        "\r\nConnection: close\r\nX-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n";
    for (auto const& h : headers)
        out += h + "\r\n";
    if (status != 304)  // a 304 has no body and no Content-Length
    {
        if (status != 200 &&
            std::none_of(headers.begin(), headers.end(), [](auto const& h) {
                return boost::algorithm::istarts_with(h, "Content-Type:");
            }))
            out += "Content-Type: text/plain; charset=utf-8\r\n";
        if (status != 200 && status != 301)
            out += "Cache-Control: no-store\r\n";
        out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    out += "\r\n";
    if (status != 304 && !head)
        out += body;
    session.write(out);
    session.close(true);
}

void
pwaError(
    Session& session,
    int status,
    bool head,
    std::vector<std::string> const& headers = {})
{
    pwaReply(
        session, status, head, headers, std::string(pwaReason(status)) + "\n");
}

// Releases a ServerHandler PWA job slot when the last copy of the job
// closure dies, whether or not it ever ran.
// Non-copyable: a copy would release the slot twice.
struct PWAJobSlot
{
    std::atomic<std::size_t>& count;

    explicit PWAJobSlot(std::atomic<std::size_t>& c) : count(c)
    {
    }
    PWAJobSlot(PWAJobSlot const&) = delete;
    PWAJobSlot&
    operator=(PWAJobSlot const&) = delete;

    ~PWAJobSlot()
    {
        count.fetch_sub(1, std::memory_order_relaxed);
    }
};

}  // namespace

char const*
ServerHandler::PWAStats::name(PWAOutcome o)
{
    static constexpr char const* names[] = {
        "served",
        "not_modified",
        "redirected",
        "not_found",
        "bad_target",
        "unknown_domain",
        "bad_host",
        "no_forwarded_for",
        "direct_connection",
        "blocked",
        "domain_mismatch",
        "dns_pending",
        "dns_failed",
        "upgrade_refused",
        "bad_method",
        "throttled",
        "busy",
        "unavailable"};
    static_assert(std::size(names) == std::size_t(PWAOutcome::count_));
    return names[std::size_t(o)];
}

Json::Value
ServerHandler::PWAStats::getJson(bool reset)
{
    Json::Value ret(Json::objectValue);
    Json::Value& o = ret["outcomes"] = Json::objectValue;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < outcomes.size(); ++i)
    {
        auto const n = reset ? outcomes[i].exchange(0) : outcomes[i].load();
        total += n;
        o[name(PWAOutcome(i))] = std::to_string(n);
    }
    ret["requests"] = std::to_string(total);
    ret["bytes_served"] =
        std::to_string(reset ? bytesServed.exchange(0) : bytesServed.load());
    return ret;
}

void
ServerHandler::onPWARequest(Session& session)
{
    using boost::beast::http::verb;
    auto const& port = session.port();
    auto const& request = session.request();
    auto const remote = session.remoteAddress().at_port(0);
    bool const head = request.method() == verb::head;
    auto count = [&](Outcome o) { ++pwaStats_.outcomes[std::size_t(o)]; };

    // Only the configured proxy, and it must say who it forwards for.
    if (!ipAllowed(
            remote.address(),
            port.secure_gateway_nets_v4,
            port.secure_gateway_nets_v6))
    {
        count(Outcome::directConnection);
        return pwaError(session, 403, head);
    }
    auto const forwarded = pwaClientAddress(request);
    if (!forwarded)
    {
        count(Outcome::noForwardedFor);
        return pwaError(session, 400, head);
    }

    // Meter the client, never the proxy.
    auto usage = m_resourceManager.newInboundEndpoint(
        beast::IPAddressConversion::from_asio(pwaBillingAddress(*forwarded)));
    if (usage.disconnect(m_journal))
    {
        count(Outcome::throttled);
        return pwaError(session, 503, head);
    }
    usage.charge(Resource::feeReferenceRPC);

    if (request.method() != verb::get && !head)
    {
        usage.charge(Resource::feeMalformedRPC);
        count(Outcome::badMethod);
        return pwaError(session, 405, head, {"Allow: GET, HEAD"});
    }
    if (boost::beast::websocket::is_upgrade(request))
    {
        usage.charge(Resource::feeMalformedRPC);
        count(Outcome::upgradeRefused);
        return pwaError(session, 403, head);
    }

    auto const route = parsePWATarget(port, request);
    switch (route.kind)
    {
        case PWARoute::Kind::badHost:
            usage.charge(Resource::feeMalformedRPC);
            count(Outcome::badHost);
            return pwaError(session, 400, head);
        case PWARoute::Kind::badTarget:
            usage.charge(Resource::feeMalformedRPC);
            count(Outcome::badTarget);
            return pwaError(session, 404, head);
        case PWARoute::Kind::redirect:
        case PWARoute::Kind::account:
        case PWARoute::Kind::domain:
            break;
    }

    // A miss costs what a hit does, so sweeping is no cheaper than reading.
    usage.charge(Resource::feeMediumBurdenRPC);

    PWALookup lookup;
    lookup.account = route.account;
    lookup.head = head;
    lookup.domain = route.domain;
    lookup.index = route.index;
    lookup.redirectTail = route.redirectTail;
    if (auto const it = request.find(boost::beast::http::field::if_none_match);
        it != request.end())
        lookup.ifNoneMatch =
            std::string(it->value().data(), it->value().size());

    if (route.kind == PWARoute::Kind::domain)
    {
        // The cache answers now. A miss starts one lookup (if fewer than
        // maxPWADnsInFlight are running and none for this host) and is
        // answered 429 with a refresh, so no request ever waits on DNS.
        std::optional<PWADns> dns;
        bool start = false;
        {
            std::lock_guard lock(pwaDnsMutex_);
            auto const it = pwaDnsCache_.find(route.domain);
            if (it != pwaDnsCache_.end() &&
                it->second.expires > std::chrono::steady_clock::now())
                dns = it->second;
            else if (
                pwaDnsInFlight_.size() < maxPWADnsInFlight &&
                pwaDnsInFlight_.insert(route.domain).second)
                start = true;
        }
        if (!dns)
        {
            usage.charge(Resource::feeMediumBurdenRPC);
            if (start &&
                !m_jobQueue.addJob(
                    jtCLIENT_PWADNS,
                    "PWA-DNS",
                    [this, host = route.domain, resolver = port.pwa_resolver] {
                        resolvePWADomain(host, resolver);
                    }))
            {
                std::lock_guard lock(pwaDnsMutex_);
                pwaDnsInFlight_.erase(route.domain);
            }
            count(Outcome::dnsPending);
            return pwaReply(
                session,
                429,
                head,
                {"Retry-After: 2", "Content-Type: text/html; charset=utf-8"},
                "<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" "
                "content=\"2\"></head><body>Looking up this host; retrying."
                "</body></html>\n");
        }
        switch (dns->status)
        {
            case PWADns::Status::none:
                count(Outcome::unknownDomain);
                return pwaError(session, 404, head);
            case PWADns::Status::failed:
                count(Outcome::dnsFailed);
                return pwaError(session, 503, head);
            case PWADns::Status::found:
                lookup.account = dns->account;
        }
    }
    queuePWALookup(session.detach(), lookup);
}

void
ServerHandler::resolvePWADomain(
    std::string const& host,
    std::optional<beast::IP::Endpoint> const& resolver)
{
    using Status = PWADns::Status;
    PWADns dns;
    std::uint32_t ttl = 60;
    struct __res_state res
    {
    };
    if (res_ninit(&res) == 0)
    {
        res.retrans = 1;  // seconds per attempt
        res.retry = 2;
        if (resolver)
        {
            res.nscount = 1;
            res.nsaddr_list[0].sin_family = AF_INET;
            res.nsaddr_list[0].sin_port = htons(resolver->port());
            res.nsaddr_list[0].sin_addr.s_addr =
                htonl(resolver->to_v4().to_uint());
        }
        unsigned char buf[4096];
        int const n = res_nquery(
            &res,
            ("_xahau-pwa." + host).c_str(),
            ns_c_in,
            ns_t_txt,
            buf,
            sizeof(buf));
        ns_msg msg;
        if (n < 0)
        {
            // NXDOMAIN and NOERROR-without-data are both "no record".
            if (res.res_h_errno == HOST_NOT_FOUND || res.res_h_errno == NO_DATA)
                dns.status = Status::none;
        }
        else if (ns_initparse(buf, n, &msg) == 0)
        {
            // Exactly one distinct address across the TXT records; other
            // strings are ignored, two or more addresses count as none.
            dns.status = Status::none;
            bool conflict = false;
            ns_rr rr;
            for (int i = 0; i < ns_msg_count(msg, ns_s_an); ++i)
            {
                if (ns_parserr(&msg, ns_s_an, i, &rr) != 0 ||
                    ns_rr_type(rr) != ns_t_txt)
                    continue;
                auto const* p = ns_rr_rdata(rr);
                auto const* const end = p + ns_rr_rdlen(rr);
                for (; p < end && p + 1 + *p <= end; p += 1 + *p)
                {
                    auto const a = parseBase58<AccountID>(
                        boost::algorithm::trim_copy(std::string(
                            reinterpret_cast<char const*>(p + 1), *p)));
                    if (!a || a->isZero())
                        continue;
                    conflict = conflict ||
                        (dns.status == Status::found && dns.account != *a);
                    dns.status = Status::found;
                    dns.account = *a;
                    ttl = std::clamp<std::uint32_t>(ns_rr_ttl(rr), 60, 3600);
                }
            }
            if (conflict)
                dns.status = Status::none;
        }
        res_nclose(&res);
    }
    dns.expires = std::chrono::steady_clock::now() +
        (dns.status == Status::found ? std::chrono::seconds(ttl)
             : dns.status == Status::none
             ? std::chrono::seconds(std::chrono::minutes(15))
             : std::chrono::seconds(60));

    std::lock_guard lock(pwaDnsMutex_);
    pwaDnsInFlight_.erase(host);
    if (pwaDnsCache_.size() >= maxPWADnsEntries)
    {
        auto const now = std::chrono::steady_clock::now();
        std::erase_if(pwaDnsCache_, [&](auto const& kv) {
            return kv.second.expires <= now;
        });
        if (pwaDnsCache_.size() >= maxPWADnsEntries)
            pwaDnsCache_.erase(pwaDnsCache_.begin());
    }
    pwaDnsCache_[host] = dns;
}

void
ServerHandler::queuePWALookup(
    std::shared_ptr<Session> const& session,
    PWALookup const& lookup)
{
    auto const& port = session->port();
    if (port.pwa_blocklist.count(lookup.account) > 0 ||
        (!port.pwa_allowlist.empty() &&
         port.pwa_allowlist.count(lookup.account) == 0))
    {
        ++pwaStats_.outcomes[std::size_t(Outcome::blocked)];
        return pwaError(*session, 403, lookup.head);
    }

    // jtCLIENT_PWA limits the workers; this bounds the queue behind them
    // and so the detached connections it holds.
    if (pwaJobs_.fetch_add(1, std::memory_order_relaxed) >= maxPWAJobs)
    {
        pwaJobs_.fetch_sub(1, std::memory_order_relaxed);
        ++pwaStats_.outcomes[std::size_t(Outcome::busy)];
        return pwaError(*session, 503, lookup.head);
    }
    auto const slot = std::make_shared<PWAJobSlot>(pwaJobs_);
    if (!m_jobQueue.addJob(
            jtCLIENT_PWA, "PWA-Client", [this, session, slot, lookup]() {
                processPWARequest(session, lookup);
            }))
    {
        ++pwaStats_.outcomes[std::size_t(Outcome::unavailable)];
        pwaError(*session, 503, lookup.head);
    }
}

void
ServerHandler::processPWARequest(
    std::shared_ptr<Session> const& session,
    PWALookup const& lookup)
{
    auto const& account = lookup.account;

    // Decide the whole response before writing any of it: the ledger read
    // can throw (SHAMapMissingNode), and nothing above Job::doJob catches.
    Outcome outcome = Outcome::unavailable;
    int status = 503;
    std::vector<std::string> headers;
    std::string body;
    try
    {
        // Validated state only: a closed ledger may still lose consensus.
        auto const ledger = app_.getLedgerMaster().getValidatedLedger();
        std::shared_ptr<SLE const> sle, root;
        if (ledger)
        {
            sle = ledger->read(keylet::appLoader(account));
            root = ledger->read(keylet::account(account));
        }
        // The host label must be the account's AccountIndex; an account
        // without one (created before it existed) has no host.
        std::optional<std::uint64_t> index;
        if (root)
            index = (*root)[~sfAccountIndex];
        if (!ledger)
        {
        }
        else if (!lookup.redirectTail.empty())
        {
            if (index)
            {
                outcome = Outcome::redirected;
                status = 301;
                headers = {
                    "Location: //pwa" + std::to_string(*index) +
                        lookup.redirectTail,
                    "Cache-Control: public, max-age=86400"};
                body = "Moved Permanently\n";
            }
            else
            {
                outcome = Outcome::notFound;
                status = 404;
            }
        }
        else if (!sle || (lookup.domain.empty() && index != lookup.index))
        {
            outcome = Outcome::notFound;
            status = 404;
        }
        else if (!lookup.domain.empty() && !pwaEndorses(root, lookup.domain))
        {
            // A custom host serves an account only with its consent.
            outcome = Outcome::domainMismatch;
            status = 503;
        }
        else if (sle)
        {
            // Every change is threaded, so the last transaction that touched
            // the object is a strong validator for its content.
            std::string const etag =
                "\"" + to_string(sle->getFieldH256(sfPreviousTxnID)) + "\"";
            headers = {
                "Content-Security-Policy: sandbox allow-scripts allow-forms "
                "allow-popups",
                "X-Frame-Options: DENY",
                "Cross-Origin-Resource-Policy: same-origin",
                "Cross-Origin-Opener-Policy: same-origin",
                "Cache-Control: no-cache",  // store, but revalidate
                "ETag: " + etag,
                // Enough for a client to vet the document against a second
                // node (pwa_validate, or ledger_entry app_loader at this
                // ledger).
                "X-NetworkID: " + std::to_string(app_.config().NETWORK_ID),
                "X-LedgerSeq: " + std::to_string(ledger->seq()),
                "X-LedgerHash: " + to_string(ledger->info().hash),
                "X-PWASeq: " +
                    std::to_string(sle->getFieldU32(sfAppLoaderSequence))};

            // If-None-Match: the tag as we sent it, or *.
            if (lookup.ifNoneMatch == etag || lookup.ifNoneMatch == "*")
            {
                outcome = Outcome::notModified;
                status = 304;
            }
            else
            {
                Blob const blob = sle->getFieldVL(sfAppLoader);
                body.assign(blob.begin(), blob.end());
                headers.push_back("Content-Type: text/html; charset=utf-8");
                outcome = Outcome::served;
                status = 200;
            }
        }
    }
    catch (std::exception const& e)
    {
        JLOG(app_.journal("PWA").warn())
            << "lookup for " << toBase58(account) << " failed: " << e.what();
        outcome = Outcome::unavailable;
        status = 503;
        headers.clear();
        body.clear();
    }

    ++pwaStats_.outcomes[std::size_t(outcome)];
    if (status == 200 || status == 304 || status == 301)
    {
        if (status == 200 && !lookup.head)
            pwaStats_.bytesServed += body.size();
        return pwaReply(*session, status, lookup.head, headers, body);
    }
    pwaError(*session, status, lookup.head);
}

void
ServerHandler::onRequest(Session& session)
{
    // PWA ports speak nothing else.
    if (session.port().protocol.count("pwa") > 0)
    {
        onPWARequest(session);
        return;
    }

    // Make sure RPC is enabled on the port
    if (session.port().protocol.count("http") == 0 &&
        session.port().protocol.count("https") == 0)
    {
        HTTPReply(403, "Forbidden", makeOutput(session), app_.journal("RPC"));
        session.close(true);
        return;
    }

    // Check user/password authorization
    if (!authorized(session.port(), build_map(session.request())))
    {
        HTTPReply(403, "Forbidden", makeOutput(session), app_.journal("RPC"));
        session.close(true);
        return;
    }

    std::shared_ptr<Session> detachedSession = session.detach();
    auto const postResult = m_jobQueue.postCoro(
        jtCLIENT_RPC,
        "RPC-Client",
        [this, detachedSession](std::shared_ptr<JobQueue::Coro> coro) {
            processSession(detachedSession, coro);
        });
    if (postResult == nullptr)
    {
        // The coroutine was rejected, probably because we're shutting down.
        HTTPReply(
            503,
            "Service Unavailable",
            makeOutput(*detachedSession),
            app_.journal("RPC"));
        detachedSession->close(true);
        return;
    }
}

void
ServerHandler::onWSMessage(
    std::shared_ptr<WSSession> session,
    std::vector<boost::asio::const_buffer> const& buffers)
{
    Json::Value jv;
    auto const size = boost::asio::buffer_size(buffers);
    if (size > RPC::Tuning::maxRequestSize ||
        !Json::Reader{}.parse(jv, buffers) || !jv.isObject())
    {
        Json::Value jvResult(Json::objectValue);
        jvResult[jss::type] = jss::error;
        jvResult[jss::error] = "jsonInvalid";
        jvResult[jss::value] = buffers_to_string(buffers);
        boost::beast::multi_buffer sb;
        Json::stream(jvResult, [&sb](auto const p, auto const n) {
            sb.commit(boost::asio::buffer_copy(
                sb.prepare(n), boost::asio::buffer(p, n)));
        });
        JLOG(m_journal.trace()) << "Websocket sending '" << jvResult << "'";
        session->send(
            std::make_shared<StreambufWSMsg<decltype(sb)>>(std::move(sb)));
        session->complete();
        return;
    }

    JLOG(m_journal.trace()) << "Websocket received '" << jv << "'";

    auto const postResult = m_jobQueue.postCoro(
        jtCLIENT_WEBSOCKET,
        "WS-Client",
        [this, session, jv = std::move(jv)](
            std::shared_ptr<JobQueue::Coro> const& coro) {
            auto const jr = this->processSession(session, coro, jv);
            auto const s = to_string(jr);
            auto const n = s.length();
            boost::beast::multi_buffer sb(n);
            sb.commit(boost::asio::buffer_copy(
                sb.prepare(n), boost::asio::buffer(s.c_str(), n)));
            session->send(
                std::make_shared<StreambufWSMsg<decltype(sb)>>(std::move(sb)));
            session->complete();
        });
    if (postResult == nullptr)
    {
        // The coroutine was rejected, probably because we're shutting down.
        session->close({boost::beast::websocket::going_away, "Shutting Down"});
    }
}

void
ServerHandler::onUDPMessage(
    std::string const& message,
    boost::asio::ip::tcp::endpoint const& remoteEndpoint,
    Port const& port,
    std::function<void(std::string const&)> sendResponse)
{
    // Datagrams arriving on the peer port belong to the UDP Superhighway
    if (port.has_peer())
        return app_.overlay().processXUSH(message, remoteEndpoint);

    Json::Value jv;
    if (message.size() > RPC::Tuning::maxRequestSize ||
        !Json::Reader{}.parse(message, jv) || !jv.isObject())
    {
        Json::Value jvResult(Json::objectValue);
        jvResult[jss::type] = jss::error;
        jvResult[jss::error] = "jsonInvalid";
        jvResult[jss::value] = message;

        std::string const response = to_string(jvResult);
        JLOG(m_journal.trace())
            << "UDP sending error response: '" << jvResult << "'";
        sendResponse(response);
        return;
    }

    JLOG(m_journal.trace())
        << "UDP received '" << jv << "' from " << remoteEndpoint;

    auto const postResult = m_jobQueue.postCoro(
        jtCLIENT_RPC,  // Using RPC job type since this is admin RPC
        "UDP-RPC",
        [this,
         remoteEndpoint,
         jv = std::move(jv),
         sendResponse = std::move(sendResponse)](
            std::shared_ptr<JobQueue::Coro> const& coro) {
            // Process the request similar to WebSocket but with UDP context
            Role const role = Role::ADMIN;  // UDP-RPC is admin-only
            auto const jr =
                this->processUDP(jv, role, coro, sendResponse, remoteEndpoint);

            std::string const response = to_string(jr);
            JLOG(m_journal.trace())
                << "UDP sending '" << jr << "' to " << remoteEndpoint;

            // Send response back via UDP
            sendResponse(response);
        });

    if (postResult == nullptr)
    {
        // Request rejected, probably shutting down
        Json::Value jvResult(Json::objectValue);
        jvResult[jss::type] = jss::error;
        jvResult[jss::error] = "serverShuttingDown";
        jvResult[jss::value] = "Server is shutting down";

        std::string const response = to_string(jvResult);
        JLOG(m_journal.trace())
            << "UDP sending shutdown response to " << remoteEndpoint;
        sendResponse(response);
    }
}

void
ServerHandler::onClose(Session& session, boost::system::error_code const&)
{
    std::lock_guard lock(mutex_);
    --count_[session.port()];
}

void
ServerHandler::onStopped(Server&)
{
    std::lock_guard lock(mutex_);
    stopped_ = true;
    condition_.notify_one();
}

//------------------------------------------------------------------------------

template <class T>
void
logDuration(
    Json::Value const& request,
    T const& duration,
    beast::Journal& journal)
{
    using namespace std::chrono_literals;
    auto const level = (duration >= 10s) ? journal.error()
        : (duration >= 1s)               ? journal.warn()
                                         : journal.debug();

    JLOG(level) << "RPC request processing duration = "
                << std::chrono::duration_cast<std::chrono::microseconds>(
                       duration)
                       .count()
                << " microseconds. request = " << request;
}

Json::Value
ServerHandler::processUDP(
    Json::Value const& jv,
    Role const& role,
    std::shared_ptr<JobQueue::Coro> const& coro,
    std::optional<std::function<void(std::string const&)>>
        sendResponse /* used for subscriptions */,
    boost::asio::ip::tcp::endpoint const& remoteEndpoint)
{
    std::shared_ptr<InfoSub> is;
    // Requests without "command" are invalid.
    Json::Value jr(Json::objectValue);
    try
    {
        auto apiVersion =
            RPC::getAPIVersionNumber(jv, app_.config().BETA_RPC_API);
        if (apiVersion == RPC::apiInvalidVersion ||
            (!jv.isMember(jss::command) && !jv.isMember(jss::method)) ||
            (jv.isMember(jss::command) && !jv[jss::command].isString()) ||
            (jv.isMember(jss::method) && !jv[jss::method].isString()) ||
            (jv.isMember(jss::command) && jv.isMember(jss::method) &&
             jv[jss::command].asString() != jv[jss::method].asString()))
        {
            jr[jss::type] = jss::response;
            jr[jss::status] = jss::error;
            jr[jss::error] = apiVersion == RPC::apiInvalidVersion
                ? jss::invalid_API_version
                : jss::missingCommand;
            jr[jss::request] = jv;
            if (jv.isMember(jss::id))
                jr[jss::id] = jv[jss::id];
            if (jv.isMember(jss::jsonrpc))
                jr[jss::jsonrpc] = jv[jss::jsonrpc];
            if (jv.isMember(jss::ripplerpc))
                jr[jss::ripplerpc] = jv[jss::ripplerpc];
            if (jv.isMember(jss::api_version))
                jr[jss::api_version] = jv[jss::api_version];

            return jr;
        }

        if (Role::FORBID == role)
        {
            jr[jss::result] = rpcError(rpcFORBIDDEN);
        }
        else
        {
            Resource::Consumer c;
            Resource::Charge loadType = Resource::feeReferenceRPC;

            if (sendResponse.has_value())
                is = UDPInfoSub::getInfoSub(
                    m_networkOPs, *sendResponse, remoteEndpoint);

            RPC::JsonContext context{
                {app_.journal("RPCHandler"),
                 app_,
                 loadType,
                 app_.getOPs(),
                 app_.getLedgerMaster(),
                 c,
                 role,
                 coro,
                 is,
                 apiVersion},
                jv};

            auto start = std::chrono::system_clock::now();
            RPC::doCommand(context, jr[jss::result]);
            auto end = std::chrono::system_clock::now();
            logDuration(jv, end - start, m_journal);
        }
    }
    catch (std::exception const& ex)
    {
        jr[jss::result] = RPC::make_error(rpcINTERNAL);
        JLOG(m_journal.error())
            << "Exception while processing WS: " << ex.what() << "\n"
            << "Input JSON: " << Json::Compact{Json::Value{jv}};
    }

    if (is)
    {
        if (auto udp = std::dynamic_pointer_cast<UDPInfoSub>(is))
            udp->destroy();
    }

    // Currently we will simply unwrap errors returned by the RPC
    // API, in the future maybe we can make the responses
    // consistent.
    //
    // Regularize result. This is duplicate code.
    if (jr[jss::result].isMember(jss::error))
    {
        jr = jr[jss::result];
        jr[jss::status] = jss::error;

        auto rq = jv;

        if (rq.isObject())
        {
            if (rq.isMember(jss::passphrase.c_str()))
                rq[jss::passphrase.c_str()] = "<masked>";
            if (rq.isMember(jss::secret.c_str()))
                rq[jss::secret.c_str()] = "<masked>";
            if (rq.isMember(jss::seed.c_str()))
                rq[jss::seed.c_str()] = "<masked>";
            if (rq.isMember(jss::seed_hex.c_str()))
                rq[jss::seed_hex.c_str()] = "<masked>";
        }

        jr[jss::request] = rq;
    }
    else
    {
        if (jr[jss::result].isMember("forwarded") &&
            jr[jss::result]["forwarded"])
            jr = jr[jss::result];
        jr[jss::status] = jss::success;
    }

    if (jv.isMember(jss::id))
        jr[jss::id] = jv[jss::id];
    if (jv.isMember(jss::jsonrpc))
        jr[jss::jsonrpc] = jv[jss::jsonrpc];
    if (jv.isMember(jss::ripplerpc))
        jr[jss::ripplerpc] = jv[jss::ripplerpc];
    if (jv.isMember(jss::api_version))
        jr[jss::api_version] = jv[jss::api_version];

    jr[jss::type] = jss::response;
    return jr;
}

Json::Value
ServerHandler::processSession(
    std::shared_ptr<WSSession> const& session,
    std::shared_ptr<JobQueue::Coro> const& coro,
    Json::Value const& jv)
{
    auto is = std::static_pointer_cast<WSInfoSub>(session->appDefined);
    if (is->getConsumer().disconnect(m_journal))
    {
        session->close(
            {boost::beast::websocket::policy_error, "threshold exceeded"});
        // FIX: This rpcError is not delivered since the session
        // was just closed.
        return rpcError(rpcSLOW_DOWN);
    }

    // Requests without "command" are invalid.
    Json::Value jr(Json::objectValue);
    Resource::Charge loadType = Resource::feeReferenceRPC;
    try
    {
        auto apiVersion =
            RPC::getAPIVersionNumber(jv, app_.config().BETA_RPC_API);
        if (apiVersion == RPC::apiInvalidVersion ||
            (!jv.isMember(jss::command) && !jv.isMember(jss::method)) ||
            (jv.isMember(jss::command) && !jv[jss::command].isString()) ||
            (jv.isMember(jss::method) && !jv[jss::method].isString()) ||
            (jv.isMember(jss::command) && jv.isMember(jss::method) &&
             jv[jss::command].asString() != jv[jss::method].asString()))
        {
            jr[jss::type] = jss::response;
            jr[jss::status] = jss::error;
            jr[jss::error] = apiVersion == RPC::apiInvalidVersion
                ? jss::invalid_API_version
                : jss::missingCommand;
            jr[jss::request] = jv;
            if (jv.isMember(jss::id))
                jr[jss::id] = jv[jss::id];
            if (jv.isMember(jss::jsonrpc))
                jr[jss::jsonrpc] = jv[jss::jsonrpc];
            if (jv.isMember(jss::ripplerpc))
                jr[jss::ripplerpc] = jv[jss::ripplerpc];
            if (jv.isMember(jss::api_version))
                jr[jss::api_version] = jv[jss::api_version];

            is->getConsumer().charge(Resource::feeMalformedRPC);
            return jr;
        }

        auto required = RPC::roleRequired(
            apiVersion,
            app_.config().BETA_RPC_API,
            jv.isMember(jss::command) ? jv[jss::command].asString()
                                      : jv[jss::method].asString());
        auto role = requestRole(
            required,
            session->port(),
            jv,
            beast::IP::from_asio(session->remote_endpoint().address()),
            is->user());
        if (Role::FORBID == role)
        {
            loadType = Resource::feeMalformedRPC;
            jr[jss::result] = rpcError(rpcFORBIDDEN);
        }
        else
        {
            RPC::JsonContext context{
                {app_.journal("RPCHandler"),
                 app_,
                 loadType,
                 app_.getOPs(),
                 app_.getLedgerMaster(),
                 is->getConsumer(),
                 role,
                 coro,
                 is,
                 apiVersion},
                jv,
                {is->user(), is->forwarded_for()}};

            auto start = std::chrono::system_clock::now();
            RPC::doCommand(context, jr[jss::result]);
            auto end = std::chrono::system_clock::now();
            logDuration(jv, end - start, m_journal);
        }
    }
    catch (std::exception const& ex)
    {
        jr[jss::result] = RPC::make_error(rpcINTERNAL);
        JLOG(m_journal.error())
            << "Exception while processing WS: " << ex.what() << "\n"
            << "Input JSON: " << Json::Compact{Json::Value{jv}};
    }

    is->getConsumer().charge(loadType);
    if (is->getConsumer().warn())
        jr[jss::warning] = jss::load;

    // Currently we will simply unwrap errors returned by the RPC
    // API, in the future maybe we can make the responses
    // consistent.
    //
    // Regularize result. This is duplicate code.
    if (jr[jss::result].isMember(jss::error))
    {
        jr = jr[jss::result];
        jr[jss::status] = jss::error;

        auto rq = jv;

        if (rq.isObject())
        {
            if (rq.isMember(jss::passphrase.c_str()))
                rq[jss::passphrase.c_str()] = "<masked>";
            if (rq.isMember(jss::secret.c_str()))
                rq[jss::secret.c_str()] = "<masked>";
            if (rq.isMember(jss::seed.c_str()))
                rq[jss::seed.c_str()] = "<masked>";
            if (rq.isMember(jss::seed_hex.c_str()))
                rq[jss::seed_hex.c_str()] = "<masked>";
        }

        jr[jss::request] = rq;
    }
    else
    {
        if (jr[jss::result].isMember("forwarded") &&
            jr[jss::result]["forwarded"])
            jr = jr[jss::result];
        jr[jss::status] = jss::success;
    }

    if (jv.isMember(jss::id))
        jr[jss::id] = jv[jss::id];
    if (jv.isMember(jss::jsonrpc))
        jr[jss::jsonrpc] = jv[jss::jsonrpc];
    if (jv.isMember(jss::ripplerpc))
        jr[jss::ripplerpc] = jv[jss::ripplerpc];
    if (jv.isMember(jss::api_version))
        jr[jss::api_version] = jv[jss::api_version];

    jr[jss::type] = jss::response;
    return jr;
}

// Run as a coroutine.
void
ServerHandler::processSession(
    std::shared_ptr<Session> const& session,
    std::shared_ptr<JobQueue::Coro> coro)
{
    processRequest(
        session->port(),
        buffers_to_string(session->request().body().data()),
        session->remoteAddress().at_port(0),
        makeOutput(*session),
        coro,
        forwardedFor(session->request()),
        [&] {
            auto const iter = session->request().find("X-User");
            if (iter != session->request().end())
                return iter->value();
            return boost::beast::string_view{};
        }());

    if (beast::rfc2616::is_keep_alive(session->request()))
        session->complete();
    else
        session->close(true);
}

static Json::Value
make_json_error(Json::Int code, Json::Value&& message)
{
    Json::Value sub{Json::objectValue};
    sub["code"] = code;
    sub["message"] = std::move(message);
    Json::Value r{Json::objectValue};
    r["error"] = sub;
    return r;
}

Json::Int constexpr method_not_found = -32601;
Json::Int constexpr server_overloaded = -32604;
Json::Int constexpr forbidden = -32605;
Json::Int constexpr wrong_version = -32606;

void
ServerHandler::processRequest(
    Port const& port,
    std::string const& request,
    beast::IP::Endpoint const& remoteIPAddress,
    Output&& output,
    std::shared_ptr<JobQueue::Coro> coro,
    std::string_view forwardedFor,
    std::string_view user)
{
    auto rpcJ = app_.journal("RPC");

    Json::Value jsonOrig;
    {
        Json::Reader reader;
        if ((request.size() > RPC::Tuning::maxRequestSize) ||
            !reader.parse(request, jsonOrig) || !jsonOrig ||
            !jsonOrig.isObject())
        {
            HTTPReply(
                400,
                "Unable to parse request: " + reader.getFormatedErrorMessages(),
                output,
                rpcJ);
            return;
        }
    }

    bool batch = false;
    unsigned size = 1;
    if (jsonOrig.isMember(jss::method) && jsonOrig[jss::method] == "batch")
    {
        batch = true;
        if (!jsonOrig.isMember(jss::params) || !jsonOrig[jss::params].isArray())
        {
            HTTPReply(400, "Malformed batch request", output, rpcJ);
            return;
        }
        size = jsonOrig[jss::params].size();
    }

    Json::Value reply(batch ? Json::arrayValue : Json::objectValue);
    auto const start(std::chrono::high_resolution_clock::now());
    for (unsigned i = 0; i < size; ++i)
    {
        Json::Value const& jsonRPC =
            batch ? jsonOrig[jss::params][i] : jsonOrig;

        if (!jsonRPC.isObject())
        {
            Json::Value r(Json::objectValue);
            r[jss::request] = jsonRPC;
            r[jss::error] =
                make_json_error(method_not_found, "Method not found");
            reply.append(r);
            continue;
        }

        unsigned apiVersion = RPC::apiVersionIfUnspecified;
        if (jsonRPC.isMember(jss::params) && jsonRPC[jss::params].isArray() &&
            jsonRPC[jss::params].size() > 0 &&
            jsonRPC[jss::params][0u].isObject())
        {
            apiVersion = RPC::getAPIVersionNumber(
                jsonRPC[jss::params][Json::UInt(0)],
                app_.config().BETA_RPC_API);
        }

        if (apiVersion == RPC::apiVersionIfUnspecified && batch)
        {
            // for batch request, api_version may be at a different level
            apiVersion =
                RPC::getAPIVersionNumber(jsonRPC, app_.config().BETA_RPC_API);
        }

        if (apiVersion == RPC::apiInvalidVersion)
        {
            if (!batch)
            {
                HTTPReply(400, jss::invalid_API_version.c_str(), output, rpcJ);
                return;
            }
            Json::Value r(Json::objectValue);
            r[jss::request] = jsonRPC;
            r[jss::error] = make_json_error(
                wrong_version, jss::invalid_API_version.c_str());
            reply.append(r);
            continue;
        }

        /* ------------------------------------------------------------------ */
        auto role = Role::FORBID;
        auto required = Role::FORBID;
        if (jsonRPC.isMember(jss::method) && jsonRPC[jss::method].isString())
            required = RPC::roleRequired(
                apiVersion,
                app_.config().BETA_RPC_API,
                jsonRPC[jss::method].asString());

        if (jsonRPC.isMember(jss::params) && jsonRPC[jss::params].isArray() &&
            jsonRPC[jss::params].size() > 0 &&
            jsonRPC[jss::params][Json::UInt(0)].isObjectOrNull())
        {
            role = requestRole(
                required,
                port,
                jsonRPC[jss::params][Json::UInt(0)],
                remoteIPAddress,
                user);
        }
        else
        {
            role = requestRole(
                required, port, Json::objectValue, remoteIPAddress, user);
        }

        Resource::Consumer usage;
        if (isUnlimited(role))
        {
            usage = m_resourceManager.newUnlimitedEndpoint(remoteIPAddress);
        }
        else
        {
            usage = m_resourceManager.newInboundEndpoint(
                remoteIPAddress, role == Role::PROXY, forwardedFor);
            if (usage.disconnect(m_journal))
            {
                if (!batch)
                {
                    HTTPReply(503, "Server is overloaded", output, rpcJ);
                    return;
                }
                Json::Value r = jsonRPC;
                r[jss::error] =
                    make_json_error(server_overloaded, "Server is overloaded");
                reply.append(r);
                continue;
            }
        }

        if (role == Role::FORBID)
        {
            usage.charge(Resource::feeMalformedRPC);
            if (!batch)
            {
                HTTPReply(403, "Forbidden", output, rpcJ);
                return;
            }
            Json::Value r = jsonRPC;
            r[jss::error] = make_json_error(forbidden, "Forbidden");
            reply.append(r);
            continue;
        }

        if (!jsonRPC.isMember(jss::method) || jsonRPC[jss::method].isNull())
        {
            usage.charge(Resource::feeMalformedRPC);
            if (!batch)
            {
                HTTPReply(400, "Null method", output, rpcJ);
                return;
            }
            Json::Value r = jsonRPC;
            r[jss::error] = make_json_error(method_not_found, "Null method");
            reply.append(r);
            continue;
        }

        Json::Value const& method = jsonRPC[jss::method];
        if (!method.isString())
        {
            usage.charge(Resource::feeMalformedRPC);
            if (!batch)
            {
                HTTPReply(400, "method is not string", output, rpcJ);
                return;
            }
            Json::Value r = jsonRPC;
            r[jss::error] =
                make_json_error(method_not_found, "method is not string");
            reply.append(r);
            continue;
        }

        std::string strMethod = method.asString();
        if (strMethod.empty())
        {
            usage.charge(Resource::feeMalformedRPC);
            if (!batch)
            {
                HTTPReply(400, "method is empty", output, rpcJ);
                return;
            }
            Json::Value r = jsonRPC;
            r[jss::error] =
                make_json_error(method_not_found, "method is empty");
            reply.append(r);
            continue;
        }

        // Extract request parameters from the request Json as `params`.
        //
        // If the field "params" is empty, `params` is an empty object.
        //
        // Otherwise, that field must be an array of length 1 (why?)
        // and we take that first entry and validate that it's an object.
        Json::Value params;
        if (!batch)
        {
            params = jsonRPC[jss::params];
            if (!params)
                params = Json::Value(Json::objectValue);

            else if (!params.isArray() || params.size() != 1)
            {
                usage.charge(Resource::feeMalformedRPC);
                HTTPReply(400, "params unparseable", output, rpcJ);
                return;
            }
            else
            {
                params = std::move(params[0u]);
                if (!params.isObjectOrNull())
                {
                    usage.charge(Resource::feeMalformedRPC);
                    HTTPReply(400, "params unparseable", output, rpcJ);
                    return;
                }
            }
        }
        else  // batch
        {
            params = jsonRPC;
        }

        std::string ripplerpc = "1.0";
        if (params.isMember(jss::ripplerpc))
        {
            if (!params[jss::ripplerpc].isString())
            {
                usage.charge(Resource::feeMalformedRPC);
                if (!batch)
                {
                    HTTPReply(400, "ripplerpc is not a string", output, rpcJ);
                    return;
                }

                Json::Value r = jsonRPC;
                r[jss::error] = make_json_error(
                    method_not_found, "ripplerpc is not a string");
                reply.append(r);
                continue;
            }
            ripplerpc = params[jss::ripplerpc].asString();
        }

        /**
         * Clear header-assigned values if not positively identified from a
         * secure_gateway.
         */
        if (role != Role::IDENTIFIED && role != Role::PROXY)
        {
            forwardedFor.remove_suffix(forwardedFor.size());
            user.remove_suffix(user.size());
        }

        JLOG(m_journal.debug()) << "Query: " << strMethod << params;

        // Provide the JSON-RPC method as the field "command" in the request.
        params[jss::command] = strMethod;
        JLOG(m_journal.trace())
            << "doRpcCommand:" << strMethod << ":" << params;

        Resource::Charge loadType = Resource::feeReferenceRPC;

        RPC::JsonContext context{
            {m_journal,
             app_,
             loadType,
             m_networkOPs,
             app_.getLedgerMaster(),
             usage,
             role,
             coro,
             InfoSub::pointer(),
             apiVersion},
            params,
            {user, forwardedFor}};
        Json::Value result;

        auto start = std::chrono::system_clock::now();

        try
        {
            RPC::doCommand(context, result);
        }
        catch (std::exception const& ex)
        {
            result = RPC::make_error(rpcINTERNAL);
            JLOG(m_journal.error()) << "Internal error : " << ex.what()
                                    << " when processing request: "
                                    << Json::Compact{Json::Value{params}};
        }

        auto end = std::chrono::system_clock::now();

        logDuration(params, end - start, m_journal);

        usage.charge(loadType);
        if (usage.warn())
            result[jss::warning] = jss::load;

        Json::Value r(Json::objectValue);
        if (ripplerpc >= "2.0")
        {
            if (result.isMember(jss::error))
            {
                result[jss::status] = jss::error;
                result["code"] = result[jss::error_code];
                result["message"] = result[jss::error_message];
                result.removeMember(jss::error_message);
                JLOG(m_journal.debug()) << "rpcError: " << result[jss::error]
                                        << ": " << result[jss::error_message];
                r[jss::error] = std::move(result);
            }
            else
            {
                result[jss::status] = jss::success;
                r[jss::result] = std::move(result);
            }
        }
        else
        {
            // Always report "status".  On an error report the request as
            // received.
            if (result.isMember(jss::error))
            {
                auto rq = params;

                if (rq.isObject())
                {  // But mask potentially sensitive information.
                    if (rq.isMember(jss::passphrase.c_str()))
                        rq[jss::passphrase.c_str()] = "<masked>";
                    if (rq.isMember(jss::secret.c_str()))
                        rq[jss::secret.c_str()] = "<masked>";
                    if (rq.isMember(jss::seed.c_str()))
                        rq[jss::seed.c_str()] = "<masked>";
                    if (rq.isMember(jss::seed_hex.c_str()))
                        rq[jss::seed_hex.c_str()] = "<masked>";
                }

                result[jss::status] = jss::error;
                result[jss::request] = rq;

                JLOG(m_journal.debug()) << "rpcError: " << result[jss::error]
                                        << ": " << result[jss::error_message];
            }
            else
            {
                result[jss::status] = jss::success;
            }
            r[jss::result] = std::move(result);
        }

        if (params.isMember(jss::jsonrpc))
            r[jss::jsonrpc] = params[jss::jsonrpc];
        if (params.isMember(jss::ripplerpc))
            r[jss::ripplerpc] = params[jss::ripplerpc];
        if (params.isMember(jss::id))
            r[jss::id] = params[jss::id];
        if (batch)
            reply.append(std::move(r));
        else
            reply = std::move(r);

        if (reply.isMember(jss::result) &&
            reply[jss::result].isMember(jss::result))
        {
            reply = reply[jss::result];
            if (reply.isMember(jss::status))
            {
                reply[jss::result][jss::status] = reply[jss::status];
                reply.removeMember(jss::status);
            }
        }
    }

    // If we're returning an error_code, use that to determine the HTTP status.
    int const httpStatus = [&reply]() {
        // This feature is enabled with ripplerpc version 3.0 and above.
        // Before ripplerpc version 3.0 always return 200.
        if (reply.isMember(jss::ripplerpc) &&
            reply[jss::ripplerpc].isString() &&
            reply[jss::ripplerpc].asString() >= "3.0")
        {
            // If there's an error_code, use that to determine the HTTP Status.
            if (reply.isMember(jss::error) &&
                reply[jss::error].isMember(jss::error_code) &&
                reply[jss::error][jss::error_code].isInt())
            {
                int const errCode = reply[jss::error][jss::error_code].asInt();
                return RPC::error_code_http_status(
                    static_cast<error_code_i>(errCode));
            }
        }
        // Return OK.
        return 200;
    }();

    auto response = to_string(reply);

    rpc_time_.notify(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start));
    ++rpc_requests_;
    rpc_size_.notify(beast::insight::Event::value_type{response.size()});

    response += '\n';

    if (auto stream = m_journal.debug())
    {
        static const int maxSize = 10000;
        if (response.size() <= maxSize)
            stream << "Reply: " << response;
        else
            stream << "Reply: " << response.substr(0, maxSize);
    }

    HTTPReply(httpStatus, response, output, rpcJ);
}

//------------------------------------------------------------------------------

/*  This response is used with load balancing.
    If the server is overloaded, status 500 is reported. Otherwise status 200
    is reported, meaning the server can accept more connections.
*/
Handoff
ServerHandler::statusResponse(http_request_type const& request) const
{
    using namespace boost::beast::http;
    Handoff handoff;
    response<string_body> msg;
    std::string reason;
    if (app_.serverOkay(reason))
    {
        msg.result(boost::beast::http::status::ok);
        msg.body() = "<!DOCTYPE html><html><head><title>" +
            std::string(systemName) +
            " Test page for rippled</title></head><body><h1>" +
            std::string(systemName) +
            " Test</h1><p>This page shows rippled http(s) "
            "connectivity is working.</p></body></html>";
    }
    else
    {
        msg.result(boost::beast::http::status::internal_server_error);
        msg.body() = "<HTML><BODY>Server cannot accept clients: " + reason +
            "</BODY></HTML>";
    }
    msg.version(request.version());
    msg.insert("Server", BuildInfo::getFullVersionString());
    msg.insert("Content-Type", "text/html");
    msg.insert("Connection", "close");
    msg.prepare_payload();
    handoff.response = std::make_shared<SimpleWriter>(msg);
    return handoff;
}

//------------------------------------------------------------------------------

void
ServerHandler::Setup::makeContexts()
{
    for (auto& p : ports)
    {
        if (p.secure())
        {
            if (p.ssl_key.empty() && p.ssl_cert.empty() && p.ssl_chain.empty())
                p.context = make_SSLContext(p.ssl_ciphers);
            else
                p.context = make_SSLContextAuthed(
                    p.ssl_key, p.ssl_cert, p.ssl_chain, p.ssl_ciphers);
        }
        else
        {
            p.context = std::make_shared<boost::asio::ssl::context>(
                boost::asio::ssl::context::sslv23);
        }
    }
}

static Port
to_Port(ParsedPort const& parsed, std::ostream& log)
{
    Port p;
    p.name = parsed.name;

    if (!parsed.ip)
    {
        log << "Missing 'ip' in [" << p.name << "]";
        Throw<std::exception>();
    }
    p.ip = *parsed.ip;

    if (!parsed.port)
    {
        log << "Missing 'port' in [" << p.name << "]";
        Throw<std::exception>();
    }
    p.port = *parsed.port;

    if (parsed.protocol.empty())
    {
        log << "Missing 'protocol' in [" << p.name << "]";
        Throw<std::exception>();
    }
    p.protocol = parsed.protocol;

    p.user = parsed.user;
    p.password = parsed.password;
    p.admin_user = parsed.admin_user;
    p.admin_password = parsed.admin_password;
    p.ssl_key = parsed.ssl_key;
    p.ssl_cert = parsed.ssl_cert;
    p.ssl_chain = parsed.ssl_chain;
    p.ssl_ciphers = parsed.ssl_ciphers;
    p.pmd_options = parsed.pmd_options;
    p.ws_queue_limit = parsed.ws_queue_limit;
    p.limit = parsed.limit;
    p.admin_nets_v4 = parsed.admin_nets_v4;
    p.admin_nets_v6 = parsed.admin_nets_v6;
    p.secure_gateway_nets_v4 = parsed.secure_gateway_nets_v4;
    p.secure_gateway_nets_v6 = parsed.secure_gateway_nets_v6;
    p.pwa_domains = parsed.pwa_domains;
    p.pwa_custom_domains = parsed.pwa_custom_domains;
    p.pwa_resolver = parsed.pwa_resolver;

    return p;
}

static std::vector<Port>
parse_Ports(Config const& config, std::ostream& log)
{
    std::vector<Port> result;

    if (!config.exists("server"))
    {
        log << "Required section [server] is missing";
        Throw<std::exception>();
    }

    ParsedPort common;
    parse_Port(common, config["server"], log);

    auto const& names = config.section("server").values();
    result.reserve(names.size());
    for (auto const& name : names)
    {
        if (!config.exists(name))
        {
            log << "Missing section: [" << name << "]";
            Throw<std::exception>();
        }

        // grpc ports are parsed by GRPCServer class. Do not validate
        // grpc port information in this file.
        if (name == SECTION_PORT_GRPC)
            continue;

        ParsedPort parsed = common;
        parse_Port(parsed, config[name], log);
        result.push_back(to_Port(parsed, log));
    }

    // PWA ports host third-party content and must only ever be reached
    // through a reverse proxy. [pwa_allowlist] and [pwa_blocklist], one
    // r-address per line, apply to all of them.
    auto accounts = [&](char const* name) {
        std::set<AccountID> out;
        for (auto const& line : config.section(name).values())
        {
            auto const account = parseBase58<AccountID>(line);
            if (!account || account->isZero())
            {
                log << "Invalid r-address '" << line << "' in [" << name << "]";
                Throw<std::exception>();
            }
            out.insert(*account);
        }
        return out;
    };
    auto const allowlist = accounts(SECTION_PWA_ALLOWLIST);
    auto const blocklist = accounts(SECTION_PWA_BLOCKLIST);
    for (auto const& a : allowlist)
        if (blocklist.count(a) > 0)
        {
            log << "Account " << toBase58(a)
                << " is in both [" SECTION_PWA_ALLOWLIST
                   "] and [" SECTION_PWA_BLOCKLIST "]";
            Throw<std::exception>();
        }
    for (auto& p : result)
    {
        auto fail = [&](char const* why) {
            log << "Invalid [" << p.name << "]: " << why;
            Throw<std::exception>();
        };
        if (p.protocol.count("pwa") == 0)
        {
            if (!p.pwa_domains.empty() || p.pwa_custom_domains ||
                p.pwa_resolver)
                fail(
                    "domain, custom_domains and resolver apply to pwa "
                    "ports only");
            continue;
        }
        if (p.protocol.size() != 1)
            fail("pwa cannot be combined with any other protocol");
        if (p.pwa_domains.empty())
            fail("pwa port requires domain");
        // A /0 gateway would let every client write its own X-Forwarded-For.
        auto const everyAddress = [](auto const& nets) {
            return std::any_of(nets.begin(), nets.end(), [](auto const& n) {
                return n.prefix_length() == 0;
            });
        };
        if ((p.secure_gateway_nets_v4.empty() &&
             p.secure_gateway_nets_v6.empty()) ||
            everyAddress(p.secure_gateway_nets_v4) ||
            everyAddress(p.secure_gateway_nets_v6))
            fail(
                "pwa port requires secure_gateway naming the proxy's own "
                "addresses");
        if (!p.admin_nets_v4.empty() || !p.admin_nets_v6.empty() ||
            !p.admin_user.empty() || !p.admin_password.empty() ||
            !p.user.empty() || !p.password.empty() || !p.ssl_key.empty() ||
            !p.ssl_cert.empty() || !p.ssl_chain.empty())
            fail("pwa port cannot have admin, credentials or TLS");
        p.pwa_allowlist = allowlist;
        p.pwa_blocklist = blocklist;
    }

    if (config.standalone())
    {
        auto it = result.begin();

        while (it != result.end())
        {
            auto& p = it->protocol;

            // Remove the peer protocol, and if that would
            // leave the port empty, remove the port as well
            if (p.erase("peer") && p.empty())
                it = result.erase(it);
            else
                ++it;
        }
    }
    else
    {
        auto const count =
            std::count_if(result.cbegin(), result.cend(), [](Port const& p) {
                return p.protocol.count("peer") != 0;
            });

        if (count > 1)
        {
            log << "Error: More than one peer protocol configured in [server]";
            Throw<std::exception>();
        }

        if (count == 0)
            log << "Warning: No peer protocol configured";
    }

    return result;
}

// Fill out the client portion of the Setup
static void
setup_Client(ServerHandler::Setup& setup)
{
    decltype(setup.ports)::const_iterator iter;
    for (iter = setup.ports.cbegin(); iter != setup.ports.cend(); ++iter)
        if (iter->protocol.count("http") > 0 ||
            iter->protocol.count("https") > 0)
            break;
    if (iter == setup.ports.cend())
        return;
    setup.client.secure = iter->protocol.count("https") > 0;
    setup.client.ip = beast::IP::is_unspecified(iter->ip)
        ?
        // VFALCO HACK! to make localhost work
        (iter->ip.is_v6() ? "::1" : "127.0.0.1")
        : iter->ip.to_string();
    setup.client.port = iter->port;
    setup.client.user = iter->user;
    setup.client.password = iter->password;
    setup.client.admin_user = iter->admin_user;
    setup.client.admin_password = iter->admin_password;
}

// Fill out the overlay portion of the Setup
static void
setup_Overlay(ServerHandler::Setup& setup)
{
    auto const iter = std::find_if(
        setup.ports.cbegin(), setup.ports.cend(), [](Port const& port) {
            return port.protocol.count("peer") != 0;
        });
    if (iter == setup.ports.cend())
    {
        setup.overlay = {};
        return;
    }
    setup.overlay = {iter->ip, iter->port};
}

ServerHandler::Setup
setup_ServerHandler(Config const& config, std::ostream&& log)
{
    ServerHandler::Setup setup;
    setup.ports = parse_Ports(config, log);

    if (config.UDP_HIGHWAY)
    {
        for (auto& port : setup.ports)
            port.udp_highway = port.has_peer();
    }

    setup_Client(setup);
    setup_Overlay(setup);

    return setup;
}

std::unique_ptr<ServerHandler>
make_ServerHandler(
    Application& app,
    boost::asio::io_context& io_context,
    JobQueue& jobQueue,
    NetworkOPs& networkOPs,
    Resource::Manager& resourceManager,
    CollectorManager& cm)
{
    return std::make_unique<ServerHandler>(
        ServerHandler::ServerHandlerCreator(),
        app,
        io_context,
        jobQueue,
        networkOPs,
        resourceManager,
        cm);
}

}  // namespace ripple
