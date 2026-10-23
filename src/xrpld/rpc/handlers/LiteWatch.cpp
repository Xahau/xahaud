//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LiteWatch.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/core/Config.h>
#include <xrpld/rpc/Context.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

namespace {

/** Collect the keys a lite_watch / lite_unwatch request names.

    {
        "accounts": ["rXXX", ...],          // AccountRoot, plus discovery
        "keylets":  ["<64 hex>", ...],      // any ledger object by key
        "lines": [                          // trust lines by their parties
            {"account": "rXXX", "issuer": "rYYY", "currency": "USD"}, ...
        ]
    }

    A single "account" is accepted as well, as in account_info.
*/
struct Wanted
{
    std::vector<AccountID> accounts;
    std::vector<std::pair<uint256, std::string>> keys;
};

std::optional<Json::Value>
collect(Json::Value const& params, Wanted& wanted)
{
    auto const account = [&](Json::Value const& v, AccountID& out) {
        if (!v.isString())
            return false;
        auto const id = parseBase58<AccountID>(v.asString());
        if (!id)
            return false;
        out = *id;
        return true;
    };

    if (params.isMember(jss::account))
    {
        AccountID id;
        if (!account(params[jss::account], id))
            return RPC::make_error(rpcACT_MALFORMED);
        wanted.accounts.push_back(id);
    }

    if (params.isMember(jss::accounts))
    {
        auto const& list = params[jss::accounts];
        if (!list.isArray())
            return RPC::expected_field_error(jss::accounts, "array");
        for (auto const& v : list)
        {
            AccountID id;
            if (!account(v, id))
                return RPC::make_error(rpcACT_MALFORMED);
            wanted.accounts.push_back(id);
        }
    }

    if (params.isMember("keylets"))
    {
        auto const& list = params["keylets"];
        if (!list.isArray())
            return RPC::expected_field_error("keylets", "array");
        for (auto const& v : list)
        {
            uint256 key;
            if (!v.isString() || v.asString().size() != 64 ||
                !key.parseHex(v.asString()))
                return RPC::make_param_error(
                    "keylets entries must be 64 hex digits");
            wanted.keys.emplace_back(key, "keylet");
        }
    }

    if (params.isMember(jss::lines))
    {
        auto const& list = params[jss::lines];
        if (!list.isArray())
            return RPC::expected_field_error(jss::lines, "array");
        for (auto const& v : list)
        {
            AccountID a, issuer;
            Currency currency;
            if (!v.isObject() || !account(v[jss::account], a) ||
                !account(v[jss::issuer], issuer) ||
                !v[jss::currency].isString() ||
                !to_currency(currency, v[jss::currency].asString()))
                return RPC::make_param_error(
                    "lines entries need account, issuer and currency");
            wanted.keys.emplace_back(
                keylet::line(a, issuer, currency).key,
                "line " + toBase58(a) + " " + to_string(currency) + " " +
                    toBase58(issuer));
        }
    }

    if (wanted.accounts.empty() && wanted.keys.empty())
        return RPC::make_param_error(
            "nothing to watch: give accounts, keylets or lines");

    return std::nullopt;
}

Json::Value
notLite()
{
    return RPC::make_error(
        rpcNOT_SUPPORTED, "This server is not running in [lite_mode].");
}

}  // namespace

Json::Value
doLiteWatch(RPC::JsonContext& context)
{
    auto& watch = context.app.getLiteWatch();
    if (!watch.enabled())
        return notLite();

    Wanted wanted;
    if (auto err = collect(context.params, wanted))
        return *err;

    Json::Value ret{Json::objectValue};
    Json::Value added{Json::arrayValue};
    for (auto const& id : wanted.accounts)
        if (watch.watchAccount(id))
            added.append(toBase58(id));
    for (auto const& [key, reason] : wanted.keys)
        if (watch.watchKey(key, LiteWatch::Tier::configured, reason))
            added.append(to_string(key));

    ret["added"] = std::move(added);
    ret["key_count"] = static_cast<Json::UInt>(watch.size());

    // Nothing is fetched retroactively: the new keys are read from the
    // next validated ledger, which is a few seconds away.
    if (auto const valid = context.ledgerMaster.getValidatedLedger())
        ret["effective_from_ledger"] = valid->info().seq + 1;

    return ret;
}

Json::Value
doLiteUnwatch(RPC::JsonContext& context)
{
    auto& watch = context.app.getLiteWatch();
    if (!watch.enabled())
        return notLite();

    Wanted wanted;
    if (auto err = collect(context.params, wanted))
        return *err;

    Json::Value ret{Json::objectValue};
    Json::Value removed{Json::arrayValue};
    for (auto const& id : wanted.accounts)
        if (watch.unwatchAccount(id))
            removed.append(toBase58(id));
    for (auto const& [key, reason] : wanted.keys)
        if (watch.unwatchKey(key))
            removed.append(to_string(key));

    ret["removed"] = std::move(removed);
    ret["key_count"] = static_cast<Json::UInt>(watch.size());
    return ret;
}

Json::Value
doLiteStatus(RPC::JsonContext& context)
{
    auto& watch = context.app.getLiteWatch();
    auto ret = watch.getJson();

    if (!context.params.isMember("verbose") ||
        !context.params["verbose"].asBool())
        ret.removeMember("keys");

    if (auto const valid = context.ledgerMaster.getValidatedLedger())
    {
        ret[jss::ledger_index] = valid->info().seq;
        ret[jss::ledger_hash] = to_string(valid->info().hash);
    }

    return ret;
}

}  // namespace ripple
