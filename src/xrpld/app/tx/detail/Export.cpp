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

    if (!ctx.tx.isFieldPresent(sfExportedTxn))
        return temMALFORMED;

    return preflight2(ctx);
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    // Parse the inner exported transaction.
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

    // Shared validation: account must match submitter.
    if (auto ter = ExportLedgerOps::validateExportAccount(
            *stpTrans, ctx.tx.getAccountID(sfAccount), ctx.j);
        !isTesSuccess(ter))
        return ter;

    // Shared validation: NetworkID self-target guard.
    if (auto ter = ExportLedgerOps::validateNetworkID(
            *stpTrans, ctx.app.config().NETWORK_ID, ctx.j);
        !isTesSuccess(ter))
        return ter;

    // Shared validation: TicketSequence required.
    if (auto ter = ExportLedgerOps::validateTicketSequence(*stpTrans, ctx.j);
        !isTesSuccess(ter))
        return ter;

    return tesSUCCESS;
}

TER
Export::doApply()
{
    auto const& exportedObj =
        ctx_.tx.peekAtField(sfExportedTxn).downcast<STObject>();

    Serializer s;
    exportedObj.add(s);
    SerialIter sit(s.slice());

    STTx exportedTx(std::ref(sit));
    uint256 const txnId = exportedTx.getTransactionID();

    auto const account = ctx_.tx.getAccountID(sfAccount);

    TER ter = ExportLedgerOps::createExportedTxn(
        view(), ctx_.app, exportedTx, txnId, j_);
    if (!isTesSuccess(ter))
        return ter;

    return ExportLedgerOps::createShadowTicket(
        view(), account, exportedTx, txnId, j_);
}

}  // namespace ripple
