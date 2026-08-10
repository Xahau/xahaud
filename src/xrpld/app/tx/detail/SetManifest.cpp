//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <xrpld/app/tx/detail/SetManifest.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/st.h>
#include <xrpl/app/misc/Manifest.h>

namespace ripple {

TxConsequences
SetManifest::makeTxConsequences(PreflightContext const& ctx)
{
    return TxConsequences{ctx.tx, TxConsequences::normal};
}

NotTEC
SetManifest::preflight(PreflightContext const& ctx)
{
    if (!ctx.rules.enabled(featureOnChainManifests))
        return temDISABLED;

    if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
        return ret;

    auto& tx = ctx.tx;
    auto& j = ctx.j;

    if (tx.getFlags() & tfUniversalMask)
    {
        JLOG(j.warn()) << "SetManifest: Invalid flags set.";
        return temINVALID_FLAG;
    }

    // rules:
    // 1. sfManifest must match the manifest template and be validly signed
    // 2. the signingpubkey must match the master r-address
    // 3. manifest must not be already revoked

    STObject const& obj = const_cast<ripple::STTx&>(tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

    // 1. sfManifest must match the manifest template and be validly signed
    auto manifest = Manifest::deserializeManifest(obj, j);
    
    if (!manifest.has_value())
    {
        JLOG(j.warn()) << "SetManifest: invalid manifest passed (parseManifest failed).";
        return temMALFORMED;
    }

    if (!manifest->verify())
    {
        JLOG(j.warn()) << "SetManifest: invalid manifest passed (manifest.verify failed).";
        return temMALFORMED;
    }

    // 2. master must match r-address
    auto wantID = calcAccountID(manifest->masterKey);
    if (ctx.tx.getAccountID(sfAccount) != wantID)
    {
        JLOG(j.warn()) << "SetManifest: master key must match sfAccount (r-address).";
        return temMALFORMED;
    }

    // 3. not already revoked will be checked in preclaim because it depends on lgr state


    // TODO: seq=0, signingpubkey =0
    return preflight2(ctx);
}

TER
SetManifest::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureOnChainManifests))
        return temDISABLED;

    // RH UPTO: check if revoked already, check if existing manifest seq >= new one, check fee?, 
    
    auto const id = ctx.tx[sfAccount];

    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    if (!sle->isFieldPresent(sfManifest))
    {
        // pass, no special conditions if they've never set a manifest
        return tesSUCCESS;
    }

    STObject const& newObj = const_cast<ripple::STTx&>(ctx.tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

    auto newManifest = Manifest::deserializeManifest(newObj, j);


    STObject const& oldObj = const_cast<ripple::STObject&>(*sle)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

    auto oldManifest = Manifest::deserializeManifest(oldObj, j);

    if (!oldManifest)
    {
        // this is an error but to prevent bricking the manifest system let them set a new one
        JLOG(ctx.j.warn())
                << "SetManifest: WARNING old sfManifest was not parsable!! " << id;
        return tesSUCCCESS;
    }

    if (newManifest->hash() == oldManifest->hash() || newManifest->sequence == oldManifest->sequence)
    {
        JLOG(ctx.j.warn())
                << "SetManifest: Old sfManifest was same as new one. " << id;
        return temREDUNDANT;
    }


    if (oldManifest->revoked())
    {
        JLOG(ctx.j.warn())  
                << "SetManifest: New manifest submitted for revoked master. " << id;
        return tefREVOKED_MANIFEST;
    }

    if (oldManifest->sequence < newManifest->sequence)
    {
        JLOG(ctx.j.warn())
                << "SetManifest: Replay or past seq manifest. " << id;
        return tefPAST_MANIFEST_SEQ;
    }
    
    return tesSUCCESS;
}

TER
SetManifest::doApply()
{
    auto const sle = view().read(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    sle.set(ctx_.tx[sfManifest]);

    view().update(sle);

    return tesSUCCESS;
}

XRPAmount
SetManifest::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount manifestFee{0};
    if (tx.isFieldPresent(sfManifest))
    {
        STObject const& newObj = const_cast<ripple::STTx&>(tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

        // one drop per byte
        manifestFee = XRPAmount { newObj.getSerializer().getDataLength() };
    }
    auto fee = Transactor::calculateBaseFee(view, tx);
    return fee + manifestFee;
}

}  // namespace ripple
