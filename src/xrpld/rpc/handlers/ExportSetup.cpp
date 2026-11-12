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
#include <xrpld/app/tx/detail/Import.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/to_string.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/jss.h>

namespace ripple {

// a JSON number usable as an unsigned: the parser gives small literals the
// signed type, so isUInt() alone would refuse them
static bool
isUnsigned(Json::Value const& v)
{
    return v.isUInt() || (v.isInt() && v.asInt() >= 0);
}

/** export_setup: the SignerListSet that sets an account up on a network it
    exports to (see Export.h), to be signed and submitted there by whatever
    the operator signs with, then imported here as an XPOP.

    Takes the usual ledger selectors, plus optional:
        account    the account, filled into tx_json
        network_id the network the list is for (default: the one network
                   an import VL key is trusted for here, if there is exactly
                   one; otherwise 0, the XRPL); above 1024 it is set as the
                   transaction's NetworkID, which such networks require
        quorum     SignerQuorum (default: 80% of the signers, rounded up)
        as_of      ripple epoch seconds: each validator's newest export key
                   recorded at or before then (default: the current keys)

    Returns:
        tx_json          the SignerListSet, complete but for Account (when
                         not given), Sequence and Fee, which the signing
                         library's autofill supplies from the other network.
                         OperationLimit is this network's ID, so the
                         transaction's XPOP can be imported here, which is
                         what records the list for xport().
        network_id       the network tx_json is for
        signers          the export keys it lists, and their validators
        export_keys_seq  UNLReport.ExportKeysSeq; the list needs updating
                         when this changes
        instructions     the steps from here, in words: autofill, sign and
                         submit tx_json on that network with any XRPL
                         library (xrpl.js, xrpl-py, xrpl4j...), then
                         Import its XPOP here; and what the list does and
                         does not change about the account there

    No wallet or signing service is assumed or linked to: tx_json is plain
    transaction JSON, which every wallet and library takes.

    An account that is already an exporter needs none of this again: the
    ledger refreshes its list itself when export_keys_seq changes, and a hook
    can xport() a SignerListSet without SignerEntries to the same effect.
*/
Json::Value
doExportSetup(RPC::JsonContext& context)
{
    auto const& params = context.params;
    if (params.isMember("as_of") && !isUnsigned(params["as_of"]))
        return RPC::invalid_field_error("as_of");
    if (params.isMember(jss::network_id) &&
        !isUnsigned(params[jss::network_id]))
        return RPC::invalid_field_error(jss::network_id);

    std::optional<AccountID> account;
    if (params.isMember(jss::account) &&
        (!params[jss::account].isString() ||
         !(account = parseBase58<AccountID>(params[jss::account].asString()))))
        return RPC::invalid_field_error(jss::account);

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);
    if (!ledger)
        return result;

    // as xport() does for a hook that names no network
    std::uint32_t const nid = params.isMember(jss::network_id)
        ? params[jss::network_id].asUInt()
        : Import::soleNetwork(context.app.config(), *ledger);

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
        if (!isUnsigned(params["quorum"]) || params["quorum"].asUInt() == 0 ||
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
    if (nid > 1024)
        tx[sfNetworkID.jsonName] = nid;
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

    result[jss::network_id] = nid;

    // how to get it onto the other network and its proof back here, with
    // no particular wallet or library assumed
    Json::Value& steps = result["instructions"] = Json::arrayValue;
    steps.append(
        "Submit tx_json on network " + std::to_string(nid) +
        " from the account it is for. Any XRPL library does this: autofill "
        "(which supplies Sequence, Fee and LastLedgerSequence there), sign "
        "with the account's key there, submit, and wait for validation. "
        "For example with xrpl.js: client.submitAndWait(tx_json, { wallet }).");
    steps.append(
        "Make an XPOP of the validated transaction (its binary form, "
        "metadata, the ledger it closed in and that ledger's validations "
        "and UNL); xpop generators such as the xpop-generator service or "
        "library produce one from the transaction hash.");
    steps.append(
        "Submit an Import here with that XPOP as Blob, signed by the same "
        "account: it records the list for xport() and, where the list "
        "names the UNL's export keys, makes the account an exporter on "
        "network " +
        std::to_string(nid) + ".");
    steps.append(
        "Repeat when export_keys_seq changes; while the account is an "
        "exporter the ledger does this refresh itself.");
    steps.append(
        "Know what this changes there. The list replaces any signer list "
        "the account has on network " +
        std::to_string(nid) +
        " and lets a quorum of the UNL's export keys sign for it. It does "
        "not touch the account's master key or regular key there: whoever "
        "holds them can still transact for the account outside any export, "
        "spend its tickets or replace the list. Keep, rotate or disable "
        "them there according to what the exporting hook is trusted with.");
    return result;
}

}  // namespace ripple
