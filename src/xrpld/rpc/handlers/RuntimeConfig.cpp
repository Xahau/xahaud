//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpld/rpc/Context.h>

namespace ripple {

Json::Value
doRuntimeConfig(RPC::JsonContext& context)
{
    auto& rc = context.app.getRuntimeConfig();
    auto const& params = context.params;

    // SET: { "set": { "*": { "send_delay_ms": 100 },
    //                 "10.0.0.2:51235": { "send_drop_pct": 100.0 } } }
    if (params.isMember("set"))
    {
        auto const& s = params["set"];
        for (auto const& target : s.getMemberNames())
        {
            auto const& v = s[target];
            ConfigVals cfg;
            if (v.isMember("send_delay_ms"))
                cfg.sendDelayMs = v["send_delay_ms"].asInt();
            if (v.isMember("send_delay_jitter_ms"))
                cfg.sendDelayJitterMs = v["send_delay_jitter_ms"].asInt();
            if (v.isMember("send_drop_pct"))
                cfg.sendDropPctX100 =
                    static_cast<int>(v["send_drop_pct"].asDouble() * 100);
            rc.setConfig(target, cfg);
        }
    }

    // CLEAR: { "clear": ["10.0.0.2:51235"] }  or  { "clear": ["*"] }
    if (params.isMember("clear"))
    {
        auto const& c = params["clear"];
        if (c.isArray())
        {
            for (auto const& target : c)
                rc.clearConfig(target.asString());
        }
    }

    // CLEAR_ALL: { "clear_all": true }
    if (params.isMember("clear_all") && params["clear_all"].asBool())
        rc.clearAllConfigs();

    // GET: always return current configs (only set fields)
    Json::Value result{Json::objectValue};
    Json::Value configs{Json::objectValue};
    for (auto const& [target, cfg] : rc.getAllConfigs())
    {
        Json::Value entry{Json::objectValue};
        if (cfg.sendDelayMs)
            entry["send_delay_ms"] = *cfg.sendDelayMs;
        if (cfg.sendDelayJitterMs)
            entry["send_delay_jitter_ms"] = *cfg.sendDelayJitterMs;
        if (cfg.sendDropPctX100)
            entry["send_drop_pct"] = *cfg.sendDropPctX100 / 100.0;
        configs[target] = entry;
    }
    result["configs"] = configs;
    return result;
}

}  // namespace ripple
