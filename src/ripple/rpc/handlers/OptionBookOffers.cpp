//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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

#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/ReadView.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/UintTypes.h>
#include <ripple/protocol/jss.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/impl/RPCHelpers.h>

namespace ripple {

Json::Value
doOptionBookOffers(RPC::JsonContext& context)
{
    // VFALCO TODO Here is a terrible place for this kind of business
    //             logic. It needs to be moved elsewhere and documented,
    //             and encapsulated into a function.
    if (context.app.getJobQueue().getJobCountGE(jtCLIENT) > 200)
        return rpcError(rpcTOO_BUSY);

    std::shared_ptr<ReadView const> lpLedger;
    auto jvResult = RPC::lookupLedger(lpLedger, context);

    if (!lpLedger)
        return jvResult;

    if (!context.params.isMember(jss::strike_price))
        return RPC::missing_field_error(jss::strike_price);

    Json::Value const& strike_price = context.params[jss::strike_price];

    if (!strike_price.isObjectOrNull())
        return RPC::object_field_error(jss::strike_price);

    // if (!taker_gets.isObjectOrNull())
    //     return RPC::object_field_error(jss::taker_gets);

    if (!strike_price.isMember(jss::currency))
        return RPC::missing_field_error("strike_price.currency");

    if (!strike_price[jss::currency].isString())
        return RPC::expected_field_error("strike_price.currency", "string");

    // if (!taker_gets.isMember(jss::currency))
    //     return RPC::missing_field_error("taker_gets.currency");

    // if (!taker_gets[jss::currency].isString())
    //     return RPC::expected_field_error("taker_gets.currency", "string");

    Currency pay_currency;

    if (!to_currency(pay_currency, strike_price[jss::currency].asString()))
    {
        JLOG(context.j.info()) << "Bad strike_price currency.";
        return RPC::make_error(
            rpcSRC_CUR_MALFORMED,
            "Invalid field 'strike_price.currency', bad currency.");
    }

    AccountID pay_issuer;

    if (strike_price.isMember(jss::issuer))
    {
        if (!strike_price[jss::issuer].isString())
            return RPC::expected_field_error("strike_price.issuer", "string");

        if (!to_issuer(pay_issuer, strike_price[jss::issuer].asString()))
            return RPC::make_error(
                rpcSRC_ISR_MALFORMED,
                "Invalid field 'strike_price.issuer', bad issuer.");

        if (pay_issuer == noAccount())
            return RPC::make_error(
                rpcSRC_ISR_MALFORMED,
                "Invalid field 'strike_price.issuer', bad issuer account one.");
    }
    else
    {
        pay_issuer = xrpAccount();
    }

    std::optional<std::uint64_t> pay_value;
    if (strike_price.isMember(jss::value))
    {
        if (!strike_price[jss::value].isString())
            return RPC::expected_field_error("strike_price.value", "string");

        pay_value = strike_price[jss::value].asInt();
        if (!pay_value)
            return RPC::invalid_field_error(jss::value);
    }

    if (isXRP(pay_currency) && !isXRP(pay_issuer))
        return RPC::make_error(
            rpcSRC_ISR_MALFORMED,
            "Unneeded field 'taker_pays.issuer' for "
            "XRP currency specification.");

    if (!isXRP(pay_currency) && isXRP(pay_issuer))
        return RPC::make_error(
            rpcSRC_ISR_MALFORMED,
            "Invalid field 'taker_pays.issuer', expected non-XRP issuer.");

    std::optional<std::uint32_t> expiration;
    if (context.params.isMember(jss::expiration))
    {
        if (!context.params[jss::expiration].isString())
            return RPC::expected_field_error(jss::expiration, "string");

        expiration = context.params[jss::expiration].asInt();
        if (!expiration)
            return RPC::invalid_field_error(jss::expiration);
    }

    unsigned int limit;
    if (auto err = readLimitField(limit, RPC::Tuning::bookOffers, context))
        return *err;

    bool const bProof(context.params.isMember(jss::proof));

    Json::Value const jvMarker(
        context.params.isMember(jss::marker) ? context.params[jss::marker]
                                             : Json::Value(Json::nullValue));

    context.netOps.getOptionBookPage(
        lpLedger,
        STAmount(Issue(pay_currency, pay_issuer), pay_value ? *pay_value : 0),
        expiration ? *expiration : 0,
        limit,
        jvMarker,
        jvResult);

    context.loadType = Resource::feeMediumBurdenRPC;

    return jvResult;
}

}  // namespace ripple
