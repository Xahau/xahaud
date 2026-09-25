//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL-Labs

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

#include <xrpld/app/tx/detail/HookDefinitionUpdate.h>
#include <xrpl/protocol/TxFlags.h>

namespace ripple {

NotTEC
HookDefinitionUpdate::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureHookFeeV2))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto const& tx = ctx.tx;

    if (tx.getFlags() & tfHookDefinitionUpdateMask)
        return temINVALID_FLAG;

    return preflight2(ctx);
}

TER
HookDefinitionUpdate::preclaim(PreclaimContext const& ctx)
{
    auto const& view = ctx.view;
    auto const& tx = ctx.tx;

    if (!view.exists(keylet::hookDefinition(tx.getFieldH256(sfHookHash))))
        return tecNO_ENTRY;

    return tesSUCCESS;
}

TER
HookDefinitionUpdate::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;

    auto hookDefSle =
        view.peek(keylet::hookDefinition(tx.getFieldH256(sfHookHash)));
    if (!hookDefSle)
        return tefINTERNAL;  // LCOV_EXCL_LINE

    Blob hook = hookDefSle->getFieldVL(sfCreateCode);

    if (!tx.isFlag(tfValidateGuards))
        return tecINTERNAL;  // LCOV_EXCL_LINE

    auto const result =
        hook::doValidateGuards(tx, hook, view.rules(), ctx_.journal);
    if (!result)
        // TODO: TEQU better error code
        // For compatibility, if an already deployed HookDefinition fails with a
        // new guard-checker, don't update/delete  it
        return tecINTERNAL;

    auto const [hookCost, callbackCost] = *result;

    {
        // if result is same as old, don't update sle
        if (hookCost == hookDefSle->getFieldU64(sfHookCost) &&
            callbackCost == hookDefSle->getFieldU64(sfHookCallbackCost))
            return tesSUCCESS;
    }

    {
        // set new cost fields
        hookDefSle->setFieldU64(sfHookCost, hookCost);
        if (callbackCost > 0)
            hookDefSle->setFieldU64(sfHookCallbackCost, callbackCost);
        else if (hookDefSle->isFieldPresent(sfHookCallbackCost))
            hookDefSle->makeFieldAbsent(sfHookCallbackCost);
    }

    {
        // remove old fee fields
        if (hookDefSle->isFieldPresent(sfFee))
            hookDefSle->makeFieldAbsent(sfFee);
        if (hookDefSle->isFieldPresent(sfHookCallbackFee))
            hookDefSle->makeFieldAbsent(sfHookCallbackFee);
    }

    view.update(hookDefSle);

    return tesSUCCESS;
}

XRPAmount
HookDefinitionUpdate::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    auto const baseFee = Transactor::calculateBaseFee(view, tx);

    auto const& hookDefSle =
        view.read(keylet::hookDefinition(tx.getFieldH256(sfHookHash)));

    if (!hookDefSle || !hookDefSle->isFieldPresent(sfCreateCode))
        // if hook definition is not present, doValidateGuards will not call, so
        // no additional fee is needed
        return baseFee;

    auto const hookCodeSize = hookDefSle->getFieldVL(sfCreateCode).size();

    return baseFee + XRPAmount{hook::computeCreationFee(hookCodeSize)};
}

}  // namespace ripple
