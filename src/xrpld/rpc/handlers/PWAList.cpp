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
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/ServerHandler.h>
#include <xrpl/protocol/ErrorCodes.h>

namespace ripple {

/** pwa_list: the PWA allowlist and blocklist. Admin only.

    "allow" and "block" put r-addresses on that list, "unallow" and
    "unblock" take them off it; each is an r-address or an array of them.
    Removals apply first, so {"unblock": a, "allow": a} moves a in one step,
    and the request applies whole or not at all. Changes are written to the
    lists file ([pwa_list_file]) before they take effect, starting from the
    file as it is now so hand edits are kept; {"reload": true} just rereads
    it. Entries from the cfg file are fixed.

    Returns the lists file's "allowlist" and "blocklist", the cfg file's
    "cfg_allowlist" and "cfg_blocklist" when non-empty, the "file" path and
    whether anything "changed". Every pwa port applies the union.
*/
Json::Value
doPWAList(RPC::JsonContext& context)
{
    auto const& params = context.params;
    PWALists add, remove;
    std::pair<char const*, std::set<AccountID>*> const fields[] = {
        {"allow", &add.allow},
        {"block", &add.block},
        {"unallow", &remove.allow},
        {"unblock", &remove.block}};
    for (auto const& [key, out] : fields)
    {
        if (!params.isMember(key))
            continue;
        Json::Value accounts = params[key];
        if (accounts.isString())
        {
            accounts = Json::arrayValue;
            accounts.append(params[key]);
        }
        if (!accounts.isArray())
            return RPC::expected_field_error(key, "r-address or array");
        for (auto const& v : accounts)
        {
            auto const a = v.isString() ? parseBase58<AccountID>(v.asString())
                                        : std::nullopt;
            if (!a || a->isZero())
                return RPC::invalid_field_error(key);
            out->insert(*a);
        }
    }

    bool reload = false;
    if (params.isMember("reload"))
    {
        if (!params["reload"].isBool())
            return RPC::expected_field_error("reload", "bool");
        reload = params["reload"].asBool();
    }
    return context.app.getServerHandler().pwaListsEdit(add, remove, reload);
}

}  // namespace ripple
