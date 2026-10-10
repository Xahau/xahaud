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

#include <xrpld/app/main/Application.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/QRCode.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

/** export_setup: the SignerListSet that sets an account up on the network
    it exports to (see Export.h), as a sign request for Xaman.

    Takes the usual ledger selectors, plus optional:
        account    the account, filled into tx_json
        quorum     SignerQuorum (default: 80% of the signers, rounded up)
        as_of      ripple epoch seconds: each validator's newest export key
                   recorded at or before then (default: the current keys)
        qr_invert  draw the QR code's dark modules, for a light background

    Returns:
        tx_json          the SignerListSet. Xaman fills in Fee and Sequence.
                         OperationLimit is this network's ID, so the
                         transaction's XPOP can be imported here, which is
                         what records the list for xport().
        signers          the export keys it lists, and their validators
        export_keys_seq  UNLReport.ExportKeysSeq; the list needs updating
                         when this changes
        xaman.url        a link Xaman opens tx_json from
        xaman.qr         the link as a QR code, one line of UTF-8 half
                         blocks per array entry, absent if too large

    Hooks need none of this: xport() a SignerListSet without SignerEntries.
*/
Json::Value
doExportSetup(RPC::JsonContext& context)
{
    auto const& params = context.params;
    if (params.isMember("as_of") && !params["as_of"].isIntegral())
        return RPC::invalid_field_error("as_of");

    std::optional<AccountID> account;
    if (params.isMember(jss::account) &&
        (!params[jss::account].isString() ||
         !(account = parseBase58<AccountID>(params[jss::account].asString()))))
        return RPC::invalid_field_error(jss::account);

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);
    if (!ledger)
        return result;

    auto const unl = ledger->read(keylet::UNLReport());
    result["export_keys_seq"] = unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0;

    auto const signers = exportSigners(
        *ledger,
        params.isMember("as_of")
            ? std::optional<std::uint32_t>(params["as_of"].asUInt())
            : std::nullopt);
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

    Json::Value& tx = result[jss::tx_json] = Json::objectValue;
    tx[jss::TransactionType] = jss::SignerListSet;
    if (account)
        tx[jss::Account] = toBase58(*account);
    tx[sfSignerQuorum.jsonName] = quorum;
    tx[sfOperationLimit.jsonName] = context.app.config().NETWORK_ID;
    Json::Value& entries = tx[sfSignerEntries.jsonName] = Json::arrayValue;
    Json::Value& list = result["signers"] = Json::arrayValue;
    for (auto const& s : signers)
    {
        Json::Value& o = list.append(Json::objectValue);
        o["validator"] = toBase58(TokenType::NodePublic, s.validator);
        o["export_key"] = strHex(s.key);
        o["close_time"] = s.closeTime;
        o[jss::account] = toBase58(s.account);

        Json::Value& e = entries.append(Json::objectValue);
        e[sfSignerEntry.jsonName][sfAccount.jsonName] = toBase58(s.account);
        e[sfSignerEntry.jsonName][sfSignerWeight.jsonName] = 1;
    }

    // Xaman reads a hex JSON template from its detect link. Uppercase hex
    // packs into the QR code's alphanumeric mode.
    Json::Value& xaman = result["xaman"] = Json::objectValue;
    xaman[jss::url] = "https://xaman.app/detect/" + strHex(to_string(tx));
    if (auto const code = qr::encode(xaman[jss::url].asString()); !code.empty())
    {
        Json::Value& lines = xaman["qr"] = Json::arrayValue;
        for (auto& l : qr::render(code, params["qr_invert"].asBool()))
            lines.append(std::move(l));
    }
    return result;
}

}  // namespace ripple
