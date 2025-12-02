//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL Labs

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

#include <ripple/app/consensus/RCLValidations.h>
#include <ripple/app/ledger/InboundLedgers.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/main/Application.h>
#include <ripple/app/misc/NetworkOPs.h>
#include <ripple/app/misc/ValidatorList.h>
#include <ripple/basics/LocalValue.h>
#include <ripple/basics/StringUtilities.h>
#include <ripple/net/RPCErr.h>
#include <ripple/protocol/ErrorCodes.h>
#include <ripple/protocol/jss.h>
#include <ripple/rpc/Context.h>
#include <ripple/rpc/DeliveredAmount.h>
#include <ripple/rpc/impl/RPCHelpers.h>

namespace ripple {

// Custom journal partition for submit_and_wait debugging
// Configure with [rpc_startup] { "command": "log_level", "partition":
// "SubmitAndWait", "severity": "debug" }
#define SWLOG(level) JLOG(context.app.journal("SubmitAndWait").level())

// {
//   tx_blob: <hex-encoded signed transaction>
//   timeout: <optional, max wait time in seconds, default 60>
// }
//
// Submit a transaction and wait for it to appear in a VALIDATED ledger.
// Designed for partial sync mode where the node may not have full state
// to validate locally - broadcasts raw transaction and monitors incoming
// ledgers for the result.
//
// The handler waits until:
//   1. Transaction is found in a ledger, AND
//   2. That ledger reaches validation quorum (enough trusted validators)
//
// Response:
//   "validated": true  - Transaction confirmed in validated ledger
//   "error": "timeout" - Timeout waiting
//   "error": "expired" - LastLedgerSequence exceeded
Json::Value
doSubmitAndWait(RPC::JsonContext& context)
{
    Json::Value jvResult;

    // Must have coroutine for polling
    if (!context.coro)
    {
        return RPC::make_error(
            rpcINTERNAL, "submit_and_wait requires coroutine context");
    }

    // Parse tx_blob
    if (!context.params.isMember(jss::tx_blob))
    {
        return rpcError(rpcINVALID_PARAMS);
    }

    auto const txBlobHex = context.params[jss::tx_blob].asString();
    auto const txBlob = strUnHex(txBlobHex);

    if (!txBlob || txBlob->empty())
    {
        return rpcError(rpcINVALID_PARAMS);
    }

    // Parse the transaction to get hash and LastLedgerSequence
    std::shared_ptr<STTx const> stx;
    try
    {
        SerialIter sit(makeSlice(*txBlob));
        stx = std::make_shared<STTx const>(std::ref(sit));
    }
    catch (std::exception& e)
    {
        jvResult[jss::error] = "invalidTransaction";
        jvResult[jss::error_exception] = e.what();
        return jvResult;
    }

    uint256 const txHash = stx->getTransactionID();

    // Extract LastLedgerSequence if present
    std::optional<std::uint32_t> lastLedgerSeq;
    if (stx->isFieldPresent(sfLastLedgerSequence))
    {
        lastLedgerSeq = stx->getFieldU32(sfLastLedgerSequence);
    }

    // Parse timeout (default 60 seconds, max 120 seconds)
    auto timeout = std::chrono::seconds(60);
    if (context.params.isMember("timeout"))
    {
        auto const t = context.params["timeout"].asUInt();
        if (t > 120)
        {
            return RPC::make_error(
                rpcINVALID_PARAMS, "timeout must be <= 120 seconds");
        }
        timeout = std::chrono::seconds(t);
    }

    // Set coroutine-local fetch timeout for SHAMap operations
    setCoroFetchTimeout(
        std::chrono::duration_cast<std::chrono::milliseconds>(timeout / 2));

    SWLOG(warn) << "starting for tx=" << txHash
                << " lastLedgerSeq=" << (lastLedgerSeq ? *lastLedgerSeq : 0)
                << " timeout=" << timeout.count() << "s";

    // Poll for the transaction result
    constexpr auto pollInterval = std::chrono::milliseconds(10);
    auto const startTime = std::chrono::steady_clock::now();

    // Broadcast IMMEDIATELY - don't wait for anything
    SWLOG(warn) << "broadcasting tx=" << txHash;
    auto broadcastResult = context.netOps.broadcastRawTransaction(*txBlob);
    if (!broadcastResult)
    {
        SWLOG(warn) << "broadcast FAILED for tx=" << txHash;
        jvResult[jss::error] = "broadcastFailed";
        jvResult[jss::error_exception] =
            "Failed to parse/broadcast transaction";
        return jvResult;
    }
    SWLOG(warn) << "broadcast SUCCESS for tx=" << txHash;

    // Prioritize TX fetching for ledgers in our window
    // This makes TX nodes fetch before state nodes for faster detection
    auto const startSeq = context.ledgerMaster.getValidLedgerIndex();
    auto const endSeq = lastLedgerSeq.value_or(startSeq + 20);
    context.app.getInboundLedgers().prioritizeTxForLedgers(startSeq, endSeq);

    jvResult[jss::tx_hash] = to_string(txHash);
    jvResult[jss::broadcast] = true;

    // Track when we find the tx and in which ledger
    std::optional<uint256> foundLedgerHash;
    std::optional<std::uint32_t> foundLedgerSeq;

    // Helper to check if a ledger is validated (has quorum)
    auto isLedgerValidated = [&](uint256 const& ledgerHash) -> bool {
        auto const quorum = context.app.validators().quorum();
        if (quorum == 0)
            return false;  // No validators configured

        auto const valCount =
            context.app.getValidations().numTrustedForLedger(ledgerHash);

        return valCount >= quorum;
    };

    // Helper to read tx result from a ledger
    auto readTxResult = [&](std::shared_ptr<Ledger const> const& ledger,
                            std::string const& source) -> bool {
        if (!ledger)
            return false;

        auto [sttx, stobj] = ledger->txRead(txHash);
        if (!sttx || !stobj)
            return false;

        jvResult[jss::status] = "success";
        jvResult[jss::validated] = true;
        jvResult["found_via"] = source;
        jvResult[jss::tx_json] = sttx->getJson(JsonOptions::none);
        jvResult[jss::metadata] = stobj->getJson(JsonOptions::none);
        jvResult[jss::ledger_hash] = to_string(ledger->info().hash);
        jvResult[jss::ledger_index] = ledger->info().seq;

        // Extract result code from metadata
        if (stobj->isFieldPresent(sfTransactionResult))
        {
            auto const result =
                TER::fromInt(stobj->getFieldU8(sfTransactionResult));
            std::string token;
            std::string human;
            transResultInfo(result, token, human);
            jvResult[jss::engine_result] = token;
            jvResult[jss::engine_result_code] = TERtoInt(result);
            jvResult[jss::engine_result_message] = human;
        }

        return true;
    };

    while (true)
    {
        auto const elapsed = std::chrono::steady_clock::now() - startTime;
        if (elapsed >= timeout)
        {
            jvResult[jss::error] = "transactionTimeout";
            jvResult[jss::error_message] =
                "Transaction not validated within timeout period";
            if (foundLedgerSeq)
            {
                jvResult["found_in_ledger"] = *foundLedgerSeq;
                auto const valCount =
                    context.app.getValidations().numTrustedForLedger(
                        *foundLedgerHash);
                auto const quorum = context.app.validators().quorum();
                jvResult["validation_count"] =
                    static_cast<unsigned int>(valCount);
                jvResult["quorum"] = static_cast<unsigned int>(quorum);
            }
            return jvResult;
        }

        // If we already found the tx, check if its ledger is now validated
        if (foundLedgerHash)
        {
            if (isLedgerValidated(*foundLedgerHash))
            {
                // Ledger is validated! Try to read from InboundLedgers first
                auto ledger = context.app.getInboundLedgers().getPartialLedger(
                    *foundLedgerHash);
                if (ledger && readTxResult(ledger, "InboundLedgers"))
                {
                    return jvResult;
                }
                // Try LedgerMaster (for when synced)
                if (foundLedgerSeq)
                {
                    ledger =
                        context.ledgerMaster.getLedgerBySeq(*foundLedgerSeq);
                    if (ledger && readTxResult(ledger, "LedgerMaster"))
                    {
                        return jvResult;
                    }
                }
                // Ledger validated but can't read yet - keep waiting
            }
        }
        else
        {
            auto const currentValidatedSeq =
                context.ledgerMaster.getValidLedgerIndex();

            // Search InboundLedgers for the tx (partial sync mode)
            auto const ledgerHash =
                context.app.getInboundLedgers().findTxLedger(txHash);

            if (ledgerHash)
            {
                auto const ledger =
                    context.app.getInboundLedgers().getPartialLedger(
                        *ledgerHash);

                if (ledger)
                {
                    foundLedgerHash = ledgerHash;
                    foundLedgerSeq = ledger->info().seq;
                    SWLOG(warn) << "FOUND tx in InboundLedgers seq="
                                << ledger->info().seq;

                    if (isLedgerValidated(*ledgerHash))
                    {
                        if (readTxResult(ledger, "InboundLedgers"))
                        {
                            return jvResult;
                        }
                    }
                }
            }

            // Search LedgerMaster for the tx (synced mode via gossip)
            // Check validated ledgers from startSeq to current
            if (!foundLedgerHash)
            {
                for (auto seq = startSeq; seq <= currentValidatedSeq; ++seq)
                {
                    auto ledger = context.ledgerMaster.getLedgerBySeq(seq);
                    if (ledger)
                    {
                        auto [sttx, stobj] = ledger->txRead(txHash);
                        if (sttx && stobj)
                        {
                            foundLedgerHash = ledger->info().hash;
                            foundLedgerSeq = seq;
                            SWLOG(warn)
                                << "FOUND tx in LedgerMaster seq=" << seq;

                            // LedgerMaster ledgers are already validated
                            if (readTxResult(ledger, "LedgerMaster"))
                            {
                                return jvResult;
                            }
                        }
                    }
                }
            }

            // Check LastLedgerSequence expiry
            if (lastLedgerSeq && currentValidatedSeq > *lastLedgerSeq)
            {
                jvResult[jss::error] = "transactionExpired";
                jvResult[jss::error_message] =
                    "LastLedgerSequence exceeded and transaction not found";
                jvResult["last_ledger_sequence"] = *lastLedgerSeq;
                jvResult["validated_ledger"] = currentValidatedSeq;
                return jvResult;
            }
        }

        // Sleep and continue polling
        context.coro->sleepFor(pollInterval);
    }
}

}  // namespace ripple
