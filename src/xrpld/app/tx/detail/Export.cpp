#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <algorithm>

namespace ripple {

NotTEC
Export::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureExport))
        return temDISABLED;

    auto const ret = preflight1(ctx);
    if (!isTesSuccess(ret))
        return ret;

    // Exactly one operation: export OR cancel, not both.
    bool const hasExport = ctx.tx.isFieldPresent(sfExportedTxn);
    bool const hasCancel = ctx.tx.isFieldPresent(sfCancelTicketSequence);

    if (hasExport == hasCancel)  // neither or both
        return temMALFORMED;

    return preflight2(ctx);
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfExportedTxn))
        return tesSUCCESS;

    // Validate the inner exported transaction.
    auto const& exportedObj = const_cast<STTx&>(ctx.tx)
                                  .peekAtField(sfExportedTxn)
                                  .downcast<STObject>();

    Serializer s;
    exportedObj.add(s);
    SerialIter sit(s.slice());

    std::shared_ptr<STTx const> stpTrans;
    try
    {
        stpTrans = std::make_shared<STTx const>(sit);
    }
    catch (std::exception const&)
    {
        return temMALFORMED;
    }

    if (auto ter = ExportLedgerOps::validateExportAccount(
            *stpTrans, ctx.tx.getAccountID(sfAccount), ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateNetworkID(
            *stpTrans, ctx.app.config().NETWORK_ID, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateTicketSequence(*stpTrans, ctx.j);
        !isTesSuccess(ter))
        return ter;

    return tesSUCCESS;
}

TER
Export::doApply()
{
    auto const account = ctx_.tx.getAccountID(sfAccount);

    // --- Shadow ticket cancel path (mutually exclusive with export) ---
    if (ctx_.tx.isFieldPresent(sfCancelTicketSequence))
    {
        auto const ticketSeq = ctx_.tx.getFieldU32(sfCancelTicketSequence);
        return ExportLedgerOps::cancelShadowTicket(
            view(), account, ticketSeq, j_);
    }

    // --- Export path ---

    auto const txId = ctx_.tx.getTransactionID();
    auto const currentSeq = view().info().seq;

    // Open ledger: return tesSUCCESS to consume sequence + fee and
    // get the transaction relayed/broadcast to all validators.
    if (view().open())
    {
        JLOG(j_.info()) << "Export: open ledger at " << currentSeq
                        << " -> tesSUCCESS (provisional)";
        return tesSUCCESS;
    }

    // Closed ledger: check if we have enough validator signatures.
    // UNL size from UNLReport ActiveValidators, fallback to local trusted keys.
    std::size_t unlSize = 0;
    {
        auto const unlReport = view().read(keylet::UNLReport());
        if (unlReport && unlReport->isFieldPresent(sfActiveValidators))
            unlSize = unlReport->getFieldArray(sfActiveValidators).size();
        else
            unlSize = ctx_.app.validators().getTrustedMasterKeys().size();
    }
    // Standalone mode: no consensus running, so we skip the quorum
    // check and sign directly with our validator keys in the blob
    // assembly step below.
    //
    // Network mode:
    //   With CE: 80% quorum (SHAMap convergence ensures agreement).
    //   Without CE: unanimity (avoids non-deterministic disagreement).
    if (!ctx_.app.config().standalone())
    {
        std::size_t threshold;
        if (unlSize == 0)
            threshold = 1;
        else if (view().rules().enabled(featureConsensusEntropy))
            threshold = calculateQuorumThreshold(unlSize);
        else
            threshold = unlSize;
        auto const sigCount = exportSigCollector().signatureCount(txId);

        if (sigCount < threshold)
        {
            if (ctx_.tx.isFieldPresent(sfLastLedgerSequence))
            {
                auto const lls = ctx_.tx.getFieldU32(sfLastLedgerSequence);
                if (currentSeq >= lls)
                {
                    exportSigCollector().clear(txId);
                    JLOG(j_.info()) << "Export: LLS expired at ledger "
                                    << currentSeq << " sigs=" << sigCount << "/"
                                    << threshold << " -> tecEXPORT_EXPIRED";
                    return tecEXPORT_EXPIRED;
                }
            }

            JLOG(j_.info())
                << "Export: not enough sigs at ledger " << currentSeq
                << " sigs=" << sigCount << " threshold=" << threshold
                << " unlSize=" << unlSize << " -> terRETRY_EXPORT";
            return terRETRY_EXPORT;
        }
    }

    // Build the multisigned transaction blob FIRST, then use its
    // hash for the shadow ticket.  getTransactionID() includes ALL
    // fields (including Signers), so the shadow ticket must store
    // the hash of the final signed blob — not the unsigned inner tx.

    auto const& exportedObj =
        ctx_.tx.peekAtField(sfExportedTxn).downcast<STObject>();

    Serializer innerSer;
    exportedObj.add(innerSer);
    SerialIter sit(innerSer.slice());

    STTx innerTx(std::ref(sit));

    STArray signers(sfSigners);

    if (ctx_.app.config().standalone())
    {
        // Standalone mode: no consensus proposals, so we sign
        // the inner tx directly with our own validator keys.
        auto const& valKeys = ctx_.app.getValidatorKeys();
        if (valKeys.keys)
        {
            auto const& pk = valKeys.keys->publicKey;
            auto const& sk = valKeys.keys->secretKey;
            auto const signerAcctID = calcAccountID(pk);

            auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
            auto const sig = ripple::sign(pk, sk, sigData.slice());

            STObject signer(sfSigner);
            signer.setAccountID(sfAccount, signerAcctID);
            signer.setFieldVL(sfSigningPubKey, pk.slice());
            signer.setFieldVL(sfTxnSignature, sig);
            signers.push_back(std::move(signer));
        }
    }
    else
    {
        // Network mode: collect real signatures from peers
        // via ExportSigCollector (populated from proposals).
        auto const allSigs = exportSigCollector().snapshotWithSigs();
        auto it = allSigs.find(txId);

        if (it != allSigs.end())
        {
            for (auto const& [valPK, sigBuf] : it->second)
            {
                if (sigBuf.size() == 0)
                    continue;  // pubkey-only, no real signature

                STObject signer(sfSigner);
                signer.setAccountID(sfAccount, calcAccountID(valPK));
                signer.setFieldVL(sfSigningPubKey, valPK.slice());
                signer.setFieldVL(
                    sfTxnSignature, Slice(sigBuf.data(), sigBuf.size()));
                signers.push_back(std::move(signer));
            }
        }
    }

    // Sort signers by AccountID (required by XRPL multisign).
    std::sort(
        signers.begin(),
        signers.end(),
        [](STObject const& a, STObject const& b) {
            return a.getAccountID(sfAccount) < b.getAccountID(sfAccount);
        });

    // Build the multisigned STTx: inner tx + empty SigningPubKey
    // + Signers array.
    STObject multiSigned(innerTx);

    if (multiSigned.isFieldPresent(sfTxnSignature))
        multiSigned.makeFieldAbsent(sfTxnSignature);

    multiSigned.setFieldVL(sfSigningPubKey, Slice{});

    if (signers.size() > 0)
        multiSigned.setFieldArray(sfSigners, signers);

    // Serialize to get the blob, then deserialize as STTx to
    // compute the correct transaction ID (includes Signers).
    Serializer outSer;
    multiSigned.add(outSer);
    Blob multisignedBlob = outSer.peekData();

    SerialIter finalSit(outSer.slice());
    STTx const signedTx(std::ref(finalSit));
    auto const signedTxHash = signedTx.getTransactionID();

    // Now create the shadow ticket with the signed tx hash.
    {
        TER ter = ExportLedgerOps::createShadowTicket(
            view(), account, innerTx, signedTxHash, j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    // Write the export result to metadata.
    STObject exportResult(sfExportResult);
    exportResult.setFieldU32(sfLedgerSequence, currentSeq);
    exportResult.setFieldH256(sfTransactionHash, txId);
    if (!multisignedBlob.empty())
        exportResult.setFieldVL(sfBlob, multisignedBlob);

    auto* avi = dynamic_cast<ApplyViewImpl*>(&view());
    if (!avi)
    {
        JLOG(j_.fatal()) << "Export: cannot write ExportResult metadata "
                         << "(view is not ApplyViewImpl)";
        return tefINTERNAL;
    }
    avi->setExportResultMetaData(std::move(exportResult));

    // Clean up the collector.
    exportSigCollector().clear(txId);

    JLOG(j_.info()) << "Export: success at ledger " << currentSeq
                    << (ctx_.app.config().standalone() ? " (standalone)"
                                                       : " (quorum met)")
                    << " -> tesSUCCESS";

    return tesSUCCESS;
}

}  // namespace ripple
