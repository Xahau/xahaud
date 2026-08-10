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

    if (!sle->isFieldPresent(sfManifestID))
    {
        // pass, no special conditions if they've never set a manifest
        return tesSUCCESS;
    }

    STObject const& newObj = const_cast<ripple::STTx&>(ctx.tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

    auto newManifest = Manifest::deserializeManifest(newObj, j);


    auto const sleOld = view.read(Keylet{ltMANIFEST, sle->getFieldH256(sfManifestID));

    if (!sleOld)
    {
        // this is actually a nasty error but it's handled with a tefBAD_LEDGER in apply
        return tesSUCCESS;
    }
    
    if (sleOld->getFieldU32(sfSequence) == std::numeric_limits<std::uint32_t>::max())
    {
        JLOG(ctx.j.warn())  
                << "SetManifest: New manifest submitted for revoked master. " << id;
        return tefREVOKED_MANIFEST;
    }

    if (newManifest->sequence <= sleOld->getFieldU32(sfSequence))
    {
        JLOG(ctx.j.warn())
                << "SetManifest: Manifest sequence already passed. " << id;
        return tefPAST_MANIFEST_SEQ;
    }
    
    return tesSUCCESS;
}

TER
SetManifest::doApply()
{
    auto sle = view().peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    if (sle->isFieldPresent(sfManifestID))
    {
        // there's an existing manifest on the account
        // all manifests have two identical objects for ease of lookup
        // keylet(ephemeral key) -> obj1
        // keylet(master key) -> obj2
        // we need to remove and re-create both objects each time the manifest
        // is updated to keep them in sync

        uint256 const firstID = sle->getFieldH256(sfManifestID);
        auto sleMan1 = view().peek(Keylet{ltMANIFEST, firstID});
        if (!sleMan1)
        {
            JLOG(ctx.j.error())
                    << "SetManifest: Old manifest object referenced but missing (ID1) !! " << strHex(firstID);
            return tefBAD_LEDGER;
        }

        uint256 const secondID = sle->getFieldH256(sfManifestID);
        if (secondID == firstID)
        {
            JLOG(ctx.j.error())
                << "SetManifest: Manifest second ID references first object!! " << strHex(firstID);
            return tefBAD_LEDGER;
        }

        auto sleMan2 = view().peek(Keylet{ltMANIFEST, secondID});
        if (!sleMan2)
        {
            JLOG(ctx.j.error())
                    << "SetManifest: Old manifest object referenced but missing (ID2) !! " << strHex(secondID);
            return tefBAD_LEDGER;
        }

        if (sleMan1->getAccountID(sfAccount) != account_ ||
            sleMan2->getAccountID(sfAccount) != account_)
        {
            JLOG(ctx.j.error())
                    << "SetManifest: One or more manifest IDs point at incorrect account!!";
            return tefBAD_LEDGER;
        }
    
        // remove both manifests so they can be recreated by the code path below

        view().erase(sleMan1);
        view().erase(sleMan2);
    }


    STObject const& obj = const_cast<ripple::STTx&>(ctx_.tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

    auto manifest = Manifest::deserializeManifest(obj, j);
    
    if (!manifest.has_value())
    {
        JLOG(j.warn()) << "SetManifest: invalid manifest passed (parseManifest failed).";
        return temMALFORMED;
    }

    if (calcAccountID(manifest->masterKey) != account_)
        return tefINTERNAL;

    Keylet klMan1 = Keylet::manifest(manifest->masterKey);
    std::optional<Keylet> klMan2;
    if (!manifest->revoked() && manifest->signingKey.has_value())
        klMan2 = Keylet::manifest(manifest->signingKey);

    auto setManifest = [&](std::shared_ptr<SLE>& sle, std::optional<uint256> otherKey) -> void
    {
        sle->setAccountID(sfAccount, account_);
        sle->setFieldVL(sfPublicKey, manifest->masterKey);
        if (manifest->signingKey.has_value())
            sle->setFieldVL(sfSigningPubKey, *(manifest->signingKey));
        sle->setFieldU32(sfSequence, manifest->sequence);
        sle->setFieldU16(sfVersion, 0);
        if (manifest->domain.has_value() && manifest->domain != "")
            sle->setFieldVL(sfDomain, manifest->domain);
        if (otherKey.has_value())
            sle>setFieldH256(sfManifestID, *otherKey);
    };

    std::shared_ptr<SLE> sleMan1 = std::make_shared<SLE>(klMan1);
    setManifest(sleMan1, klMan2);
    
    if (klMan2.has_value())
    {
        sleMan2 = std::make_shared<SLE>(*klMan2);
        setManifest(sleMan2, klMan1);
        view().insert(sleMan2);
    }
    
    view().insert(sleMan1);

    sle->setFieldH256(sfManifestID, klMan1.key);

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
