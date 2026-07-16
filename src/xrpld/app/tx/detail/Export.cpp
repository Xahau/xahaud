#include <xrpld/app/consensus/ConsensusExtensions.h>
#include <xrpld/app/consensus/ExportSignatureHarvester.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/AccountID.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/ValidatorBitset.h>

namespace ripple {

NotTEC
Export::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureExport))
        return temDISABLED;

    auto const ret = preflight1(ctx);
    if (!isTesSuccess(ret))
        return ret;

    if (ctx.tx.getFlags() & tfExportMask)
        return temINVALID_FLAG;

    // Exactly one operation: create an Export OR control an existing W.
    bool const hasExport = ctx.tx.isFieldPresent(sfExportedTxn);
    bool const hasControl = ctx.tx.isFieldPresent(sfTransactionHash);

    if (hasExport == hasControl)  // neither or both
        return temMALFORMED;
    if (hasExport && (ctx.tx.getFlags() & tfExportEraseLatch) != 0)
        return temMALFORMED;
    if (hasControl && ctx.tx.getFieldH256(sfTransactionHash).isZero())
        return temMALFORMED;

    if (hasExport)
    {
        bool const hasUniverse = ctx.tx.isFieldPresent(sfExportUniverseHash);
        bool const hasCommittee = ctx.tx.isFieldPresent(sfExportCommittee);
        if (hasUniverse != hasCommittee)
            return temMALFORMED;
        if (hasUniverse)
        {
            if (auto const ter =
                    ExportLedgerOps::validateCommitteeShape(ctx.tx, ctx.j);
                !isTesSuccess(ter))
                return ter;
        }
        else if (!ctx.tx.isFieldPresent(sfEmitDetails))
        {
            return temMALFORMED;
        }
    }
    else if (
        ctx.tx.isFieldPresent(sfExportUniverseHash) ||
        ctx.tx.isFieldPresent(sfExportCommittee))
    {
        return temMALFORMED;
    }

    // Exported transactions can retry across consensus rounds; every retrying
    // export needs an explicit outer expiry. Lifecycle controls are immediate.
    if (hasExport && !ctx.tx.isFieldPresent(sfLastLedgerSequence))
        return temMALFORMED;

    return preflight2(ctx);
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfExportedTxn))
        return tesSUCCESS;

    if (ctx.tx.isFieldPresent(sfExportUniverseHash) &&
        ctx.tx.getFieldH256(sfExportUniverseHash) != ctx.view.info().parentHash)
        return tecEXPORT_UNIVERSE_MISMATCH;

    auto baseTarget = ExportLedgerOps::exportIntentTarget(ctx.tx);
    if (!baseTarget)
        return temMALFORMED;

    if (auto ter =
            ExportLedgerOps::validateExportSigningFields(*baseTarget, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateExportAccount(
            *baseTarget, ctx.tx.getAccountID(sfAccount), ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateNetworkID(
            *baseTarget, ctx.app.config().NETWORK_ID, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateOriginMemoProjection(
            *baseTarget, ctx.app.config().NETWORK_ID, ctx.j);
        !isTesSuccess(ter))
        return ter;

    if (auto ter = ExportLedgerOps::validateTicketSequence(*baseTarget, ctx.j);
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

    // --- Export latch control path (mutually exclusive with export) ---
    if (ctx_.tx.isFieldPresent(sfTransactionHash))
    {
        auto const origin = ctx_.tx.getFieldH256(sfTransactionHash);
        bool const erase = (ctx_.tx.getFlags() & tfExportEraseLatch) != 0;
        return ExportLedgerOps::controlExportLatch(
            view(), ctx_.rawView(), account, origin, erase, j_);
    }

    // --- Export intent path ---
    auto const txId = ctx_.tx.getTransactionID();
    auto const currentSeq = view().info().seq;
    auto baseTarget = ExportLedgerOps::exportIntentTarget(ctx_.tx);
    if (!baseTarget)
        return temMALFORMED;

    auto parentLedger = ctx_.replayParentLedger();
    if (!parentLedger)
        parentLedger = ctx_.app.getLedgerMaster().getLedgerByHash(
            view().info().parentHash);
    if (!parentLedger || parentLedger->info().hash != view().info().parentHash)
        return tefBAD_LEDGER;

    auto const validatorView =
        ctx_.app.getConsensusExtensions().makeActiveValidatorView(parentLedger);
    auto const universeHash = view().info().parentHash;
    if (!validatorView->fromUNLReport || !validatorView->sourceLedgerHash ||
        *validatorView->sourceLedgerHash != universeHash)
        return tecEXPORT_UNIVERSE_MISMATCH;

    Blob committeeBitmap;
    if (ctx_.tx.isFieldPresent(sfExportCommittee))
        committeeBitmap = ctx_.tx.getFieldVL(sfExportCommittee);
    else
    {
        if (validatorView->orderedOriginalMasterKeys.empty() ||
            validatorView->orderedOriginalMasterKeys.size() >
                ExportLimits::maxCommitteeMembers)
            return tecEXPORT_UNIVERSE_MISMATCH;
        committeeBitmap = makeValidatorBitset(
            validatorView->orderedOriginalMasterKeys.size(),
            [](std::size_t) { return true; });
    }

    auto const committee = resolveExportCommittee(
        makeSlice(committeeBitmap),
        validatorView->orderedOriginalMasterKeys.size());
    if (!committee)
        return tecEXPORT_UNIVERSE_MISMATCH;

    auto const targetNetworkID = baseTarget->isFieldPresent(sfNetworkID)
        ? baseTarget->getFieldU32(sfNetworkID)
        : std::uint32_t{0};
    auto identity = ExportOriginMemo::identityForm(
        *baseTarget,
        ExportOriginMemo::Origin{
            ctx_.app.config().NETWORK_ID, targetNetworkID, txId});
    if (!identity)
        return tefINTERNAL;

    if (auto const prune = ExportLedgerOps::pruneExpiredExportLatches(
            view(), ctx_.rawView(), currentSeq, j_);
        !isTesSuccess(prune))
        return prune;

    auto const ter = ExportLedgerOps::createPendingExportLatch(
        view(),
        ctx_.rawView(),
        account,
        ctx_.tx,
        identity.value(),
        universeHash,
        committeeBitmap,
        mPriorBalance,
        j_);
    if (!isTesSuccess(ter))
        return ter;

    JLOG(j_.info()) << "Export: admitted post-validation intent"
                    << " txHash=" << txId << " ledgerSeq=" << currentSeq
                    << " committee=" << committee->members.selected()
                    << " quorum=" << committee->quorum << " result=tesSUCCESS";
    return tesSUCCESS;
}

}  // namespace ripple
