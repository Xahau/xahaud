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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/misc/HashRouter.h>
#include <ripple/app/misc/Transaction.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/tx/apply.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/resource/Fees.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/GRPCHandlers.h>
#include <ripple/rpc/impl/RPCHelpers.h>
#include <ripple/rpc/impl/TransactionSign.h>
#include <future>
#include <thread>
#include <vector>

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

// Helper function to process a single transaction blob
static Json::Value
processSingleTransaction(
    RPC::JsonContext& context,
    const std::string& txBlob,
    const NetworkOPs::FailHard& failType)
{
    Json::Value result;

    auto ret = strUnHex(txBlob);
    if (!ret || !ret->size())
    {
        result[jss::error] = "invalidTransaction";
        result[jss::error_exception] = "Invalid hex encoding";
        return result;
    }

    SerialIter sitTrans(makeSlice(*ret));
    std::shared_ptr<STTx const> stpTrans;

    try
    {
        stpTrans = std::make_shared<STTx const>(std::ref(sitTrans));
    }
    catch (std::exception& e)
    {
        result[jss::error] = "invalidTransaction";
        result[jss::error_exception] = e.what();
        return result;
    }

    // Validity check
    {
        if (!context.app.checkSigs())
            forceValidity(
                context.app.getHashRouter(),
                stpTrans->getTransactionID(),
                Validity::SigGoodOnly);
        auto [validity, reason] = checkValidity(
            context.app.getHashRouter(),
            *stpTrans,
            context.ledgerMaster.getCurrentLedger()->rules(),
            context.app.config());
        if (validity != Validity::Valid)
        {
            result[jss::error] = "invalidTransaction";
            result[jss::error_exception] = "fails local checks: " + reason;
            return result;
        }
    }

    std::string reason;
    auto tpTrans = std::make_shared<Transaction>(stpTrans, reason, context.app);
    if (tpTrans->getStatus() != NEW)
    {
        result[jss::error] = "invalidTransaction";
        result[jss::error_exception] = "fails local checks: " + reason;
        return result;
    }

    try
    {
        context.netOps.processTransaction(
            tpTrans, isUnlimited(context.role), true, failType);
    }
    catch (std::exception& e)
    {
        result[jss::error] = "internalSubmit";
        result[jss::error_exception] = e.what();
        return result;
    }

    try
    {
        result[jss::tx_json] = tpTrans->getJson(JsonOptions::none);
        result[jss::tx_blob] =
            strHex(tpTrans->getSTransaction()->getSerializer().peekData());

        if (temUNCERTAIN != tpTrans->getResult())
        {
            std::string sToken;
            std::string sHuman;

            transResultInfo(tpTrans->getResult(), sToken, sHuman);

            result[jss::engine_result] = sToken;
            result[jss::engine_result_code] = tpTrans->getResult();
            result[jss::engine_result_message] = sHuman;

            auto const submitResult = tpTrans->getSubmitResult();

            result[jss::accepted] = submitResult.any();
            result[jss::applied] = submitResult.applied;
            result[jss::broadcast] = submitResult.broadcast;
            result[jss::queued] = submitResult.queued;
            result[jss::kept] = submitResult.kept;

            if (auto currentLedgerState = tpTrans->getCurrentLedgerState())
            {
                result[jss::account_sequence_next] =
                    safe_cast<Json::Value::UInt>(
                        currentLedgerState->accountSeqNext);
                result[jss::account_sequence_available] =
                    safe_cast<Json::Value::UInt>(
                        currentLedgerState->accountSeqAvail);
                result[jss::open_ledger_cost] =
                    to_string(currentLedgerState->minFeeRequired);
                result[jss::validated_ledger_index] =
                    safe_cast<Json::Value::UInt>(
                        currentLedgerState->validatedLedger);
            }
        }

        return result;
    }
    catch (std::exception& e)
    {
        result[jss::error] = "internalJson";
        result[jss::error_exception] = e.what();
        return result;
    }
}

// {
//   tx_json: <object>,
//   secret: <secret>
// }
// OR for batch submission:
// {
//   "tx_blobs": [<blob1>, <blob2>, ...],
// }
Json::Value
doSubmit(RPC::JsonContext& context)
{
    context.loadType = Resource::feeMediumBurdenRPC;

    // Check for batch submission
    if (context.params.isMember("tx_blobs"))
    {
        if (!context.params["tx_blobs"].isArray())
            return rpcError(rpcINVALID_PARAMS);

        const auto& txBlobs = context.params["tx_blobs"];
        const auto blobCount = txBlobs.size();

        if (blobCount == 0)
            return rpcError(rpcINVALID_PARAMS);

        // Limit batch size to prevent resource exhaustion
        constexpr size_t maxBatchSize = 100;
        if (blobCount > maxBatchSize)
        {
            Json::Value error;
            error[jss::error] = "batchSizeExceeded";
            error["error_message"] =
                "Batch size exceeds maximum of " + std::to_string(maxBatchSize);
            return error;
        }

        auto const failType = getFailHard(context);

        // Process transactions in parallel
        std::vector<std::future<Json::Value>> futures;
        futures.reserve(blobCount);

        // Launch async tasks for each transaction
        for (size_t i = 0; i < blobCount; ++i)
        {
            if (!txBlobs[i].isString())
            {
                // Create error result for invalid blob
                std::promise<Json::Value> errorPromise;
                Json::Value errorResult;
                errorResult[jss::error] = "invalidTransaction";
                errorResult[jss::error_exception] =
                    "tx_blobs element must be string";
                errorPromise.set_value(std::move(errorResult));
                futures.push_back(errorPromise.get_future());
                continue;
            }

            const std::string txBlobStr = txBlobs[i].asString();
            futures.push_back(std::async(
                std::launch::async, [&context, txBlobStr, failType]() {
                    return processSingleTransaction(
                        context, txBlobStr, failType);
                }));
        }

        // Collect results
        Json::Value jvResult;
        Json::Value& results = jvResult["tx_results"] = Json::arrayValue;

        for (auto& future : futures)
        {
            results.append(future.get());
        }

        jvResult["batch_count"] = static_cast<Json::UInt>(blobCount);

        // Count successful submissions
        Json::UInt successCount = 0;
        for (const auto& result : results)
        {
            std::cout << result << "\n";
            if (!result.isMember(jss::error))
                ++successCount;
        }
        jvResult["success_count"] = successCount;

        return jvResult;
    }

    // Single transaction submission (original code path)
    if (!context.params.isMember(jss::tx_blob))
    {
        auto const failType = getFailHard(context);

        if (context.role != Role::ADMIN && !context.app.config().canSign())
            return RPC::make_error(
                rpcNOT_SUPPORTED, "Signing is not supported by this server.");

        auto ret = RPC::transactionSubmit(
            context.params,
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

    // Process single tx_blob
    auto const failType = getFailHard(context);
    return processSingleTransaction(
        context, context.params[jss::tx_blob].asString(), failType);
}

}  // namespace ripple
