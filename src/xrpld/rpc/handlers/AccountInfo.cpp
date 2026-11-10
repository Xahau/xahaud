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
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/ledger/ReadView.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/GRPCHandlers.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpl/hook/Misc.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/UintTypes.h>
#include <xrpl/protocol/jss.h>
#include <algorithm>
#include <grpc/status.h>
#include <map>
#include <optional>
#include <vector>

namespace ripple {

namespace {

void
setEffectiveParameters(
    STObject& result,
    STObject const& entry,
    std::shared_ptr<SLE const> const& definition)
{
    bool const hasDefinition =
        definition && definition->isFieldPresent(sfHookParameters);
    bool const hasEntry = entry.isFieldPresent(sfHookParameters);
    if (!hasDefinition && !hasEntry)
        return;  // LCOV_EXCL_LINE

    STArray parameters{sfHookParameters};

    auto merge = [&parameters](STArray const& source) {
        for (auto const& parameter : source)
        {
            Blob const name = parameter.getFieldVL(sfHookParameterName);
            Blob const value = parameter.getFieldVL(sfHookParameterValue);

            auto existing = std::find_if(
                parameters.begin(),
                parameters.end(),
                [&name](STObject const& current) {
                    return current.getFieldVL(sfHookParameterName) == name;
                });

            if (existing == parameters.end())
            {
                STObject merged = STObject::makeInnerObject(sfHookParameter);
                merged.setFieldVL(sfHookParameterName, name);
                // gatherHookParameters treats an absent value as an empty
                // value. Keep it explicit in the effective response.
                merged.setFieldVL(sfHookParameterValue, value);
                parameters.push_back(std::move(merged));
            }
            else
            {
                existing->setFieldVL(sfHookParameterValue, value);
            }
        }
    };

    if (hasDefinition)
        merge(definition->getFieldArray(sfHookParameters));
    if (hasEntry)
        merge(entry.getFieldArray(sfHookParameters));

    result.setFieldArray(sfHookParameters, std::move(parameters));
}

uint256
effectiveHookOn(
    STObject const& entry,
    std::shared_ptr<SLE const> const& definition,
    SField const& direction)
{
    if (entry.isFieldPresent(direction))
        return entry.getFieldH256(direction);
    if (entry.isFieldPresent(sfHookOn))
        return entry.getFieldH256(sfHookOn);

    if (!definition)
        return uint256{0};  // LCOV_EXCL_LINE

    if (definition->isFieldPresent(direction))
        return definition->getFieldH256(direction);
    if (definition->isFieldPresent(sfHookOn))
        return definition->getFieldH256(sfHookOn);
    return uint256{0};  // LCOV_EXCL_LINE
}

STObject
effectiveHook(
    STObject const& entry,
    std::shared_ptr<SLE const> const& definition)
{
    STObject result = STObject::makeInnerObject(sfHook);

    // If there is no definition, return the entry as is
    if (!definition)
        return result;  // LCOV_EXCL_LINE

    // A blank sfHooks slot is meaningful: preserve it as an empty Hook
    // object instead of applying the defaults used for an installed hook.
    if (!entry.isFieldPresent(sfHookHash))
        return result;  // LCOV_EXCL_LINE

    // These fields are deliberately selected rather than copying the raw
    // objects: HookDefinition bookkeeping must not leak from account_info.
    if (entry.isFieldPresent(sfHookHash))
        result.setFieldH256(sfHookHash, entry.getFieldH256(sfHookHash));
    // CreateCode is not included in AccountRoot Hook field
    // if (definition->isFieldPresent(sfCreateCode))
    //     result.setFieldVL(sfCreateCode,
    //     definition->getFieldVL(sfCreateCode));
    if (entry.isFieldPresent(sfHookGrants))
        result.setFieldArray(sfHookGrants, entry.getFieldArray(sfHookGrants));

    if (entry.isFieldPresent(sfHookNamespace))
        result.setFieldH256(
            sfHookNamespace, entry.getFieldH256(sfHookNamespace));
    else if (definition->isFieldPresent(sfHookNamespace))
        result.setFieldH256(
            sfHookNamespace, definition->getFieldH256(sfHookNamespace));

    if (definition->isFieldPresent(sfHookApiVersion))
        result.setFieldU16(
            sfHookApiVersion, definition->getFieldU16(sfHookApiVersion));

    if (entry.isFieldPresent(sfHookCanEmit))
        result.setFieldH256(sfHookCanEmit, entry.getFieldH256(sfHookCanEmit));
    else if (definition->isFieldPresent(sfHookCanEmit))
        result.setFieldH256(
            sfHookCanEmit, definition->getFieldH256(sfHookCanEmit));
    else
        result.setFieldH256(sfHookCanEmit, UINT256_BIT[ttHOOK_SET]);

    if (entry.isFieldPresent(sfHookName))
        result.setFieldVL(sfHookName, entry.getFieldVL(sfHookName));
    if (entry.isFieldPresent(sfFlags))
        result.setFieldU32(sfFlags, entry.getFieldU32(sfFlags));
    else if (definition->isFieldPresent(sfFlags))
        result.setFieldU32(sfFlags, definition->getFieldU32(sfFlags));

    auto const incoming = effectiveHookOn(entry, definition, sfHookOnIncoming);
    auto const outgoing = effectiveHookOn(entry, definition, sfHookOnOutgoing);
    if (incoming == outgoing)
        result.setFieldH256(sfHookOn, incoming);
    else
    {
        result.setFieldH256(sfHookOnIncoming, incoming);
        result.setFieldH256(sfHookOnOutgoing, outgoing);
    }

    setEffectiveParameters(result, entry, definition);
    return result;
}

Json::Value
accountHooks(ReadView const& ledger, AccountID const& account)
{
    Json::Value hooks(Json::arrayValue);
    auto const hookSLE = ledger.read(keylet::hook(account));
    if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
        return hooks;

    std::map<uint256, std::shared_ptr<SLE const>> definitions;
    for (auto const& hookElement : hookSLE->getFieldArray(sfHooks))
    {
        auto const& entry = hookElement.downcast<STObject const>();
        std::shared_ptr<SLE const> definition;
        if (entry.isFieldPresent(sfHookHash))
        {
            auto const hash = entry.getFieldH256(sfHookHash);
            auto [it, inserted] = definitions.emplace(hash, nullptr);
            if (inserted)
                it->second = ledger.read(keylet::hookDefinition(hash));
            definition = it->second;
        }

        Json::Value wrapped(Json::objectValue);
        wrapped[sfHook.jsonName] =
            effectiveHook(entry, definition).getJson(JsonOptions::none);
        hooks.append(std::move(wrapped));
    }
    return hooks;
}

}  // namespace

// {
//   account: <ident>,
//   ledger_hash : <ledger>
//   ledger_index : <ledger_index>
//   signer_lists : <bool> // optional (default false)
//                         //   if true return SignerList(s).
//   queue : <bool>        // optional (default false)
//                         //   if true return information about transactions
//                         //   in the current TxQ, only if the requested
//                         //   ledger is open. Otherwise if true, returns an
//                         //   error.
// }

// TODO(tom): what is that "default"?
Json::Value
doAccountInfo(RPC::JsonContext& context)
{
    auto& params = context.params;

    if (params.isMember(jss::hooks) && !params[jss::hooks].isBool())
        return RPC::invalid_field_error(jss::hooks);
    bool const includeHooks =
        params.isMember(jss::hooks) && params[jss::hooks].asBool();

    std::string strIdent;
    if (params.isMember(jss::account))
    {
        if (!params[jss::account].isString())
            return RPC::invalid_field_error(jss::account);
        strIdent = params[jss::account].asString();
    }
    else if (params.isMember(jss::ident))
    {
        if (!params[jss::ident].isString())
            return RPC::invalid_field_error(jss::ident);
        strIdent = params[jss::ident].asString();
    }
    else
        return RPC::missing_field_error(jss::account);

    std::shared_ptr<ReadView const> ledger;
    auto result = RPC::lookupLedger(ledger, context);

    if (!ledger)
        return result;

    // Get info on account.
    auto id = parseBase58<AccountID>(strIdent);
    if (!id)
    {
        RPC::inject_error(rpcACT_MALFORMED, result);
        return result;
    }
    auto const accountID{std::move(id.value())};

    static constexpr std::
        array<std::pair<std::string_view, LedgerSpecificFlags>, 11>
            lsFlags{
                {{"defaultRipple", lsfDefaultRipple},
                 {"depositAuth", lsfDepositAuth},
                 {"disableMasterKey", lsfDisableMaster},
                 {"disallowIncomingXRP", lsfDisallowXRP},
                 {"globalFreeze", lsfGlobalFreeze},
                 {"noFreeze", lsfNoFreeze},
                 {"passwordSpent", lsfPasswordSpent},
                 {"requireAuthorization", lsfRequireAuth},
                 {"tshCollect", lsfTshCollect},
                 {"requireDestinationTag", lsfRequireDestTag},
                 {"uriTokenIssuer", lsfURITokenIssuer}}};

    static constexpr std::
        array<std::pair<std::string_view, LedgerSpecificFlags>, 5>
            disallowIncomingFlags{
                {{"disallowIncomingNFTokenOffer",
                  lsfDisallowIncomingNFTokenOffer},
                 {"disallowIncomingCheck", lsfDisallowIncomingCheck},
                 {"disallowIncomingPayChan", lsfDisallowIncomingPayChan},
                 {"disallowIncomingTrustline", lsfDisallowIncomingTrustline},
                 {"disallowIncomingRemit", lsfDisallowIncomingRemit}}};

    static constexpr std::pair<std::string_view, LedgerSpecificFlags>
        allowTrustLineClawbackFlag{
            "allowTrustLineClawback", lsfAllowTrustLineClawback};

    auto const sleAccepted = ledger->read(keylet::account(accountID));
    if (sleAccepted)
    {
        auto const queue =
            params.isMember(jss::queue) && params[jss::queue].asBool();

        if (queue && !ledger->open())
        {
            // It doesn't make sense to request the queue
            // with any closed or validated ledger.
            RPC::inject_error(rpcINVALID_PARAMS, result);
            return result;
        }

        Json::Value jvAccepted(Json::objectValue);
        RPC::injectSLE(jvAccepted, *sleAccepted);
        result[jss::account_data] = jvAccepted;

        Json::Value acctFlags{Json::objectValue};
        for (auto const& lsf : lsFlags)
            acctFlags[lsf.first.data()] = sleAccepted->isFlag(lsf.second);

        if (ledger->rules().enabled(featureDisallowIncoming))
        {
            for (auto const& lsf : disallowIncomingFlags)
                acctFlags[lsf.first.data()] = sleAccepted->isFlag(lsf.second);
        }

        if (ledger->rules().enabled(featureClawback))
            acctFlags[allowTrustLineClawbackFlag.first.data()] =
                sleAccepted->isFlag(allowTrustLineClawbackFlag.second);

        result[jss::account_flags] = std::move(acctFlags);

        if (includeHooks)
            result[jss::hooks] = accountHooks(*ledger, accountID);

        // The document[https://xrpl.org/account_info.html#account_info] states
        // that signer_lists is a bool, however assigning any string value
        // works. Do not allow this. This check is for api Version 2 onwards
        // only
        if (context.apiVersion > 1u && params.isMember(jss::signer_lists) &&
            !params[jss::signer_lists].isBool())
        {
            RPC::inject_error(rpcINVALID_PARAMS, result);
            return result;
        }

        // Return SignerList(s) if that is requested.
        if (params.isMember(jss::signer_lists) &&
            params[jss::signer_lists].asBool())
        {
            // We put the SignerList in an array because of an anticipated
            // future when we support multiple signer lists on one account.
            Json::Value jvSignerList = Json::arrayValue;

            // This code will need to be revisited if in the future we support
            // multiple SignerLists on one account.
            auto const sleSigners = ledger->read(keylet::signers(accountID));
            if (sleSigners)
                jvSignerList.append(sleSigners->getJson(JsonOptions::none));

            // Documentation states this is returned as part of the account_info
            // response, but previously the code put it under account_data. We
            // can move this to the documentated location from apiVersion 2
            // onwards.
            if (context.apiVersion == 1)
            {
                result[jss::account_data][jss::signer_lists] =
                    std::move(jvSignerList);
            }
            else
            {
                result[jss::signer_lists] = std::move(jvSignerList);
            }
        }
        // Return queue info if that is requested
        if (queue)
        {
            Json::Value jvQueueData = Json::objectValue;

            auto const txs = context.app.getTxQ().getAccountTxs(accountID);
            if (!txs.empty())
            {
                jvQueueData[jss::txn_count] =
                    static_cast<Json::UInt>(txs.size());

                auto& jvQueueTx = jvQueueData[jss::transactions];
                jvQueueTx = Json::arrayValue;

                std::uint32_t seqCount = 0;
                std::uint32_t ticketCount = 0;
                std::optional<std::uint32_t> lowestSeq;
                std::optional<std::uint32_t> highestSeq;
                std::optional<std::uint32_t> lowestTicket;
                std::optional<std::uint32_t> highestTicket;
                bool anyAuthChanged = false;
                XRPAmount totalSpend(0);

                // We expect txs to be returned sorted by SeqProxy.  Verify
                // that with a couple of asserts.
                SeqProxy prevSeqProxy = SeqProxy::sequence(0);
                for (auto const& tx : txs)
                {
                    Json::Value jvTx = Json::objectValue;

                    if (tx.seqProxy.isSeq())
                    {
                        XRPL_ASSERT(
                            prevSeqProxy < tx.seqProxy,
                            "rpple::doAccountInfo : first sorted proxy");
                        prevSeqProxy = tx.seqProxy;
                        jvTx[jss::seq] = tx.seqProxy.value();
                        ++seqCount;
                        if (!lowestSeq)
                            lowestSeq = tx.seqProxy.value();
                        highestSeq = tx.seqProxy.value();
                    }
                    else
                    {
                        XRPL_ASSERT(
                            prevSeqProxy < tx.seqProxy,
                            "rpple::doAccountInfo : second sorted proxy");
                        prevSeqProxy = tx.seqProxy;
                        jvTx[jss::ticket] = tx.seqProxy.value();
                        ++ticketCount;
                        if (!lowestTicket)
                            lowestTicket = tx.seqProxy.value();
                        highestTicket = tx.seqProxy.value();
                    }

                    jvTx[jss::fee_level] = to_string(tx.feeLevel);
                    if (tx.lastValid)
                        jvTx[jss::LastLedgerSequence] = *tx.lastValid;

                    jvTx[jss::fee] = to_string(tx.consequences.fee());
                    auto const spend = tx.consequences.potentialSpend() +
                        tx.consequences.fee();
                    jvTx[jss::max_spend_drops] = to_string(spend);
                    totalSpend += spend;
                    bool const authChanged = tx.consequences.isBlocker();
                    if (authChanged)
                        anyAuthChanged = authChanged;
                    jvTx[jss::auth_change] = authChanged;

                    jvQueueTx.append(std::move(jvTx));
                }

                if (seqCount)
                    jvQueueData[jss::sequence_count] = seqCount;
                if (ticketCount)
                    jvQueueData[jss::ticket_count] = ticketCount;
                if (lowestSeq)
                    jvQueueData[jss::lowest_sequence] = *lowestSeq;
                if (highestSeq)
                    jvQueueData[jss::highest_sequence] = *highestSeq;
                if (lowestTicket)
                    jvQueueData[jss::lowest_ticket] = *lowestTicket;
                if (highestTicket)
                    jvQueueData[jss::highest_ticket] = *highestTicket;

                jvQueueData[jss::auth_change_queued] = anyAuthChanged;
                jvQueueData[jss::max_spend_drops_total] = to_string(totalSpend);
            }
            else
                jvQueueData[jss::txn_count] = 0u;

            result[jss::queue_data] = std::move(jvQueueData);
        }
    }
    else
    {
        result[jss::account] = toBase58(accountID);
        RPC::inject_error(rpcACT_NOT_FOUND, result);
    }

    return result;
}

}  // namespace ripple
