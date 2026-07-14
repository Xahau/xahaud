#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/consensus/ExportSignatureHarvester.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/app/tx/detail/ExportSignatureUpgrader.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>

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

    // Exported transactions can retry across consensus rounds; every retrying
    // export needs an explicit outer expiry.  Cancel-only exports are
    // immediate.
    if (hasExport && !ctx.tx.isFieldPresent(sfLastLedgerSequence))
        return temMALFORMED;

    return preflight2(ctx);
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfExportedTxn))
        return tesSUCCESS;

    auto innerTx = ExportLedgerOps::innerExportedTx(ctx.tx);
    if (!innerTx)
        return temMALFORMED;

    if (auto ter =
            ExportLedgerOps::validateExportSigningFields(*innerTx, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateExportAccount(
            *innerTx, ctx.tx.getAccountID(sfAccount), ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateNetworkID(
            *innerTx, ctx.app.config().NETWORK_ID, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateOriginMemoProjection(
            *innerTx, ctx.app.config().NETWORK_ID, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateTicketSequence(*innerTx, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter =
            ExportLedgerOps::validateRetryWindow(ctx.tx, ctx.view.seq(), ctx.j);
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
        JLOG(j_.info()) << "Export: open ledger apply"
                        << " ledgerSeq=" << currentSeq << " txHash=" << txId
                        << " result=tesSUCCESS"
                        << " provisional=yes";
        return tesSUCCESS;
    }

    auto& consensusExtensions = ctx_.app.getConsensusExtensions();
    bool const standalone = ctx_.app.config().standalone();
    auto const& valKeys = ctx_.app.getValidatorKeys();
    bool const historicalReplay = ctx_.historicalLedgerReplay();
    auto parentLedger = ctx_.replayParentLedger();
    if (parentLedger && parentLedger->info().hash != view().info().parentHash)
    {
        JLOG(j_.warn()) << "Export: ignoring mismatched replay parent"
                        << " txHash=" << txId << " ledgerSeq=" << currentSeq
                        << " parentHash=" << view().info().parentHash
                        << " replayParentHash=" << parentLedger->info().hash;
        if (historicalReplay)
            return terRETRY_EXPORT;
        parentLedger.reset();
    }
    if (historicalReplay && !parentLedger)
    {
        JLOG(j_.warn()) << "Export: retrying historical replay without parent"
                        << " txHash=" << txId << " ledgerSeq=" << currentSeq
                        << " parentHash=" << view().info().parentHash;
        return terRETRY_EXPORT;
    }
    if (!parentLedger)
        parentLedger = ctx_.app.getLedgerMaster().getLedgerByHash(
            view().info().parentHash);
    if (!standalone && !parentLedger)
    {
        JLOG(j_.warn()) << "Export: retrying without parent ledger"
                        << " txHash=" << txId << " ledgerSeq=" << currentSeq
                        << " parentHash=" << view().info().parentHash;
        return terRETRY_EXPORT;
    }

    auto const validatorView =
        consensusExtensions.makeActiveValidatorView(parentLedger);
    bool const trustWitnessMembership =
        ctx_.trustExportSignatureWitnessMembership();
    // Direct/apply-test callers that supply a witness without consensus
    // materialization use this live-manifest filter. Live builds and replay
    // set trustWitnessMembership, so apply does not re-resolve accepted witness
    // signers through mutable manifests.
    auto const isActiveSigner = [standalone,
                                 &valKeys,
                                 &consensusExtensions,
                                 validatorView](PublicKey const& key) {
        if (standalone)
            return valKeys.keys && key == valKeys.keys->publicKey;
        return consensusExtensions.isActiveValidator(key, *validatorView);
    };
    auto const isWitnessSigner = [trustWitnessMembership,
                                  &isActiveSigner](PublicKey const& key) {
        if (trustWitnessMembership)
            return true;
        return isActiveSigner(key);
    };
    // Closed-ledger export builds a local parent-ledger validator view, not the
    // mutable apply view or cached RNG state, so apply order cannot move
    // quorum.
    auto const unlSize = validatorView->size();
    auto const originalUNLSize = validatorView->originalViewSize;

    // Network mode: active-view 80% quorum. Standalone mode uses a local
    // one-signer witness synthesized in onPreBuild. Both paths consume the same
    // ttEXPORT_SIGNATURES replay witness before creating ledger effects.
    // Deserialize the inner tx early — needed both for the upgrade
    // pass (verify unverified sigs) and for blob assembly.
    auto innerTx = ExportLedgerOps::innerExportedTx(ctx_.tx);
    if (!innerTx)
    {
        JLOG(j_.warn()) << "Export: malformed inner exported tx"
                        << " txHash=" << txId << " ledgerSeq=" << currentSeq;
        return temMALFORMED;
    }

    auto upgradeUnverifiedForNextRound = [&]() {
        if (standalone)
            return;

        // Closed-ledger apply must not create new current-round quorum
        // material. These upgrades are retained for a retrying export, where
        // the sidecar alignment gate can publish and converge them first.
        // Upgrade only active-view signatures; inactive trusted signatures may
        // stay cached, but they must not become quorum material.
        ExportSignatureUpgrader::upgradeUnverifiedSignatures(
            consensusExtensions.exportSigCollector(),
            *innerTx,
            txId,
            currentSeq,
            isActiveSigner,
            j_);
    };

    ExportResultBuilder::SignatureSnapshot signatures;
    std::optional<uint256> exportSignatureHash;

    //@@start export-doapply-replay-witness-snapshot
    std::size_t const threshold = standalone
        ? 1
        : ConsensusExtensions::exportWitnessThreshold(*validatorView);

    bool const activeViewFitsTarget =
        standalone ||
        ConsensusExtensions::exportAuthorityFitsTargetSignerCap(
            *validatorView, STTx::maxMultiSigners());
    if (!activeViewFitsTarget)
    {
        JLOG(j_.warn())
            << "Export: retrying because source authority exceeds target "
               "signer cap"
            << " txHash=" << txId << " ledgerSeq=" << currentSeq
            << " unlSize=" << unlSize << " originalUNLSize=" << originalUNLSize
            << " targetSignerCap=" << STTx::maxMultiSigners();
    }
    else if (!standalone && !validatorView->fromUNLReport)
    {
        JLOG(j_.warn())
            << "Export: retrying without ledger-anchored validator view"
            << " txHash=" << txId << " ledgerSeq=" << currentSeq
            << " unlSize=" << unlSize << " threshold=" << threshold;
    }
    else
    {
        // ttEXPORT_SIGNATURES is the export signature interface. Consensus
        // sidecars, standalone helpers, or replay all hand signatures to Export
        // through the same transaction-stream witness; apply re-checks the
        // witness against this parent ledger before creating ledger effects.
        //
        // In live consensus builds, onPreBuild has scrubbed any pre-existing
        // witness and re-materialized this one from the accepted sidecar root.
        // Historical replay consumes the same persisted witness after manifests
        // may have rotated. In both modes, membership comes from the validated
        // tx stream; apply still verifies every signature and requires quorum.
        if (auto witness = ctx_.exportSignatureWitness(txId))
        {
            exportSignatureHash = witness->witnessHash;
            for (auto const& [pk, sig] : witness->signatures)
            {
                if (!isWitnessSigner(pk))
                    continue;

                if (!verifyExportSignatureAgainstTx(
                        ctx_.tx,
                        pk,
                        Slice(sig.data(), sig.size()),
                        txId,
                        j_,
                        "export signature witness"))
                    continue;

                signatures.emplace(pk, sig);
            }
        }
    }
    //@@end export-doapply-replay-witness-snapshot

    //@@start export-doapply-retry-without-signature-quorum
    if (signatures.size() < threshold)
    {
        auto const sigCount = historicalReplay
            ? std::size_t{0}
            : consensusExtensions.exportSigCollector().signatureCount(
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
                if (!historicalReplay)
                    ctx_.app.getConsensusExtensions()
                        .exportSigCollector()
                        .clear(txId);
                JLOG(j_.info())
                    << "Export: last ledger expired"
                    << " txHash=" << txId << " ledgerSeq=" << currentSeq
                    << " lastLedgerSequence=" << lls << " sigs=" << sigCount
                    << " threshold=" << threshold << " unlSize=" << unlSize
                    << " result=tecEXPORT_EXPIRED";
                return tecEXPORT_EXPIRED;
            }
        }

        if (!historicalReplay)
            upgradeUnverifiedForNextRound();

        JLOG(j_.info()) << "Export: insufficient signatures"
                        << " txHash=" << txId << " ledgerSeq=" << currentSeq
                        << " witnessSigs=" << signatures.size()
                        << " collectorSigs=" << sigCount
                        << " threshold=" << threshold << " unlSize=" << unlSize
                        << " exportSigConvergenceFailed="
                        << (consensusExtensions.exportSigConvergenceFailed()
                                ? "yes"
                                : "no")
                        << " result=terRETRY_EXPORT";
        return terRETRY_EXPORT;
    }
    //@@end export-doapply-retry-without-signature-quorum

    if (!exportSignatureHash)
    {
        JLOG(j_.fatal()) << "Export: quorum signatures without witness hash"
                         << " txHash=" << txId << " ledgerSeq=" << currentSeq
                         << " signatures=" << signatures.size();
        return tefINTERNAL;
    }

    auto assembled = ExportResultBuilder::assembleClosedLedger(
        *innerTx, signatures, currentSeq, txId, *exportSignatureHash);

    // Bind the callback latch to the unsigned target intent. Different valid
    // signer subsets produce different transaction IDs on the target chain.
    {
        TER ter = ExportLedgerOps::createShadowTicket(
            view(), account, *innerTx, mPriorBalance, j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    // Write the export result to metadata. The metadata references the replay
    // witness pseudo instead of duplicating the validator signatures; clients
    // assemble the destination transaction from ttEXPORT + ttEXPORT_SIGNATURES
    // using the same canonical signer order and target-chain cap as
    // ExportResultBuilder.
    auto* avi = dynamic_cast<ApplyViewImpl*>(&view());
    if (!avi)
    {
        JLOG(j_.fatal()) << "Export: cannot write ExportResult metadata"
                         << " txHash=" << txId << " ledgerSeq=" << currentSeq
                         << " reason=view-not-ApplyViewImpl";
        return tefINTERNAL;
    }
    avi->setExportResultMetaData(std::move(assembled.metadata));

    // Clean up the collector.
    if (!historicalReplay)
        ctx_.app.getConsensusExtensions().exportSigCollector().clear(txId);

    JLOG(j_.info()) << "Export: success"
                    << " txHash=" << txId << " ledgerSeq=" << currentSeq
                    << " signers=" << assembled.signerCount << " mode="
                    << (ctx_.app.config().standalone() ? "standalone"
                                                       : "network")
                    << " result=tesSUCCESS";

    return tesSUCCESS;
}

}  // namespace ripple
