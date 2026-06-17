//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <xrpld/app/tx/detail/SetBoot.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/st.h>

namespace ripple {

TxConsequences
SetBoot::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
SetBoot::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featurePWABoot))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;
    auto& j = ctx.j;

    if (tx.getFlags() & tfUniversalMask)
    {
        JLOG(j.warn()) << "SetBoot: invalid flags.";
        return temINVALID_FLAG;
    }

    // sfBootBlob present => set/replace (validate size); absent => delete.
    if (tx.isFieldPresent(sfBootBlob))
    {
        Blob const& blob = tx.getFieldVL(sfBootBlob);
        if (blob.size() == 0 || blob.size() > SetBoot::maxBootBlobBytes)
        {
            JLOG(j.warn()) << "SetBoot: boot blob must be 1.."
                           << SetBoot::maxBootBlobBytes << " bytes.";
            return temMALFORMED;
        }
    }

    return preflight2(ctx);
}

TER
SetBoot::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featurePWABoot))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];
    if (!ctx.view.read(keylet::account(id)))
        return terNO_ACCOUNT;

    return tesSUCCESS;
}

TER
SetBoot::doApply()
{
    Sandbox sb(&ctx_.view());

    auto sle = sb.peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    if (ctx_.tx.isFieldPresent(sfBootBlob))
        sle->setFieldVL(sfBootBlob, ctx_.tx.getFieldVL(sfBootBlob));
    else if (sle->isFieldPresent(sfBootBlob))
        sle->makeFieldAbsent(sfBootBlob);

    sb.update(sle);
    sb.apply(ctx_.rawView());
    return tesSUCCESS;
}

XRPAmount
SetBoot::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    // one drop per blob byte (mirrors SetRemarks) — prices bloat, keeps the stub small.
    XRPAmount blobFee{0};
    if (tx.isFieldPresent(sfBootBlob))
        blobFee =
            XRPAmount{static_cast<std::int64_t>(tx.getFieldVL(sfBootBlob).size())};
    return Transactor::calculateBaseFee(view, tx) + blobFee;
}

}  // namespace ripple
