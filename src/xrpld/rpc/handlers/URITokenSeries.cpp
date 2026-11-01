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

#include <xrpld/app/main/Application.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/Tuning.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/LedgerFormats.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>

namespace ripple {

/** RPC command that walks a sequential URIToken series.
    {
      account: <issuer>
      uri: <string>            // "lot", or "lot#seq-<n>" to start at <n>
      ledger_hash: <string>    // optional
      ledger_index: <string | unsigned integer> // optional
      limit: <integer>         // optional
      marker: <unsigned integer> // optional: the seq to resume from
    }
    Returns the series header (precommitments and counters) and up to `limit`
    members in ascending sequence order, with `marker` set to the next seq when
    there are more.
*/
Json::Value
doURITokenSeries(RPC::JsonContext& context)
{
    auto const& params = context.params;
    if (!params.isMember(jss::account))
        return RPC::missing_field_error(jss::account);

    if (!params.isMember(jss::uri) || !params[jss::uri].isString())
        return RPC::expected_field_error(jss::uri, "string");

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);
    if (ledger == nullptr)
        return result;

    AccountID accountID;
    {
        auto const strIdent = params[jss::account].asString();
        if (auto jv = RPC::accountFromString(accountID, strIdent))
        {
            for (auto it = jv.begin(); it != jv.end(); ++it)
                result[it.memberName()] = *it;

            return result;
        }
    }

    auto const [uri, startSeq] =
        RPC::splitURITokenURI(params[jss::uri].asString());

    unsigned int limit;
    if (auto err = readLimitField(limit, RPC::Tuning::accountObjects, context))
        return *err;

    std::uint32_t start = startSeq;
    if (params.isMember(jss::marker))
    {
        auto const& marker = params[jss::marker];
        if (!marker.isConvertibleTo(Json::uintValue))
            return RPC::expected_field_error(jss::marker, "unsigned integer");
        start = marker.asUInt();
    }

    auto const klHeader = keylet::uritokenSeries(accountID, uri);
    auto const sleHeader = ledger->read(klHeader);
    if (!sleHeader)
        return rpcError(rpcOBJECT_NOT_FOUND);

    result[jss::account] = toBase58(accountID);
    result[jss::uri] = std::string(uri.begin(), uri.end());
    result[jss::series_index] = to_string(klHeader.key);
    result[jss::series] = sleHeader->getJson(JsonOptions::none);

    Json::Value& tokens = (result[jss::uri_tokens] = Json::arrayValue);

    // members are contiguous and ascending from key(start) to key(2^32 - 1)
    uint256 const last = keylet::uritoken(accountID, uri, 0xFFFFFFFFu).key;
    std::optional<uint256> k = keylet::uritoken(accountID, uri, start).key;
    if (!ledger->exists(Keylet{ltURI_TOKEN, *k}))
        k = ledger->succ(*k, last);

    for (; k; k = ledger->succ(*k, last))
    {
        auto const sle = ledger->read(Keylet{ltURI_TOKEN, *k});

        // skip anything in the span that isn't a member (foreign objects are
        // ~2^-167 per series; this is belt and braces)
        if (!sle || !sle->isFieldPresent(sfURITokenSequence) ||
            sle->getAccountID(sfIssuer) != accountID ||
            sle->getFieldVL(sfURI) != uri)
            continue;

        if (tokens.size() >= limit)
        {
            result[jss::marker] = sle->getFieldU32(sfURITokenSequence);
            break;
        }

        tokens.append(sle->getJson(JsonOptions::none));
    }

    result[jss::limit] = limit;
    context.loadType = Resource::feeMediumBurdenRPC;
    return result;
}

}  // namespace ripple
