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
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ripple {

/** Maps custom domains to accounts for "pwa" ports, by DNS.

    A domain owner who points <host> at a pwa reverse proxy (CNAME or
    A/AAAA) names the account to serve with a TXT record on the attribute
    leaf beneath it:

        _xahau-pwa.<host>.  TXT  "<r-address>"

    The record lives on its own underscore label (RFC 8552) rather than on
    <host> itself because <host> is usually a CNAME to the proxy, and a TXT
    query for a CNAME owner answers with the TARGET's records -- the proxy
    operator's, not the domain owner's.

    Exactly one distinct, valid, non-zero classic address must be present
    across the TXT records at that name; character-strings within one record
    are concatenated (as for SPF) and surrounding blanks ignored. Records that
    are not addresses are ignored, so the name can carry other attributes
    later. Two or more distinct addresses are treated as no record at all:
    the domain owner has not said which one they mean.

    Lookups are asynchronous: one UDP socket and one timer per query, on the
    server's io_context, with no thread ever blocked on the network. Queries
    for the same host are coalesced, distinct queries in flight are capped,
    and every answer (positive, negative or failure) is cached for a bounded
    time in a bounded table, so that a client sweeping random host names
    costs the resolver one query per name at most, and the node a bounded
    amount of memory.

    This is a stub resolver: it trusts whatever recursive resolver it is
    configured with, and does not validate DNSSEC. Query IDs come from the
    CSPRNG and every query uses a fresh ephemeral port on a connected
    socket, and an answer must echo the question exactly; but an operator
    who wants answers authenticated should point it at a validating
    resolver on the same host (unbound, systemd-resolved with DNSSEC=yes).

    Thread safe.
*/
class PWADomainResolver
{
public:
    enum class Status : std::uint8_t {
        found,   // exactly one address named
        none,    // NXDOMAIN, no TXT, no address, or more than one
        failed,  // no usable answer from any resolver (timeout, SERVFAIL,
                 // REFUSED, truncated, malformed), or stopped
    };

    struct Answer
    {
        Status status = Status::failed;
        AccountID account;
    };

    using Handler = std::function<void(Answer const&)>;

    enum class Start : std::uint8_t {
        started,    // the handler will be called once, later
        coalesced,  // joined a query already in flight for this host
        busy,       // too many queries or waiters; handler not called
        stopped,    // stop() was called; handler not called
    };

    // The attribute leaf the TXT record is published under.
    static constexpr std::string_view recordLabel = "_xahau-pwa";

    static constexpr std::size_t maxCacheEntries = 10'000;
    static constexpr std::size_t maxQueriesInFlight = 64;
    static constexpr std::size_t maxWaiters = 256;
    static constexpr std::chrono::milliseconds attemptTimeout{1500};
    static constexpr std::size_t maxAttempts = 4;

    // TTLs are taken from the TXT records, clamped to this range.
    static constexpr std::uint32_t minPositiveTTL = 60;
    static constexpr std::uint32_t maxPositiveTTL = 3600;
    // No record: long enough to absorb a sweep, short enough that a new
    // record is picked up quickly.
    static constexpr std::uint32_t negativeTTL = 60;
    // Resolver trouble: just long enough to stop a burst re-querying.
    static constexpr std::uint32_t failureTTL = 10;

    PWADomainResolver(boost::asio::io_context& io, beast::Journal j);
    ~PWADomainResolver();

    PWADomainResolver(PWADomainResolver const&) = delete;
    PWADomainResolver&
    operator=(PWADomainResolver const&) = delete;

    /** A cached, unexpired answer for `host`, if there is one. */
    std::optional<Answer>
    cached(std::string const& host);

    /** Look `host` up, asking `servers` in turn, and call `handler` with the
        answer from an io_context thread (or from the thread that calls
        stop(), with Status::failed). Unless the result is `started` or
        `coalesced` the handler is never called.

        `host` must already be lower case, without a trailing dot, and a
        valid host name (see isCustomDomainName).
    */
    Start
    resolve(
        std::string const& host,
        std::vector<boost::asio::ip::udp::endpoint> const& servers,
        Handler handler);

    /** Fail every waiting request and refuse new ones. Idempotent. */
    void
    stop();

    /** Drop every cached answer. Returns how many were dropped. */
    std::size_t
    flush();

    /** Counters and table sizes, for pwa_info. With `reset`, the counters
        (not the cache) are zeroed under the same lock.
    */
    Json::Value
    getJson(bool reset = false);

    //--------------------------------------------------------------------------
    // Pieces exposed for unit tests and for parse_Ports.

    /** Whether `host` (lower case, no trailing dot, no port) can be a custom
        domain: two or more LDH labels of 1..63 characters, not starting or
        ending with '-', a top-level label that is not all digits (so no
        IPv4 literal), and short enough that the record name fits in 253.
    */
    static bool
    isCustomDomainName(std::string_view host);

    /** The wire-format query for TXT/IN at "_xahau-pwa.<host>", with RD set
        and an EDNS(0) OPT record advertising a 1232 byte payload.
    */
    static std::vector<std::uint8_t>
    encodeQuery(std::string_view host, std::uint16_t id);

    struct Parsed
    {
        enum class Kind : std::uint8_t {
            answer,    // NOERROR; txt holds every TXT RR found (maybe none)
            nxdomain,  // RCODE 3
            error,     // other RCODE, truncated, or not a response
            mismatch,  // not an answer to this query: ignore and keep
                       // waiting (stray or spoofed datagram)
        };
        Kind kind = Kind::mismatch;
        std::vector<std::string> txt;
        std::uint32_t ttl = 0;  // smallest TTL among txt
    };

    /** Parse a response to `query` (as built by encodeQuery). Never reads
        outside `response`.
    */
    static Parsed
    parseResponse(
        std::vector<std::uint8_t> const& query,
        std::uint8_t const* response,
        std::size_t size);

    /** The answer the TXT strings at the record name give. */
    static Answer
    decide(std::vector<std::string> const& txt);

    /** Nameservers from /etc/resolv.conf (port 53), in file order. */
    static std::vector<boost::asio::ip::udp::endpoint>
    systemResolvers(std::string const& path = "/etc/resolv.conf");

private:
    class Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace ripple

#endif
