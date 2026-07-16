#include <xrpld/app/consensus/ConsensusExtensions.h>
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

    if (ctx.tx.getFlags() & tfExportMask)
        return temINVALID_FLAG;

    bool const hasExport = ctx.tx.isFieldPresent(sfExportedTxn);
    bool const hasControl = ctx.tx.isFieldPresent(sfTransactionHash);
    bool const hasDigest = ctx.tx.isFieldPresent(sfExportCommitteeHash);
    bool const hasRoster = ctx.tx.isFieldPresent(sfExportCommittee);
    auto const flags = ctx.tx.getFlags() & ~tfUniversal;

    if (ctx.tx.isFieldPresent(sfEmitDetails) && (!hasExport || hasRoster))
        return temMALFORMED;

    // Intent: target + digest, with optional matching creation roster.
    if (hasExport)
    {
        if (hasControl || !hasDigest || flags != 0)
            return temMALFORMED;
        if (auto const ter =
                ExportLedgerOps::validateCommitteeBinding(ctx.tx, ctx.j);
            !isTesSuccess(ter))
            return ter;
        if (!ctx.tx.isFieldPresent(sfLastLedgerSequence))
            return temMALFORMED;
    }
    // Latch control: W, optionally with irreversible erase.
    else if (hasControl)
    {
        if (hasDigest || hasRoster ||
            (flags != 0 && flags != tfExportEraseLatch) ||
            ctx.tx.getFieldH256(sfTransactionHash).isZero())
            return temMALFORMED;
    }
    // Committee setup: roster only. Its digest is derived canonically.
    else if (hasRoster)
    {
        if (hasDigest || flags != 0)
            return temMALFORMED;
        if (auto const ter = ExportLedgerOps::validateCommitteeRoster(
                makeSlice(ctx.tx.getFieldVL(sfExportCommittee)), ctx.j);
            !isTesSuccess(ter))
            return ter;
    }
    // Committee deletion: exact digest plus its dedicated flag.
    else if (hasDigest)
    {
        if (flags != tfExportEraseCommittee ||
            ctx.tx.getFieldH256(sfExportCommitteeHash).isZero())
            return temMALFORMED;
    }
    else
        return temMALFORMED;

    return preflight2(ctx);
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.tx.isFieldPresent(sfExportedTxn))
        return tesSUCCESS;

    auto const account = ctx.tx.getAccountID(sfAccount);
    auto const digest = ctx.tx.getFieldH256(sfExportCommitteeHash);
    if (!ctx.tx.isFieldPresent(sfExportCommittee))
    {
        auto const committee =
            ctx.view.read(keylet::exportCommittee(account, digest));
        if (!committee)
            return tecNO_ENTRY;
        if (committee->getType() != ltEXPORT_COMMITTEE ||
            !committee->isFieldPresent(sfExportCommittee) ||
            !ExportLedgerOps::isMatchingExportCommittee(
                *committee,
                account,
                digest,
                makeSlice(committee->getFieldVL(sfExportCommittee))))
            return tefBAD_LEDGER;
    }

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

    if (auto ter = ExportLedgerOps::validateAdmissionWindow(
            ctx.tx, ctx.view.seq(), ctx.j);
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

    bool const hasExport = ctx_.tx.isFieldPresent(sfExportedTxn);
    bool const hasRoster = ctx_.tx.isFieldPresent(sfExportCommittee);
    bool const hasDigest = ctx_.tx.isFieldPresent(sfExportCommitteeHash);

    // --- Immutable committee deletion path ---
    if (!hasExport && hasDigest)
        return ExportLedgerOps::eraseExportCommittee(
            view(),
            ctx_.rawView(),
            account,
            ctx_.tx.getFieldH256(sfExportCommitteeHash),
            j_);

    Blob roster;
    uint256 committeeDigest;
    if (hasRoster)
    {
        auto const canonical = canonicalizeExportCommittee(
            makeSlice(ctx_.tx.getFieldVL(sfExportCommittee)));
        if (!canonical)
            return temMALFORMED;
        roster = std::move(*canonical);
        committeeDigest = exportCommitteeHash(makeSlice(roster));
        if (committeeDigest.isZero() ||
            (hasDigest &&
             committeeDigest != ctx_.tx.getFieldH256(sfExportCommitteeHash)))
            return temMALFORMED;
    }
    else
    {
        if (!hasDigest)
            return temMALFORMED;
        committeeDigest = ctx_.tx.getFieldH256(sfExportCommitteeHash);
        auto const committeeSLE =
            view().read(keylet::exportCommittee(account, committeeDigest));
        if (!committeeSLE || !committeeSLE->isFieldPresent(sfExportCommittee))
            return tecNO_ENTRY;
        roster = committeeSLE->getFieldVL(sfExportCommittee);
        if (!ExportLedgerOps::isMatchingExportCommittee(
                *committeeSLE, account, committeeDigest, makeSlice(roster)))
            return tefBAD_LEDGER;
    }

    auto const committee = resolveExportCommittee(makeSlice(roster));
    if (!committee)
        return temMALFORMED;

    // Bare committee setup performs no signing or latch work. Eligibility is
    // evaluated when an intent actually selects the committee.
    if (!hasExport)
        return ExportLedgerOps::createExportCommittee(
            view(),
            ctx_.rawView(),
            account,
            makeSlice(roster),
            mPriorBalance,
            j_);

    auto parentLedger = ctx_.replayParentLedger();
    if (!parentLedger)
        parentLedger = ctx_.app.getLedgerMaster().getLedgerByHash(
            view().info().parentHash);
    if (!parentLedger || parentLedger->info().hash != view().info().parentHash)
        return tefBAD_LEDGER;

    auto const validatorView =
        ctx_.app.getConsensusExtensions().makeActiveValidatorView(parentLedger);
    if (!validatorView->fromUNLReport || !validatorView->sourceLedgerHash ||
        *validatorView->sourceLedgerHash != view().info().parentHash)
        return tecEXPORT_COMMITTEE_UNAVAILABLE;

    for (auto const& master : committee->members)
    {
        if (!std::binary_search(
                validatorView->orderedOriginalMasterKeys.begin(),
                validatorView->orderedOriginalMasterKeys.end(),
                master))
            return tecEXPORT_COMMITTEE_UNAVAILABLE;
    }

    if (hasRoster)
    {
        auto const ter = ExportLedgerOps::createExportCommittee(
            view(),
            ctx_.rawView(),
            account,
            makeSlice(roster),
            mPriorBalance,
            j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    // --- Export intent path ---
    auto const txId = ctx_.tx.getTransactionID();
    auto const currentSeq = view().info().seq;
    auto baseTarget = ExportLedgerOps::exportIntentTarget(ctx_.tx);
    if (!baseTarget)
        return temMALFORMED;

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
        committeeDigest,
        mPriorBalance,
        j_);
    if (!isTesSuccess(ter))
        return ter;

    JLOG(j_.info()) << "Export: admitted post-validation intent"
                    << " txHash=" << txId << " ledgerSeq=" << currentSeq
                    << " committee=" << committee->members.size()
                    << " quorum=" << committee->quorum << " result=tesSUCCESS";
    return tesSUCCESS;
}

}  // namespace ripple
