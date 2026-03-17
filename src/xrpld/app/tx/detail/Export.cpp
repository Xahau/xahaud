#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpl/protocol/Feature.h>
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

    // At least one operation must be present.
    bool const hasExport = ctx.tx.isFieldPresent(sfExportedTxn);
    bool const hasCancel = ctx.tx.isFieldPresent(sfCancelTicketSequence);

    if (!hasExport && !hasCancel)
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

    // Export operation: create ltEXPORTED_TXN + ltSHADOW_TICKET.
    if (ctx_.tx.isFieldPresent(sfExportedTxn))
    {
        auto const& exportedObj =
            ctx_.tx.peekAtField(sfExportedTxn).downcast<STObject>();

        Serializer s;
        exportedObj.add(s);
        SerialIter sit(s.slice());

        STTx exportedTx(std::ref(sit));
        uint256 const txnId = exportedTx.getTransactionID();

        TER ter = ExportLedgerOps::createExportedTxn(
            view(), ctx_.app, exportedTx, txnId, j_);
        if (!isTesSuccess(ter))
            return ter;

        ter = ExportLedgerOps::createShadowTicket(
            view(), account, exportedTx, txnId, j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    // Cancel operation: delete an existing shadow ticket.
    if (ctx_.tx.isFieldPresent(sfCancelTicketSequence))
    {
        auto const ticketSeq = ctx_.tx.getFieldU32(sfCancelTicketSequence);

        TER ter =
            ExportLedgerOps::cancelShadowTicket(view(), account, ticketSeq, j_);
        if (!isTesSuccess(ter))
            return ter;
    }

    return tesSUCCESS;
}

}  // namespace ripple
