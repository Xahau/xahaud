#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
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

    if (auto ter = ExportLedgerOps::checkExportTxnLimit(ctx.view, ctx.j);
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

    auto& consensusExtensions = ctx_.app.getConsensusExtensions();
    auto const parentLedger =
        ctx_.app.getLedgerMaster().getLedgerByHash(view().info().parentHash);
    auto const validatorView =
        consensusExtensions.makeActiveValidatorView(parentLedger);
    auto const isActiveSigner = [&consensusExtensions,
                                 validatorView](PublicKey const& key) {
        return consensusExtensions.isActiveValidator(key, *validatorView);
    };
    // Closed-ledger export builds a local parent-ledger validator view, not the
    // mutable apply view or cached RNG state, so apply order cannot move
    // quorum.
    auto const unlSize = validatorView->size();

    // Standalone mode: no consensus running, so we skip the quorum
    // check and sign directly with our validator keys in the blob
    // assembly step below.
    //
    // Network mode: active-view 80% quorum. Export-only rounds are still
    // deterministic because exportSigSetHash is signed in ExtendedPosition and
    // converged before closed-ledger apply can use the signatures.
    // Deserialize the inner tx early — needed both for the upgrade
    // pass (verify unverified sigs) and for blob assembly.
    auto const& exportedObj =
        ctx_.tx.peekAtField(sfExportedTxn).downcast<STObject>();

    Serializer innerSer;
    exportedObj.add(innerSer);
    SerialIter sit(innerSer.slice());

    STTx innerTx(std::ref(sit));

    auto upgradeUnverifiedForNextRound = [&]() {
        if (ctx_.app.config().standalone())
            return;

        // Closed-ledger apply must not create new current-round quorum
        // material. These upgrades are retained for a retrying export, where
        // the sidecar alignment gate can publish and converge them first.
        auto& collector = consensusExtensions.exportSigCollector();
        auto const unverified = collector.unverifiedSignatures(txId);
        for (auto const& [valPK, sigBuf] : unverified)
        {
            // Upgrade only active-view signatures; inactive trusted signatures
            // may stay cached, but they must not become quorum material.
            if (!isActiveSigner(valPK))
                continue;

            auto const signerAcctID = calcAccountID(valPK);
            auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
            if (verify(
                    valPK,
                    sigData.slice(),
                    Slice(sigBuf.data(), sigBuf.size())))
            {
                collector.upgradeSignature(txId, valPK, sigBuf, currentSeq);
            }
            else
            {
                JLOG(j_.warn()) << "Export: upgrade verify failed for tx "
                                << txId << " — removing invalid sig";
                collector.removeSignature(txId, valPK, sigBuf);
            }
        }
    };

    // Atomic quorum check + snapshot for network mode.
    // Only verified signatures count toward quorum and appear
    // in the snapshot.
    std::optional<std::map<PublicKey, Buffer>> collectedSigs;

    if (!ctx_.app.config().standalone())
    {
        std::size_t const threshold =
            unlSize == 0 ? 1 : calculateQuorumThreshold(unlSize);

        // The collector may contain old trusted signatures; quorum counts only
        // signatures whose keys resolve into the same frozen active view.
        if (!consensusExtensions.exportSigConvergenceFailed())
        {
            collectedSigs =
                consensusExtensions.exportSigCollector().checkQuorumAndSnapshot(
                    txId, threshold, isActiveSigner);
        }

        if (!collectedSigs)
        {
            auto const sigCount =
                consensusExtensions.exportSigCollector().signatureCount(
                    txId, isActiveSigner);
            // LLS semantics for retriable exports:
            //
            // Transactor::preclaim rejects with tefMAX_LEDGER when
            // seq > LLS, so this tx can never run past ledger LLS.
            // Within that window the export has three possible outcomes
            // each ledger:
            //
            //   ledger < LLS:  tesSUCCESS (quorum) or terRETRY_EXPORT
            //   ledger == LLS: tesSUCCESS (quorum) or tecEXPORT_EXPIRED
            //   ledger > LLS:  tefMAX_LEDGER (never reaches doApply)
            //
            // The >= check here only fires in the no-quorum branch, so
            // if quorum IS met on the LLS ledger it still succeeds.
            // tecEXPORT_EXPIRED consumes the sequence cleanly rather
            // than letting tefMAX_LEDGER silently drop the tx.
            if (ctx_.tx.isFieldPresent(sfLastLedgerSequence))
            {
                auto const lls = ctx_.tx.getFieldU32(sfLastLedgerSequence);
                if (currentSeq >= lls)
                {
                    ctx_.app.getConsensusExtensions()
                        .exportSigCollector()
                        .clear(txId);
                    JLOG(j_.info()) << "Export: LLS expired at ledger "
                                    << currentSeq << " sigs=" << sigCount << "/"
                                    << threshold << " -> tecEXPORT_EXPIRED";
                    return tecEXPORT_EXPIRED;
                }
            }

            upgradeUnverifiedForNextRound();

            JLOG(j_.info())
                << "Export: not enough sigs at ledger " << currentSeq
                << " sigs=" << sigCount << " threshold=" << threshold
                << " unlSize=" << unlSize << " exportSigConvergenceFailed="
                << (consensusExtensions.exportSigConvergenceFailed() ? "yes"
                                                                     : "no")
                << " -> terRETRY_EXPORT";
            return terRETRY_EXPORT;
        }
    }

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
        // Network mode: use the atomically-snapshotted sigs from
        // the quorum check above.
        for (auto const& [valPK, sigBuf] : *collectedSigs)
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

    // Sort signers by AccountID (required by XRPL multisign).
    std::sort(
        signers.begin(),
        signers.end(),
        [](STObject const& a, STObject const& b) {
            return a.getAccountID(sfAccount) < b.getAccountID(sfAccount);
        });

    // Build the multisigned tx.  Use sfExportedTxn as the field type
    // so it nests properly in ExportResult metadata as readable JSON.
    STObject multiSigned(sfExportedTxn);
    {
        // Copy all non-signing fields from innerTx, then we'll add
        // signing fields (empty SigningPubKey + Signers) below.
        Serializer s;
        innerTx.addWithoutSigningFields(s);
        SerialIter sit(s.slice());
        multiSigned.set(sit);
    }

    // Set empty SigningPubKey (indicates multisigned).
    multiSigned.setFieldVL(sfSigningPubKey, Slice{});

    if (signers.size() > 0)
        multiSigned.setFieldArray(sfSigners, signers);

    // Compute the signed tx hash for the shadow ticket.
    // getHash(transactionID) includes ALL fields (Signers etc.),
    // matching what STTx::getTransactionID() produces.
    auto const signedTxHash = multiSigned.getHash(HashPrefix::transactionID);

    // Create the shadow ticket with the signed tx hash.
    {
        TER ter = ExportLedgerOps::createShadowTicket(
            view(), account, innerTx, signedTxHash, j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    // Write the export result to metadata.  The multisigned tx is
    // stored as sfExportedTxn (OBJECT) so it renders as readable
    // JSON in metadata, not an opaque hex blob.
    STObject exportResult(sfExportResult);
    exportResult.setFieldU32(sfLedgerSequence, currentSeq);
    exportResult.setFieldH256(sfTransactionHash, txId);
    exportResult.set(std::move(multiSigned));

    auto* avi = dynamic_cast<ApplyViewImpl*>(&view());
    if (!avi)
    {
        JLOG(j_.fatal()) << "Export: cannot write ExportResult metadata "
                         << "(view is not ApplyViewImpl)";
        return tefINTERNAL;
    }
    avi->setExportResultMetaData(std::move(exportResult));

    // Clean up the collector.
    ctx_.app.getConsensusExtensions().exportSigCollector().clear(txId);

    JLOG(j_.info()) << "Export: success at ledger " << currentSeq
                    << (ctx_.app.config().standalone() ? " (standalone)"
                                                       : " (quorum met)")
                    << " -> tesSUCCESS";

    return tesSUCCESS;
}

}  // namespace ripple
