#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpld/app/misc/ValidatorList.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/ledger/ApplyViewImpl.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
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
    // Standalone / unit tests: no real UNL, just require 1 sig.
    // With CE: 80% quorum (SHAMap convergence ensures deterministic agreement).
    // Without CE: unanimity (avoids non-deterministic quorum disagreement).
    std::size_t threshold;
    if (unlSize == 0 || ctx_.app.config().standalone())
        threshold = 1;
    else if (view().rules().enabled(featureConsensusEntropy))
        threshold = calculateQuorumThreshold(unlSize);
    else
        threshold = unlSize;
    auto const sigCount = exportSigCollector().signatureCount(txId);

    if (sigCount < threshold)
    {
        // If we're at or past LLS, give up with tecEXPORT_EXPIRED so the
        // sequence is consumed and subsequent txns aren't blocked forever.
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

        JLOG(j_.info()) << "Export: not enough sigs at ledger " << currentSeq
                        << " sigs=" << sigCount << " threshold=" << threshold
                        << " unlSize=" << unlSize << " -> terRETRY_EXPORT";
        return terRETRY_EXPORT;
    }

    // Quorum met — create shadow ticket from inner tx.
    {
        auto const& exportedObj =
            ctx_.tx.peekAtField(sfExportedTxn).downcast<STObject>();

        Serializer s;
        exportedObj.add(s);
        SerialIter sit(s.slice());

        STTx exportedTx(std::ref(sit));

        TER ter = ExportLedgerOps::createShadowTicket(
            view(), account, exportedTx, exportedTx.getTransactionID(), j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    // Write the export result to metadata.
    STObject exportResult(sfExportResult);
    exportResult.setFieldU32(sfLedgerSequence, currentSeq);
    exportResult.setFieldH256(sfTransactionHash, txId);

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

    JLOG(j_.info()) << "Export: quorum met at ledger " << currentSeq
                    << " sigs=" << sigCount << "/" << threshold
                    << " -> tesSUCCESS";

    return tesSUCCESS;
}

}  // namespace ripple
