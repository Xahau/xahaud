//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/main/Application.h>
#include <ripple/app/misc/ExportSignatureCollector.h>
#include <ripple/app/misc/ValidatorKeys.h>
#include <ripple/app/tx/impl/ExportSign.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/OpenView.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/Sign.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/st.h>

namespace ripple {

//@@start exportsign-transactor
TxConsequences
ExportSign::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
ExportSign::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    if (!ctx.rules.enabled(featureExport))
        return temDISABLED;

    return preflight2(ctx);
}

TER
ExportSign::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureExport))
        return temDISABLED;

    // Signer must be in the UNLReport
    auto const& pkSignerField = ctx.tx.getSigningPubKey();
    if (!publicKeyType(makeSlice(pkSignerField)))
    {
        JLOG(ctx.j.warn()) << "ExportSign: Invalid signing public key type.";
        return tefBAD_AUTH;
    }

    PublicKey pkSigner{makeSlice(pkSignerField)};

    if (!inUNLReport(ctx.view, ctx.app, pkSigner, ctx.j))
    {
        JLOG(ctx.j.warn()) << "ExportSign: Signer isn't in the UNLReport.";
        return tefFAILURE;
    }

    return tesSUCCESS;
}

TER
ExportSign::doApply()
{
    uint256 txnID(ctx_.tx.getFieldH256(sfTransactionHash));
    std::cerr << "[EXPORT-TRACE] STEP-2b: doApply ttEXPORT_SIGN txnID=" << txnID
              << " viewSeq=" << view().seq() << std::endl;

    JLOG(j_.info()) << "HookExport[" << txnID
                    << "]: ttExportSign adding signature to transaction";

    auto key = keylet::exportedTxn(txnID);
    auto const& sle = view().peek(key);

    if (!sle)
    {
        // Most likely this was a double-up, so just ignore
        JLOG(j_.warn()) << "HookError[" << txnID
                        << "]: ttExportSign could not find exported txn in "
                           "ledger";
        return tesSUCCESS;
    }

    // Grab the signer object off the txn
    STObject signerObj = const_cast<ripple::STTx&>(ctx_.tx)
                             .getField(sfSigner)
                             .downcast<STObject>();

    // Append it to the signers field in the ledger object
    STArray signers = sle->getFieldArray(sfSigners);
    signers.push_back(signerObj);
    sle->setFieldArray(sfSigners, signers);

    view().update(sle);

    return tesSUCCESS;
}
//@@end exportsign-transactor

//@@start make-export-sign-txns
std::vector<std::shared_ptr<STTx const>>
makeExportSignTxns(OpenView& view, Application& app, beast::Journal const& j)
{
    std::vector<std::shared_ptr<STTx const>> result;

    if (!view.rules().enabled(featureExport))
        return result;

    JLOG(j.debug()) << "EXPORT_SIGN processing: started";

    auto const seq = view.info().seq;

    // If we're not a validator we do nothing here
    if (app.getValidationPublicKey().empty())
        return result;

    auto const& keys = app.getValidatorKeys();

    if (keys.configInvalid())
        return result;

    PublicKey pkSigning = app.getValidationPublicKey();
    auto const pk = app.validatorManifests().getMasterKey(pkSigning);

    // Only continue if we're on the UNLReport
    if (!inUNLReport(view, app, pk, j))
        return result;

    AccountID signingAcc = calcAccountID(pkSigning);

    Keylet const exportedDirKeylet{keylet::exportedDir()};
    if (dirIsEmpty(view, exportedDirKeylet))
        return result;

    std::shared_ptr<SLE const> sleDirNode{};
    unsigned int uDirEntry{0};
    uint256 dirEntry{beast::zero};

    if (!cdirFirst(
            view, exportedDirKeylet.key, sleDirNode, uDirEntry, dirEntry))
        return result;

    do
    {
        Keylet const itemKeylet{ltCHILD, dirEntry};
        auto sleItem = view.read(itemKeylet);
        if (!sleItem)
        {
            JLOG(j.warn())
                << "ExportedTxn processing: directory node in ledger " << seq
                << " has index to object that is missing: "
                << to_string(dirEntry);
            continue;
        }

        LedgerEntryType const nodeType{
            safe_cast<LedgerEntryType>((*sleItem)[sfLedgerEntryType])};

        if (nodeType != ltEXPORTED_TXN)
        {
            JLOG(j.warn()) << "ExportedTxn processing: exported directory "
                              "contained non ltEXPORTED_TXN type";
            continue;
        }

        auto const& exported = const_cast<ripple::STLedgerEntry&>(*sleItem)
                                   .getField(sfExportedTxn)
                                   .downcast<STObject>();

        auto exportedLgrSeq = sleItem->getFieldU32(sfLedgerSequence);

        // Only sign transactions that were added in the previous ledger
        if (exportedLgrSeq != seq - 1)
            continue;

        auto s = std::make_shared<ripple::Serializer>();
        exported.add(*s);
        SerialIter sitTrans(s->slice());
        try
        {
            auto const& stpTrans =
                std::make_shared<STTx const>(std::ref(sitTrans));

            if (!stpTrans->isFieldPresent(sfAccount) ||
                stpTrans->getAccountID(sfAccount) == beast::zero)
            {
                JLOG(j.warn())
                    << "Hook: Export failure: sfAccount missing or zero.";
                continue;
            }

            auto txnHash = stpTrans->getTransactionID();

            // Build the multisig for the inner exported transaction
            Serializer sigData = buildMultiSigningData(*stpTrans, signingAcc);
            auto multisig =
                ripple::sign(keys.publicKey, keys.secretKey, sigData.slice());

            // Create the ttEXPORT_SIGN transaction
            auto exportSignTx =
                std::make_shared<STTx>(ttEXPORT_SIGN, [&](auto& obj) {
                    obj.set(([&]() {
                        auto inner = std::make_unique<STObject>(sfSigner);
                        inner->setFieldVL(sfSigningPubKey, keys.publicKey);
                        inner->setAccountID(sfAccount, signingAcc);
                        inner->setFieldVL(sfTxnSignature, multisig);
                        return inner;
                    })());
                    obj.setFieldU32(sfLedgerSequence, seq);
                    obj.setFieldH256(sfTransactionHash, txnHash);
                    obj.setAccountID(sfAccount, calcAccountID(pk));
                    obj.setFieldU32(sfSequence, 0);
                    obj.setFieldVL(sfSigningPubKey, pkSigning.slice());
                    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);

                    if (app.config().NETWORK_ID > 1024)
                        obj.setFieldU32(sfNetworkID, app.config().NETWORK_ID);
                });

            // Sign the outer transaction using our ephemeral key
            exportSignTx->sign(pkSigning, app.getValidationSecretKey());

            JLOG(j.debug())
                << "EXPORT_SIGN txn: " << exportSignTx->getFullText();

            result.push_back(exportSignTx);
        }
        catch (std::exception& e)
        {
            JLOG(j.warn()) << "ExportedTxn Processing: Failure: " << e.what()
                           << "\n";
        }

    } while (
        cdirNext(view, exportedDirKeylet.key, sleDirNode, uDirEntry, dirEntry));

    return result;
}
//@@end make-export-sign-txns

//@@start sign-pending-exports
/**
 * Sign pending exports for ephemeral signature collection.
 *
 * Called during validate() to sign ALL pending ltEXPORTED_TXN entries. The
 * signatures are returned as (txnHash, sfSigner) pairs to be included in the
 * TMValidation message and broadcast to peers.
 *
 * Continuous broadcasting design:
 * ===============================
 * We sign ALL pending exports (not just those from the current ledger) and
 * cache signatures in ExportSignatureCollector. On subsequent calls:
 *
 * 1. If we have a cached signature -> return it (no re-signing needed)
 * 2. If no cached signature -> sign now and it gets cached when stored
 *
 * This ensures:
 * - Late validators can still contribute (they sign when they come online)
 * - Network partitions self-heal on reconnect
 * - Node restarts recover (re-sign from ledger state)
 * - Signatures keep broadcasting until export is finalized
 *
 * The ltEXPORTED_TXN existing in the ledger is the gatekeeper - once it's
 * deleted (after ttEXPORT processed or expired), signatures naturally stop
 * being broadcast. No explicit expiry check needed.
 *
 * @param view The current ledger view being validated
 * @param app The application (for validator keys and UNL)
 * @param j Journal for logging
 * @return Vector of (txnHash, signerObject) pairs to broadcast
 */
std::vector<std::pair<uint256, STObject>>
signPendingExports(
    ReadView const& view,
    Application& app,
    beast::Journal const& j)
{
    std::vector<std::pair<uint256, STObject>> result;

    if (!view.rules().enabled(featureExport))
        return result;

    JLOG(j.debug()) << "signPendingExports: started";

    auto const seq = view.info().seq;

    // If we're not a validator we do nothing here
    if (app.getValidationPublicKey().empty())
        return result;

    auto const& keys = app.getValidatorKeys();

    if (keys.configInvalid())
        return result;

    PublicKey pkSigning = app.getValidationPublicKey();
    auto const pk = app.validatorManifests().getMasterKey(pkSigning);

    // Only continue if we're on the UNLReport
    if (!inUNLReport(view, app, pk, j))
        return result;

    AccountID signingAcc = calcAccountID(pkSigning);

    Keylet const exportedDirKeylet{keylet::exportedDir()};
    if (dirIsEmpty(view, exportedDirKeylet))
        return result;

    std::shared_ptr<SLE const> sleDirNode{};
    unsigned int uDirEntry{0};
    uint256 dirEntry{beast::zero};

    if (!cdirFirst(
            view, exportedDirKeylet.key, sleDirNode, uDirEntry, dirEntry))
        return result;

    do
    {
        Keylet const itemKeylet{ltCHILD, dirEntry};
        auto sleItem = view.read(itemKeylet);
        if (!sleItem)
        {
            JLOG(j.warn()) << "signPendingExports: directory node in ledger "
                           << seq << " has index to object that is missing: "
                           << to_string(dirEntry);
            continue;
        }

        LedgerEntryType const nodeType{
            safe_cast<LedgerEntryType>((*sleItem)[sfLedgerEntryType])};

        if (nodeType != ltEXPORTED_TXN)
        {
            JLOG(j.warn()) << "signPendingExports: exported directory "
                              "contained non ltEXPORTED_TXN type";
            continue;
        }

        auto const& exported = const_cast<ripple::STLedgerEntry&>(*sleItem)
                                   .getField(sfExportedTxn)
                                   .downcast<STObject>();

        // Parse the exported transaction to get its hash
        auto s = std::make_shared<ripple::Serializer>();
        exported.add(*s);
        SerialIter sitTrans(s->slice());
        try
        {
            auto const& stpTrans =
                std::make_shared<STTx const>(std::ref(sitTrans));

            if (!stpTrans->isFieldPresent(sfAccount) ||
                stpTrans->getAccountID(sfAccount) == beast::zero)
            {
                JLOG(j.warn())
                    << "signPendingExports: sfAccount missing or zero.";
                continue;
            }

            auto txnHash = stpTrans->getTransactionID();

            // Check if we already have our signature cached in the collector.
            // This enables continuous broadcasting: we sign once, then keep
            // re-broadcasting our cached signature every ledger until the
            // export is finalized (ltEXPORTED_TXN deleted).
            auto& collector = app.getExportSignatureCollector();
            auto cachedSig = collector.getSignatureFrom(txnHash, pkSigning);

            if (cachedSig)
            {
                // Use cached signature - no need to re-sign
                JLOG(j.info()) << "[EXPORT-TIMING] signPendingExports: using "
                                  "CACHED signature for "
                               << txnHash;
                result.emplace_back(txnHash, *cachedSig);
                continue;
            }

            // First time seeing this export - sign it now
            JLOG(j.info())
                << "[EXPORT-TIMING] signPendingExports: signing FRESH for "
                << txnHash;

            // Build the multisig for the exported transaction
            Serializer sigData = buildMultiSigningData(*stpTrans, signingAcc);
            auto multisig =
                ripple::sign(keys.publicKey, keys.secretKey, sigData.slice());

            // Create the sfSigner object
            STObject signer(sfSigner);
            signer.setFieldVL(sfSigningPubKey, keys.publicKey);
            signer.setAccountID(sfAccount, signingAcc);
            signer.setFieldVL(sfTxnSignature, multisig);

            JLOG(j.debug())
                << "signPendingExports: signed export " << txnHash
                << " with validator " << toBase58(TokenType::NodePublic, pk);

            result.emplace_back(txnHash, std::move(signer));
        }
        catch (std::exception& e)
        {
            JLOG(j.warn()) << "signPendingExports: Failure: " << e.what()
                           << "\n";
        }

    } while (
        cdirNext(view, exportedDirKeylet.key, sleDirNode, uDirEntry, dirEntry));

    JLOG(j.debug()) << "signPendingExports: signed " << result.size()
                    << " exports";

    return result;
}
//@@end sign-pending-exports

}  // namespace ripple
