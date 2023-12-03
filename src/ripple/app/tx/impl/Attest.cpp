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

#include <ripple/app/tx/impl/Attest.h>
#include <ripple/basics/Log.h>
#include <ripple/ledger/View.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>

namespace ripple {

TxConsequences
Attest::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
Attest::preflight(PreflightContext const& ctx)
{
    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto const flags = ctx.tx.getFlags();

    if (ctx.rules.enabled(fix1543) && (flags & tfAttestMask))
        return temINVALID_FLAG;


    return preflight2(ctx);
}

TER
Attest::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureAttestations))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    Keylet kl =
        keylet::attestation(id, ctx.tx.getFieldH256(sfAttestedTxnID));

    uint32_t flags = ctx.tx.getFlags();

    bool const isDelete = (flags | tfDeleteAttestation) == tfDeleteAttestation;
    bool const exists = ctx.view.exists(kl);
    
    if (exists && !isDelete)
        return tecDUPLICATE;
    else if (!exists && isDelete)
        return tecNO_ENTRY;


    return tesSUCCESS;
}

TER
Attest::doApply()
{
    auto j = ctx_.app.journal("View");

    auto const sle = view().peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    // check for sufficient reserves
    {
        STAmount const reserve{
            view().fees().accountReserve(sle->getFieldU32(sfOwnerCount) + 1)};

        STAmount const afterFee =
            mPriorBalance - ctx_.tx.getFieldAmount(sfFee).xrp();

        if (afterFee > mPriorBalance || afterFee < reserve)
            return tecINSUFFICIENT_RESERVE;
    }


    Keylet kl =
        keylet::attestation(account_, ctx_.tx.getFieldH256(sfAttestedTxnID));

    uint32_t flags = ctx_.tx.getFlags();
    
    bool const isDelete = (flags | tfDeleteAttestation) == tfDeleteAttestation;
    bool const exists = view().exists(kl);

    // delete mode
    if (exists && isDelete)
    {
        auto sleA = view().peek(kl);

        AccountID owner = sleA->getAccountID(sfOwner);

        auto const page = (*sleA)[sfOwnerNode];
        if (!view().dirRemove(
                keylet::ownerDir(owner), page, kl.key, true))
        {
            JLOG(j.fatal())
                << "Could not remove Attestation from owner directory";
            return tefBAD_LEDGER;
        }

        view().erase(sleA);
        adjustOwnerCount(view(), sle, -1, j);
        return tesSUCCESS;
    }
    // create mode
    else if (!exists && !isDelete)
    {
        auto sleA = std::make_shared<SLE>(kl);

        sleA->setAccountID(sfOwner, account_);

        sleA->setFieldH256(sfAttestedTxnID, ctx_.tx.getFieldH256(sfAttestedTxnID));

        auto const page = view().dirInsert(
            keylet::ownerDir(account_), kl, describeOwnerDir(account_));

        JLOG(j_.trace())
            << "Adding Attestation to owner directory " << to_string(kl.key)
            << ": " << (page ? "success" : "failure");

        if (!page)
            return tecDIR_FULL;

        sleA->setFieldU64(sfOwnerNode, *page);
        view().insert(sleA);

        adjustOwnerCount(view(), sle, 1, j);
        return tesSUCCESS;
    }
    // everything else is invalid
    else
        return tecINTERNAL;
}

XRPAmount
Attest::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    return Transactor::calculateBaseFee(view, tx);
}

}  // namespace ripple
