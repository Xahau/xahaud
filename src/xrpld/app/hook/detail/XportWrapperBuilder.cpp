#include <xrpld/app/hook/detail/XportWrapperBuilder.h>
#include <xrpld/app/tx/detail/ExportLedgerOps.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/TxFormats.h>

#include <memory>

namespace hook {
namespace XportWrapperBuilder {

using namespace ripple;

Expected<Result, HookReturnCode>
build(Input const& input)
{
    if (input.committeeHash.isZero())
        return Unexpected(::hook_api::hook_return_code::INVALID_ARGUMENT);

    std::shared_ptr<STTx const> innerTx;
    try
    {
        SerialIter sit(input.innerTxBlob);
        innerTx = std::make_shared<STTx const>(sit);
    }
    catch (std::exception const& e)
    {
        JLOG(input.j.trace()) << "HookExport: Failed " << e.what();
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);
    }

    if (auto ter =
            ExportLedgerOps::validateExportSigningFields(*innerTx, input.j);
        !isTesSuccess(ter))
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);

    if (auto ter = ExportLedgerOps::validateExportAccount(
            *innerTx, input.exporter, input.j);
        !isTesSuccess(ter))
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);

    if (auto ter = ExportLedgerOps::validateNetworkID(
            *innerTx, input.networkID, input.j);
        !isTesSuccess(ter))
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);

    if (auto ter = ExportLedgerOps::validateOriginMemoProjection(
            *innerTx, input.networkID, input.j);
        !isTesSuccess(ter))
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);

    if (auto ter = ExportLedgerOps::validateTicketSequence(*innerTx, input.j);
        !isTesSuccess(ter))
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);

    if (!input.generateNonce)
    {
        JLOG(input.j.trace())
            << "HookExport: Nonce callback missing for ttEXPORT wrapper";
        return Unexpected(::hook_api::hook_return_code::INTERNAL_ERROR);
    }

    auto nonce = input.generateNonce();
    if (!nonce)
        return Unexpected(::hook_api::hook_return_code::INTERNAL_ERROR);

    Serializer innerSer;
    innerTx->add(innerSer);

    STObject exportObj(sfGeneric);
    exportObj.setFieldU16(sfTransactionType, ttEXPORT);
    exportObj[sfAccount] = input.exporter;
    exportObj[sfSequence] = 0u;
    exportObj.setFieldVL(sfSigningPubKey, Blob{});
    exportObj[sfFirstLedgerSequence] = input.ledgerSeq + 1;
    exportObj[sfLastLedgerSequence] =
        input.ledgerSeq + ExportLimits::maxAdmissionWindowLedgers;
    exportObj[sfFee] = STAmount{0};
    exportObj.setFieldH256(sfExportCommitteeHash, input.committeeHash);

    SerialIter sit(innerSer.slice());
    exportObj.set(std::make_unique<STObject>(sit, sfExportedTxn));

    STObject emitDetails(sfEmitDetails);
    emitDetails.setFieldU32(sfEmitGeneration, input.emitGeneration);
    emitDetails.setFieldU64(sfEmitBurden, input.emitBurden);
    emitDetails.setFieldH256(sfEmitParentTxnID, input.parentTxnID);
    emitDetails.setFieldH256(sfEmitNonce, *nonce);
    emitDetails.setFieldH256(sfEmitHookHash, input.hookHash);
    if (input.hasCallback)
        emitDetails.setAccountID(sfEmitCallback, input.exporter);
    exportObj.set(std::move(emitDetails));

    if (!input.calculateFee)
    {
        JLOG(input.j.trace()) << "HookExport: Fee calculation callback missing "
                                 "for ttEXPORT wrapper";
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);
    }

    Serializer feeSer;
    exportObj.add(feeSer);
    auto feeResult = input.calculateFee(feeSer.slice());
    if (!feeResult)
    {
        JLOG(input.j.trace())
            << "HookExport: Fee calculation failed for ttEXPORT wrapper";
        return Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE);
    }
    exportObj[sfFee] = STAmount{static_cast<std::uint64_t>(*feeResult)};

    Serializer exportSer;
    exportObj.add(exportSer);
    STTx wrapperTx(SerialIter{exportSer.slice()});

    return Result{std::move(wrapperTx)};
}

}  // namespace XportWrapperBuilder
}  // namespace hook
