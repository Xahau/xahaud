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

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/rpc/Context.h>
#include <xrpl/basics/Slice.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/predicate.hpp>

namespace ripple {

/** pwa_validate: is a served AppLoader document the account's current one?

    {"account": <r-address>, "hash": <hex SHA-512Half of the bytes served>,
     "domain": <host it was served from, optional>}

    Judged against the validated ledger. "valid" is true when the account's
    current loader hashes to `hash` and, if `domain` is given, the account's
    Domain field equals it or is a parent of it (ignoring case) -- i.e. the
    owner endorses that host. On false, "reason" says why. A client that
    was served from an older ledger (X-LedgerSeq) than "ledger_index" and
    gets "hash mismatch" should simply reload.
*/
Json::Value
doPWAValidate(RPC::JsonContext& context)
{
    auto const& params = context.params;
    for (auto const& field : {jss::account, jss::hash})
        if (!params.isMember(field))
            return RPC::missing_field_error(field);
        else if (!params[field].isString())
            return RPC::invalid_field_error(field);
    auto const account =
        parseBase58<AccountID>(params[jss::account].asString());
    if (!account || account->isZero())
        return rpcError(rpcACT_MALFORMED);
    uint256 hash;
    if (!hash.parseHex(params[jss::hash].asString()))
        return RPC::invalid_field_error(jss::hash);
    std::string domain;
    if (params.isMember(jss::domain))
    {
        if (!params[jss::domain].isString())
            return RPC::invalid_field_error(jss::domain);
        domain =
            boost::algorithm::to_lower_copy(params[jss::domain].asString());
        if (!domain.empty() && domain.back() == '.')
            domain.pop_back();
    }

    auto const ledger = context.ledgerMaster.getValidatedLedger();
    if (!ledger)
        return rpcError(rpcNOT_SYNCED);

    Json::Value ret(Json::objectValue);
    ret[jss::ledger_index] = ledger->seq();
    auto invalid = [&](char const* reason) {
        ret["valid"] = false;
        ret["reason"] = reason;
        return ret;
    };

    auto const sle = ledger->read(keylet::appLoader(*account));
    if (!sle)
        return invalid("no loader");
    ret["app_loader_sequence"] = (*sle)[sfAppLoaderSequence];
    if (sha512Half(makeSlice(sle->getFieldVL(sfAppLoader))) != hash)
        return invalid("hash mismatch");
    if (!domain.empty())
    {
        auto const root = ledger->read(keylet::account(*account));
        std::string endorsed;
        if (root && root->isFieldPresent(sfDomain))
        {
            auto const d = root->getFieldVL(sfDomain);
            endorsed = boost::algorithm::to_lower_copy(
                std::string(d.begin(), d.end()));
        }
        if (endorsed.empty() ||
            (domain != endorsed &&
             !boost::algorithm::ends_with(domain, "." + endorsed)))
            return invalid("domain not endorsed by account");
    }
    ret["valid"] = true;
    return ret;
}

}  // namespace ripple
