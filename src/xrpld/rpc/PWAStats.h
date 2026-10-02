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

#ifndef RIPPLE_RPC_PWASTATS_H_INCLUDED
#define RIPPLE_RPC_PWASTATS_H_INCLUDED

#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/AccountID.h>

#include <boost/asio/ip/address.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace ripple {

/** Usage statistics for the PWA ("pwa" protocol) ports, reported to admins
    by the pwa_info RPC.

    Every request that reaches a pwa port is counted exactly once, under
    the outcome that ended it. Per-account and per-client breakdowns are
    kept in bounded tables: once a table is full, requests for entries not
    already in it are counted in an "untracked" total instead, so a client
    sweeping random addresses cannot grow memory without limit. 404s are
    never tracked per account, for the same reason.

    Clients are keyed by the address requests are billed against: the
    IPv4 address, or the IPv6 /64 (shown as "<prefix>::/64").

    Thread safe. Everything, counters included, sits behind one mutex, so
    a snapshot is always self-consistent and a reset loses no request.
*/
class PWAStats
{
public:
    enum class Outcome : std::uint8_t {
        served = 0,        // 200
        notModified,       // 304
        notFound,          // 404: no such loader
        badTarget,         // 404: target is not /<r-address>
        badMethod,         // 405
        noForwardedFor,    // 400: proxy sent no usable X-Forwarded-For
        directConnection,  // 403: not from a secure_gateway address
        throttled,         // 503: resource manager said disconnect
        unavailable,       // 503: no validated ledger, job queue refused,
                           //      or the ledger read failed
        upgradeRefused,    // 403: websocket upgrade request
        busy,              // 503: too many lookups already in flight
        count_
    };

    static constexpr std::size_t maxTrackedAccounts = 10'000;
    static constexpr std::size_t maxTrackedClients = 10'000;
    static constexpr std::size_t maxTop = 200;

    PWAStats();

    /** Record the outcome of one request.

        @param outcome  How the request ended.
        @param client   The billing address (IPv4, or IPv6 masked to its
                        /64), when one was parsed.
        @param account  The loader's owner; only for served / notModified.
        @param bytes    Body bytes written; only for served.
        @param ledgerSeq Validated ledger the answer came from, if any.
    */
    void
    record(
        Outcome outcome,
        std::optional<boost::asio::ip::address> const& client = {},
        std::optional<AccountID> const& account = {},
        std::size_t bytes = 0,
        std::uint32_t ledgerSeq = 0);

    /** Snapshot as JSON, with the `top` busiest accounts and clients.

        With `reset`, everything is zeroed and the collection window
        restarted under the same lock that took the snapshot, so no
        request is counted twice or lost between the two.
    */
    Json::Value
    getJson(std::size_t top, bool reset = false);

    /** Zero everything and restart the collection window. */
    void
    reset();

    static char const*
    to_string(Outcome o);

private:
    struct AccountEntry
    {
        std::uint64_t served = 0;
        std::uint64_t notModified = 0;
        std::uint64_t bytes = 0;
        std::uint32_t lastLedger = 0;
        std::int64_t lastSeen = 0;  // seconds since the Unix epoch
    };

    struct ClientEntry
    {
        std::uint64_t requests = 0;
        std::uint64_t served = 0;
        std::uint64_t rejected = 0;  // anything but served / notModified
        std::int64_t lastSeen = 0;
    };

    static constexpr std::size_t outcomeCount =
        static_cast<std::size_t>(Outcome::count_);

    void
    resetLocked();

    std::mutex mutex_;
    std::array<std::uint64_t, outcomeCount> outcomes_{};
    std::uint64_t bytesServed_ = 0;
    std::uint32_t lastLedger_ = 0;
    std::chrono::system_clock::time_point since_;
    hash_map<AccountID, AccountEntry> accounts_;
    std::unordered_map<std::string, ClientEntry> clients_;
    std::uint64_t untrackedAccountRequests_ = 0;
    std::uint64_t untrackedClientRequests_ = 0;
};

}  // namespace ripple

#endif
