//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#ifndef RIPPLE_RPC_PWADOMAINRESOLVER_H_INCLUDED
#define RIPPLE_RPC_PWADOMAINRESOLVER_H_INCLUDED

#include <xrpl/beast/utility/Journal.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ripple {

/** Maps custom domains to accounts for "pwa" ports, by DNS.

    A domain owner names the account to serve at the attribute leaf
    (RFC 8552) beneath their host, `_xahau-pwa.<host> TXT "<r-address>"`;
    the leaf, because <host> is usually a CNAME to the proxy, and a TXT
    query for a CNAME owner answers with the target's records. Exactly one
    distinct address must be present across the TXT records there; other
    records are ignored, and two or more addresses count as none.

    An asynchronous stub resolver on the server's io_context: one UDP
    socket and timer per query, queries for one host coalesced, in-flight
    queries capped, and every answer cached for a bounded time in a bounded
    table. It trusts the configured recursive resolver and does not
    validate DNSSEC. Thread safe.
*/
class PWADomainResolver
{
public:
    enum class Status : std::uint8_t { found, none, failed };

    struct Answer
    {
        Status status = Status::failed;
        AccountID account;
    };

    using Handler = std::function<void(Answer const&)>;
    enum class Start : std::uint8_t { started, coalesced, busy, stopped };

    static constexpr std::string_view recordLabel = "_xahau-pwa";
    static constexpr std::size_t maxCacheEntries = 10'000;
    static constexpr std::size_t maxQueriesInFlight = 64;
    static constexpr std::size_t maxWaiters = 256;
    static constexpr std::chrono::milliseconds attemptTimeout{1500};
    static constexpr std::size_t maxAttempts = 4;
    static constexpr std::uint32_t minPositiveTTL = 60;
    static constexpr std::uint32_t maxPositiveTTL = 3600;
    static constexpr std::uint32_t negativeTTL = 60;
    static constexpr std::uint32_t failureTTL = 10;

    PWADomainResolver(boost::asio::io_context& io, beast::Journal j);
    ~PWADomainResolver();
    PWADomainResolver(PWADomainResolver const&) = delete;
    PWADomainResolver&
    operator=(PWADomainResolver const&) = delete;

    /** A cached, unexpired answer for `host` (lower case, no trailing dot). */
    std::optional<Answer>
    cached(std::string const& host);

    /** Look `host` up and call `handler` once, from an io_context thread
        (or from stop(), with Status::failed), unless the result is `busy`
        or `stopped`. */
    Start
    resolve(
        std::string const& host,
        std::vector<boost::asio::ip::udp::endpoint> const& servers,
        Handler handler);

    /** Fail every waiting request and refuse new ones. */
    void
    stop();

    /** Drop every cached answer; returns how many. */
    std::size_t
    flush();

    /** Cache size and counters, for pwa_info. */
    Json::Value
    getJson();

    // Exposed for tests and parse_Ports.

    /** Two or more LDH labels, no all-digit top label, fits in 253 with
        the record label prepended. */
    static bool
    isCustomDomainName(std::string_view host);

    static std::vector<std::uint8_t>
    encodeQuery(std::string_view host, std::uint16_t id);

    struct Parsed
    {
        enum class Kind : std::uint8_t { answer, nxdomain, error, mismatch };
        Kind kind = Kind::mismatch;
        std::vector<std::string> txt;
        std::uint32_t ttl = 0;
    };

    static Parsed
    parseResponse(
        std::vector<std::uint8_t> const& query,
        std::uint8_t const* response,
        std::size_t size);

    static Answer
    decide(std::vector<std::string> const& txt);

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace ripple

#endif
