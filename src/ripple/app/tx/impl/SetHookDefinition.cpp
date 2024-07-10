//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#include <ripple/app/tx/impl/SetHookDefinition.h>
#include <ripple/basics/Log.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/TxFlags.h>

namespace ripple {

XRPAmount
SetHookDefinition::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

NotTEC
SetHookDefinition::preflight(PreflightContext const& ctx)
{
    // if (!ctx.rules.enabled(featureHooksUpdate2))
    //     return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    return preflight2(ctx);
}

TER
SetHookDefinition::preclaim(PreclaimContext const& ctx)
{
    // if (!ctx.view.rules().enabled(featureHooksUpdate2))
    //     return temDISABLED;

    return tesSUCCESS;
}

TER
SetHookDefinition::doApply()
{
    auto j = ctx_.app.journal("View");
    Sandbox sb(&ctx_.view());
    auto const sle = sb.read(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    ripple::Blob wasmBytes = ctx_.tx.getFieldVL(sfCreateCode);

    if (wasmBytes.size() > hook::maxHookWasmSize())
    {
        JLOG(j.warn()) << "Malformed transaction: SetHookDefinition operation "
                          "would create blob larger than max";
        return tecINTERNAL;
    }

    auto const createHookHash =
        ripple::sha512Half_s(ripple::Slice(wasmBytes.data(), wasmBytes.size()));

    Keylet keylet = ripple::keylet::hookDefinition(
        account_, ctx_.tx.getFieldU32(sfSequence));
    auto newHookDef = std::make_shared<SLE>(keylet);
    newHookDef->setFieldH256(sfHookHash, createHookHash);
    newHookDef->setAccountID(sfOwner, account_);
    newHookDef->setFieldH256(sfHookOn, ctx_.tx.getFieldH256(sfHookOn));
    newHookDef->setFieldH256(sfHookNamespace, ctx_.tx.getFieldH256(sfHookOn));
    newHookDef->setFieldArray(
        sfHookParameters,
        ctx_.tx.isFieldPresent(sfHookParameters)
            ? ctx_.tx.getFieldArray(sfHookParameters)
            : STArray{});
    newHookDef->setFieldU16(
        sfHookApiVersion, ctx_.tx.getFieldU16(sfHookApiVersion));
    newHookDef->setFieldVL(sfCreateCode, wasmBytes);
    newHookDef->setFieldH256(sfHookSetTxnID, ctx_.tx.getTransactionID());
    newHookDef->setFieldU64(sfReferenceCount, 0);
    newHookDef->setFieldU32(sfFlags, hsfMUTALBE);
    if (ctx_.tx.isFieldPresent(sfHookGrants))
    {
        auto const& grants = ctx_.tx.getFieldArray(sfHookGrants);
        if (!grants.empty())
            newHookDef->setFieldArray(sfHookGrants, grants);
    }

    sb.update(newHookDef);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

}  // namespace ripple
