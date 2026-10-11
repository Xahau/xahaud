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

#include <xrpld/app/main/Application.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/ServerHandler.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

/** pwa_info: usage statistics for this node's "pwa" ports. Admin only.

    Request:
        {
          "top":       <uint>,  // optional, rows per table, default 20,
                                // max 200
          "reset":     <bool>,  // optional; return the snapshot, then zero
                                // the counters
          "flush_dns": <bool>   // optional; drop every cached custom domain
                                // answer (after taking the snapshot)
        }

    Response:
        {
          "ports": [ { "name", "ip", "port", "domains": [...],
                       "custom_domains": <bool>,
                       "resolvers": [...] } ],      // only if custom_domains
          "since", "window_seconds",               // collection window
          "requests", "outcomes": { served, not_modified, not_found,
              bad_target, bad_method, no_forwarded_for, direct_connection,
              throttled, unavailable, upgrade_refused, busy, bad_host,
              blocked, redirected, unknown_domain, dns_failed },
          "bytes_served", "last_ledger_index",
          "accounts": { "tracked", "untracked_requests",
              "top": [ { account, served, not_modified, bytes,
                         last_ledger_index, last_seen } ] },
          "clients":  { "tracked", "untracked_requests",
              "top": [ { ip, requests, served, rejected, last_seen } ] },
          "domains":  { "tracked", "untracked_requests",
              "top": [ { domain, account, served, not_modified, bytes,
                         last_seen } ] },
          "dns": { cache_entries, queries_in_flight, waiting_requests,
                   cache_hits, cache_misses, queries, coalesced, busy,
                   found, no_record, failed, timeouts },
          "reset": true,                           // only if reset was asked
          "dns_flushed": <uint>                    // only if flush_dns
        }

    Clients are the rightmost X-Forwarded-For address, i.e. the address
    requests are billed against: an IPv4 address, or an IPv6 /64 shown as
    "<prefix>::/64". 64-bit counters are strings. With "reset", the
    snapshot returned and the reset happen atomically (the DNS counters
    under their own lock; the DNS cache is kept unless flush_dns is set).
    "domains" lists custom domains that served a document; its "account"
    is the one the domain last resolved to.
*/
Json::Value
doPWAInfo(RPC::JsonContext& context)
{
    auto const& params = context.params;

    std::size_t top = 20;
    if (params.isMember(jss::top))
    {
        // isIntegral() also admits booleans, so test the two integer types.
        auto const& v = params[jss::top];
        if (!(v.isInt() || v.isUInt()) || (v.isInt() && v.asInt() < 0) ||
            v.asUInt() > PWAStats::maxTop)
            return RPC::expected_field_error(
                jss::top, "unsigned integer no greater than 200");
        top = v.asUInt();
    }

    bool reset = false;
    if (params.isMember(jss::reset))
    {
        if (!params[jss::reset].isBool())
            return RPC::expected_field_error(jss::reset, "bool");
        reset = params[jss::reset].asBool();
    }

    bool flushDNS = false;
    if (params.isMember("flush_dns"))
    {
        if (!params["flush_dns"].isBool())
            return RPC::expected_field_error("flush_dns", "bool");
        flushDNS = params["flush_dns"].asBool();
    }

    auto& handler = context.app.getServerHandler();
    auto& stats = handler.pwaStats();
    auto& resolver = handler.pwaResolver();

    Json::Value ret = stats.getJson(top, reset);
    ret["dns"] = resolver.getJson(reset);
    if (flushDNS)
        ret["dns_flushed"] = static_cast<Json::UInt>(resolver.flush());

    Json::Value& ports = ret[jss::ports] = Json::arrayValue;
    for (auto const& port : handler.setup().ports)
    {
        if (port.protocol.count("pwa") == 0)
            continue;
        Json::Value& p = ports.append(Json::objectValue);
        p[jss::name] = port.name;
        p[jss::ip] = port.ip.to_string();
        p[jss::port] = port.port;
        Json::Value& domains = p["domains"] = Json::arrayValue;
        for (auto const& d : port.pwa_domains)
            domains.append(d);
        p["custom_domains"] = port.pwa_custom_domains;
        if (port.pwa_custom_domains)
        {
            Json::Value& resolvers = p["resolvers"] = Json::arrayValue;
            for (auto const& r : port.pwa_resolvers)
            {
                auto const addr = r.address();
                resolvers.append(
                    (addr.is_v6() ? "[" + addr.to_string() + "]"
                                  : addr.to_string()) +
                    ":" + std::to_string(r.port()));
            }
        }
    }

    if (reset)
        ret[jss::reset] = true;

    return ret;
}

}  // namespace ripple
