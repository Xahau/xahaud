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

// submit_json_tx -- prototype json-tx submission path.
//
// Request:
//   {
//     "tx_json_str": "<exact ASCII bytes the client signed>",
//     "signature":   "<hex-encoded signature over tx_json_str>"
//   }
//
// Pipeline:
//   1. Parse tx_json_str as JSON.
//   2. Verify `signature` over the UTF-8 bytes of tx_json_str using
//      SigningPubKey from the parsed object -- NOT the classical
//      signing payload.
//   3. Build an STTx from the parsed object + signature.
//   4. forceValidity(SigGoodOnly) so downstream code skips the
//      classical sig check (which would fail -- TxnSignature is over
//      the ASCII JSON, not over the binary signing payload).
//   5. Route through the normal Transaction / processTransaction flow.
//
// NB: for this to survive peer relay / re-validation a consensus-level
// change is required: a new `sfJsonTxBody` Blob field that carries
// tx_json_str on chain, plus an amendment that routes STTx::checkSign
// through the ASCII bytes when the field is present. This handler is
// the ingress-side prototype only.

#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/Transaction.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/rpc/Context.h>
#include <xrpld/rpc/detail/RPCHelpers.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/ErrorCodes.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/RPCErr.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/jss.h>
#include <xrpl/resource/Fees.h>

namespace ripple {

namespace {

NetworkOPs::FailHard
getFailHard(RPC::JsonContext const& context)
{
    return NetworkOPs::doFailHard(
        context.params.isMember("fail_hard") &&
        context.params["fail_hard"].asBool());
}

Json::Value
invalidTx(std::string const& reason)
{
    Json::Value jv;
    jv[jss::error] = "invalidTransaction";
    jv[jss::error_exception] = reason;
    return jv;
}

}  // namespace

Json::Value
doSubmitJsonTx(RPC::JsonContext& context)
{
    context.loadType = Resource::feeMediumBurdenRPC;

    // Gate the whole RPC on the amendment. Without it, accepting a
    // json-tx submission would just queue a tx that every peer node
    // rejects as soon as it re-validates the signature classically.
    if (!context.ledgerMaster.getCurrentLedger()->rules().enabled(
            featureJsonTx))
        return rpcError(rpcNOT_ENABLED);

    if (!context.params.isMember("tx_json_str") ||
        !context.params.isMember("signature"))
        return rpcError(rpcINVALID_PARAMS);

    std::string const txJsonStr = context.params["tx_json_str"].asString();
    if (txJsonStr.empty())
        return rpcError(rpcINVALID_PARAMS);

    auto sigBlob = strUnHex(context.params["signature"].asString());
    if (!sigBlob || sigBlob->empty())
        return rpcError(rpcINVALID_PARAMS);

    // 1. Parse the ASCII JSON.
    Json::Value parsed;
    Json::Reader reader;
    if (!reader.parse(txJsonStr, parsed) || !parsed.isObject())
        return invalidTx("tx_json_str is not a valid JSON object");

    // 2. Verify the signature over tx_json_str using SigningPubKey.
    if (!parsed.isMember(jss::SigningPubKey))
        return invalidTx("missing SigningPubKey");

    auto pkBlob = strUnHex(parsed[jss::SigningPubKey].asString());
    if (!pkBlob || !publicKeyType(makeSlice(*pkBlob)))
        return invalidTx("invalid SigningPubKey");

    PublicKey const pk(makeSlice(*pkBlob));

    Slice const msg(
        reinterpret_cast<unsigned char const*>(txJsonStr.data()),
        txJsonStr.size());

    if (!verify(pk, msg, makeSlice(*sigBlob)))
        return invalidTx("signature over tx_json_str failed verification");

    // 3. Build the STTx from parsed + signature + JsonTxBody carrying
    //    the exact ASCII bytes the client signed. TxnSignature is the
    //    ASCII-level sig; the classical sig check must be skipped
    //    (see step 4).
    Json::Value stTxJson = parsed;
    stTxJson[jss::TxnSignature] = strHex(*sigBlob);
    stTxJson[sfJsonTxBody.jsonName] = strHex(txJsonStr);

    STParsedJSONObject parsedObj("tx_json", stTxJson);
    if (!parsedObj.object)
        return invalidTx(
            parsedObj.error.isMember(jss::error_message)
                ? parsedObj.error[jss::error_message].asString()
                : "failed to parse tx_json_str into STObject");

    std::shared_ptr<STTx const> stTx;
    try
    {
        stTx = std::make_shared<STTx const>(std::move(*parsedObj.object));
    }
    catch (std::exception& e)
    {
        return invalidTx(e.what());
    }

    // 4. Bypass the classical TxnSignature check -- we already verified
    //    the signature over the ASCII JSON above.
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
        return invalidTx("fails local checks: " + reason);

    std::string buildReason;
    auto transaction =
        std::make_shared<Transaction>(stTx, buildReason, context.app);
    if (transaction->getStatus() != NEW)
        return invalidTx("fails local checks: " + buildReason);

    try
    {
        auto const failType = getFailHard(context);
        context.netOps.processTransaction(
            transaction, isUnlimited(context.role), true, failType);
    }
    catch (std::exception& e)
    {
        Json::Value jv;
        jv[jss::error] = "internalSubmit";
        jv[jss::error_exception] = e.what();
        return jv;
    }

    Json::Value jvResult;
    try
    {
        jvResult[jss::tx_json] = transaction->getJson(JsonOptions::none);
        jvResult[jss::tx_blob] = strHex(
            transaction->getSTransaction()->getSerializer().peekData());
        jvResult["tx_json_str"] = txJsonStr;

        if (temUNCERTAIN != transaction->getResult())
        {
            std::string sToken, sHuman;
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
    }
    catch (std::exception& e)
    {
        jvResult[jss::error] = "internalJson";
        jvResult[jss::error_exception] = e.what();
    }

    return jvResult;
}

}  // namespace ripple
