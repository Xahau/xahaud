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

#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

/** export_signer_list: the SignerListSet that lets the validators' export
    keys sign for an account on another network (see Export.h).

    Takes the usual ledger selectors, plus optional:
        as_of    ripple epoch seconds: each validator's newest export key
                 recorded at or before then (default: the current keys)
        quorum   SignerQuorum (default: 80% of the signers, rounded up)
        account  the account on the other network, filled into tx_json

    Returns tx_json, ready for the other network's autofill, the signers it
    lists, and UNLReport.ExportKeysSeq, which an exporting hook compares to
    know when its signer list may need updating.
*/
Json::Value
doExportSignerList(RPC::JsonContext& context)
{
    auto const& params = context.params;

    std::optional<std::uint32_t> asOf;
    if (params.isMember("as_of"))
    {
        if (!params["as_of"].isIntegral())
            return RPC::invalid_field_error("as_of");
        asOf = params["as_of"].asUInt();
    }

    std::optional<AccountID> account;
    if (params.isMember(jss::account))
    {
        if (!params[jss::account].isString() ||
            !(account =
                  parseBase58<AccountID>(params[jss::account].asString())))
            return RPC::invalid_field_error(jss::account);
    }

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);
    if (!ledger)
        return result;

    auto const unl = ledger->read(keylet::UNLReport());
    result["export_keys_seq"] = unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0;

    // one export key per active validator, in UNLReport order
    struct Signer
    {
        PublicKey validator;
        Blob key;
        std::uint32_t closeTime;
        AccountID account;
    };
    std::vector<Signer> signers;
    if (unl && unl->isFieldPresent(sfActiveValidators))
    {
        for (auto const& v : unl->getFieldArray(sfActiveValidators))
        {
            PublicKey const master(v[sfPublicKey]);
            auto const acc =
                ledger->read(keylet::account(calcAccountID(master)));
            if (!acc || !acc->isFieldPresent(sfExportKeys))
                continue;

            for (auto const& e : acc->getFieldArray(sfExportKeys))
            {
                auto const t = e.getFieldU32(sfCloseTime);
                if (asOf && t > *asOf)
                    continue;
                auto const key = e.getFieldVL(sfExportKey);
                auto const id = calcAccountID(PublicKey(makeSlice(key)));
                if (std::none_of(
                        signers.begin(), signers.end(), [&](Signer const& s) {
                            return s.account == id;
                        }))
                    signers.push_back({master, key, t, id});
                break;
            }
        }
    }

    auto const max = STTx::maxMultiSigners(&ledger->rules());
    result["truncated"] = signers.size() > max;
    if (signers.size() > max)
        signers.erase(signers.begin() + max, signers.end());

    if (signers.empty())
        return RPC::make_error(
            rpcOBJECT_NOT_FOUND, "No export keys for the requested time.");

    std::uint32_t quorum = (signers.size() * 4 + 4) / 5;
    if (params.isMember("quorum"))
    {
        if (!params["quorum"].isIntegral() || params["quorum"].asUInt() == 0 ||
            params["quorum"].asUInt() > signers.size())
            return RPC::invalid_field_error("quorum");
        quorum = params["quorum"].asUInt();
    }

    std::sort(signers.begin(), signers.end(), [](auto const& a, auto const& b) {
        return a.account < b.account;
    });

    Json::Value& list = result["signers"] = Json::arrayValue;
    Json::Value entries = Json::arrayValue;
    for (auto const& s : signers)
    {
        Json::Value& o = list.append(Json::objectValue);
        o["validator"] = toBase58(TokenType::NodePublic, s.validator);
        o["export_key"] = strHex(s.key);
        o["close_time"] = s.closeTime;
        o[jss::account] = toBase58(s.account);

        Json::Value e = Json::objectValue;
        e[sfSignerEntry.jsonName][sfAccount.jsonName] = toBase58(s.account);
        e[sfSignerEntry.jsonName][sfSignerWeight.jsonName] = 1;
        entries.append(e);
    }

    Json::Value& tx = result[jss::tx_json] = Json::objectValue;
    tx[jss::TransactionType] = jss::SignerListSet;
    if (account)
        tx[jss::Account] = toBase58(*account);
    tx[sfSignerQuorum.jsonName] = quorum;
    tx[sfSignerEntries.jsonName] = entries;

    return result;
}

}  // namespace ripple
