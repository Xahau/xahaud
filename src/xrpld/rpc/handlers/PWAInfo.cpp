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
          "top":   <uint>,  // optional, rows per table, default 20, max 200
          "reset": <bool>   // optional; return the snapshot, then zero it
        }

    Response:
        {
          "ports": [ { "name", "ip", "port" } ],   // configured pwa ports
          "since", "window_seconds",               // collection window
          "requests", "outcomes": { served, not_modified, not_found,
              bad_target, bad_method, no_forwarded_for, direct_connection,
              throttled, unavailable },
          "bytes_served", "last_ledger_index",
          "accounts": { "tracked", "untracked_requests",
              "top": [ { account, served, not_modified, bytes,
                         last_ledger_index, last_seen } ] },
          "clients":  { "tracked", "untracked_requests",
              "top": [ { ip, requests, served, rejected, last_seen } ] },
          "reset": true                            // only if reset was asked
        }

    Clients are the rightmost X-Forwarded-For address, i.e. the address
    requests are billed against. 64-bit counters are strings.
*/
Json::Value
doPWAInfo(RPC::JsonContext& context)
{
    auto const& params = context.params;

    std::size_t top = 20;
    if (params.isMember(jss::top))
    {
        auto const& v = params[jss::top];
        if (!v.isIntegral() || (v.isInt() && v.asInt() < 0) ||
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

    auto& handler = context.app.getServerHandler();
    auto& stats = handler.pwaStats();

    Json::Value ret = stats.getJson(top);

    Json::Value& ports = ret[jss::ports] = Json::arrayValue;
    for (auto const& port : handler.setup().ports)
    {
        if (port.protocol.count("pwa") == 0)
            continue;
        Json::Value& p = ports.append(Json::objectValue);
        p[jss::name] = port.name;
        p[jss::ip] = port.ip.to_string();
        p[jss::port] = port.port;
    }

    if (reset)
    {
        stats.reset();
        ret[jss::reset] = true;
    }

    return ret;
}

}  // namespace ripple
