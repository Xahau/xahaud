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

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/app/misc/TxQ.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/GRPCHandlers.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpld/rpc/detail/TransactionSign.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/resource/Fees.h>

#include <xrpl/protocol/JSONTxSignatures.h>

namespace ripple {

static NetworkOPs::FailHard
getFailHard(RPC::JsonContext const& context)
{
    return NetworkOPs::doFailHard(
        context.params.isMember("fail_hard") &&
        context.params["fail_hard"].asBool());
}

// {
//    tx_blob: serialized tx
// }
// Only for debug use!!!!
Json::Value
doInject(RPC::JsonContext& context)
{
    if (context.role != Role::ADMIN)
        return rpcError(rpcNO_PERMISSION);

    Json::Value jvResult;

    auto ret = strUnHex(context.params[jss::tx_blob].asString());

    if (!ret || !ret->size())
        return rpcError(rpcINVALID_PARAMS);

    SerialIter sitTrans(makeSlice(*ret));

    std::shared_ptr<STTx const> stpTrans;

    try
    {
        stpTrans = std::make_shared<STTx const>(std::ref(sitTrans));
    }
    catch (std::exception& e)
    {
        jvResult[jss::error] = "invalidTransaction";
        jvResult[jss::error_exception] = e.what();
        jvResult[jss::in_queue] = false;

        return jvResult;
    }

    context.app.getTxQ().debugTxInject(*stpTrans);

    jvResult[jss::tx_json] = stpTrans->getJson(JsonOptions::none);
    jvResult[jss::in_queue] = true;

    return jvResult;
}

// {
//   tx_blob: <string> XOR tx_json: <object>
//              XOR { tx: <json text>, signature: <hex> },
//   secret: <secret>
// }
Json::Value
doSubmit(RPC::JsonContext& context)
{
    context.loadType = Resource::feeMediumBurdenRPC;

    bool const jsontx = !context.params.isMember(jss::tx_blob) &&
        context.params.isMember(jss::tx) &&
        context.params.isMember(jss::sig);

    if (!context.params.isMember(jss::tx_blob) && !jsontx)
    {
        auto const failType = getFailHard(context);

        if (context.role != Role::ADMIN && !context.app.config().canSign())
            return RPC::make_error(
                rpcNOT_SUPPORTED, "Signing is not supported by this server.");

        auto ret = RPC::transactionSubmit(
            context.params,
            context.apiVersion,
            failType,
            context.role,
            context.ledgerMaster.getValidatedLedgerAge(),
            context.app,
            RPC::getProcessTxnFn(context.netOps));

        ret[jss::deprecated] =
            "Signing support in the 'submit' command has been "
            "deprecated and will be removed in a future version "
            "of the server. Please migrate to a standalone "
            "signing tool.";

        return ret;
    }

    Json::Value jvResult;

    std::optional<Blob> ret;
    if (!jsontx)
    {
        ret = strUnHex(context.params[jss::tx_blob].asString());

        if (!ret || !ret->size())
            return rpcError(rpcINVALID_PARAMS);
    }

    std::shared_ptr<STTx const> stTx;

    try
    {
        if (!jsontx)
        {
            SerialIter sitTrans(makeSlice(*ret));
            stTx = std::make_shared<STTx const>(std::ref(sitTrans));
        }
        else
        {
            std::string const raw = context.params[jss::tx].asString();
            auto const [san, diff] = sanitize_jsontx(raw);
            auto const sig = strUnHex(context.params[jss::sig].asString());
            if (!sig || sig->empty())
                throw std::runtime_error("jsontx: bad signature");

            Json::Value jv;
            if (Json::Reader r; !r.parse(san, jv))
                throw std::runtime_error("jsontx: unparsable canonical form");

            // The preimage carries the key but not the signature over itself.
            for (auto const& n :
                 {sfTxnSignature.fieldName, sfSigners.fieldName})
                if (jv.isMember(n))
                    throw std::runtime_error(
                        "jsontx: " + n + " must not appear in tx");
            if (!jv.isMember(sfSigningPubKey.fieldName))
                throw std::runtime_error("jsontx: tx must carry SigningPubKey");

            // Hand the parser the u64 rather than teaching STUInt64 a second
            // spelling; the ISO form only ever exists in the preimage.
            std::optional<std::uint64_t> ms;
            if (jv.isMember(sfTime.fieldName))
            {
                ms = jsontx_iso(jv[sfTime.fieldName].asString());
                jv.removeMember(sfTime.fieldName);
            }

            STParsedJSONObject parsed("tx_json", jv);
            if (!parsed.object)
                throw std::runtime_error(
                    parsed.error[jss::error_message].asString());
            if (ms)
                parsed.object->setFieldU64(sfTime, *ms);
            parsed.object->setFieldVL(sfTxnSignature, *sig);
            stTx = std::make_shared<STTx const>(std::move(*parsed.object));

            // Round-trip the binary codec, then run the exact check a relaying
            // node will run, so this path cannot accept anything the network
            // would later reject.
            Serializer s;
            stTx->add(s);
            SerialIter si(s.slice());
            STTx const rt{si};
            if (jsontx_verify(rt, diff) != raw)
                throw std::runtime_error("jsontx: does not round-trip");
        }
    }
    catch (std::exception& e)
    {
        jvResult[jss::error] = "invalidTransaction";
        jvResult[jss::error_exception] = e.what();

        return jvResult;
    }

    {
        // jsontx signs the plaintext preimage rather than the binary one, so
        // the binary TxnSignature check is satisfied out of band above.
        if (!context.app.checkSigs() || jsontx)
            forceValidity(
                context.app.getHashRouter(),
                stTx->getTransactionID(),
                Validity::SigGoodOnly);
        auto [validity, reason] = checkValidity(
            context.app.getHashRouter(),
            *stTx,
            context.ledgerMaster.getCurrentLedger()->rules(),
            context.app.config());
        if (validity != Validity::Valid)
        {
            jvResult[jss::error] = "invalidTransaction";
            jvResult[jss::error_exception] = "fails local checks: " + reason;

            return jvResult;
        }
    }

    std::string reason;
    auto transaction = std::make_shared<Transaction>(stTx, reason, context.app);
    if (transaction->getStatus() != NEW)
    {
        jvResult[jss::error] = "invalidTransaction";
        jvResult[jss::error_exception] = "fails local checks: " + reason;

        return jvResult;
    }

    try
    {
        auto const failType = getFailHard(context);

        context.netOps.processTransaction(
            transaction, isUnlimited(context.role), true, failType);
    }
    catch (std::exception& e)
    {
        jvResult[jss::error] = "internalSubmit";
        jvResult[jss::error_exception] = e.what();

        return jvResult;
    }

    try
    {
        jvResult[jss::tx_json] = transaction->getJson(JsonOptions::none);
        jvResult[jss::tx_blob] =
            strHex(transaction->getSTransaction()->getSerializer().peekData());

        if (temUNCERTAIN != transaction->getResult())
        {
            std::string sToken;
            std::string sHuman;

            transResultInfo(transaction->getResult(), sToken, sHuman);

            jvResult[jss::engine_result] = sToken;
            jvResult[jss::engine_result_code] = transaction->getResult();
            jvResult[jss::engine_result_message] = sHuman;

            auto const submitResult = transaction->getSubmitResult();

            jvResult[jss::accepted] = submitResult.any();
            jvResult[jss::applied] = submitResult.applied;
            jvResult[jss::broadcast] = submitResult.broadcast;
            jvResult[jss::queued] = submitResult.queued;
            jvResult[jss::kept] = submitResult.kept;

            if (auto currentLedgerState = transaction->getCurrentLedgerState())
            {
                jvResult[jss::account_sequence_next] =
                    safe_cast<Json::Value::UInt>(
                        currentLedgerState->accountSeqNext);
                jvResult[jss::account_sequence_available] =
                    safe_cast<Json::Value::UInt>(
                        currentLedgerState->accountSeqAvail);
                jvResult[jss::open_ledger_cost] =
                    to_string(currentLedgerState->minFeeRequired);
                jvResult[jss::validated_ledger_index] =
                    safe_cast<Json::Value::UInt>(
                        currentLedgerState->validatedLedger);
            }
        }

        return jvResult;
    }
    catch (std::exception& e)
    {
        jvResult[jss::error] = "internalJson";
        jvResult[jss::error_exception] = e.what();

        return jvResult;
    }
}

}  // namespace ripple
