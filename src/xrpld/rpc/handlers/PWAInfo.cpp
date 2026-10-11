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

/** pwa_info: usage of this node's "pwa" ports. Admin only.

    {"reset": true} zeroes the request counters after reading them;
    {"flush_dns": true} drops every cached custom domain answer. Returns the
    ports (name, ip, port, domains, custom_domains), "requests" and
    "outcomes" (64-bit counters as strings) and "bytes_served".
*/
Json::Value
doPWAInfo(RPC::JsonContext& context)
{
    auto const& params = context.params;
    auto flag = [&](char const* key, bool& out) {
        if (!params.isMember(key))
            return true;
        if (!params[key].isBool())
            return false;
        out = params[key].asBool();
        return true;
    };
    bool reset = false, flush = false;
    if (!flag(jss::reset, reset))
        return RPC::expected_field_error(jss::reset, "bool");
    if (!flag("flush_dns", flush))
        return RPC::expected_field_error("flush_dns", "bool");

    auto& handler = context.app.getServerHandler();
    Json::Value ret = handler.pwaStats().getJson(reset);
    if (flush)
        ret["dns_flushed"] = static_cast<Json::UInt>(handler.pwaDnsFlush());
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
    }
    return ret;
}

}  // namespace ripple
