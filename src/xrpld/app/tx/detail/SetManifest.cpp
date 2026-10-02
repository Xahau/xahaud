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

#include <xrpld/app/misc/Manifest.h>
#include <xrpld/app/tx/detail/SetManifest.h>
#include <xrpld/core/Config.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/XRPAmount.h>
#include <xrpl/protocol/st.h>

#include <limits>

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

    // SetManifest is an ordinary account transaction: the account's signature
    // authorizes it and pays for it. Reject a bad outer signature before
    // spending work on the manifest.
    if (auto const ret = preflight2(ctx); !isTesSuccess(ret))
        return ret;

    // rules:
    // 1. sfManifest must match the manifest template and be validly signed
    // 2. the signingpubkey must match the master r-address
    // 3. manifest must not be already revoked

    STObject const& obj =
        const_cast<ripple::STTx&>(tx).getField(sfManifest).downcast<STObject>();

    // 1. sfManifest must match the manifest template and be validly signed
    auto manifest = deserializeManifest(obj, j);

    if (!manifest.has_value())
    {
        JLOG(j.warn())
            << "SetManifest: invalid manifest passed (parseManifest failed).";
        return temMALFORMED;
    }

    // The account signature authenticates only the envelope, so the
    // manifest's own signatures are checked here.
    if (!manifest->verify())
    {
        JLOG(j.warn())
            << "SetManifest: invalid manifest passed (manifest.verify failed).";
        return temMALFORMED;
    }

    // 2. master must match r-address
    auto wantID = calcAccountID(manifest->masterKey);
    if (ctx.tx.getAccountID(sfAccount) != wantID)
    {
        JLOG(j.warn())
            << "SetManifest: master key must match sfAccount (r-address).";
        return temMALFORMED;
    }

    // 3. not already revoked will be checked in preclaim because it depends on
    // lgr state

    return tesSUCCESS;
}

TER
SetManifest::preclaim(PreclaimContext const& ctx)
{
    if (!ctx.view.rules().enabled(featureOnChainManifests))
        return temDISABLED;

    auto const id = ctx.tx[sfAccount];

    // The account must exist: it pays the fee and anchors sfManifestID.
    auto const sle = ctx.view.read(keylet::account(id));
    if (!sle)
        return terNO_ACCOUNT;

    STObject const& newObj = const_cast<ripple::STTx&>(ctx.tx)
                                 .getField(sfManifest)
                                 .downcast<STObject>();

    auto const newManifest = deserializeManifest(newObj, ctx.j);
    if (!newManifest)
        return tefINTERNAL;  // preflight already parsed this successfully

    auto const canonical = keylet::manifest(newManifest->masterKey);
    bool const registered = sle->isFieldPresent(sfManifestID);

    // The AccountRoot pointer and canonical master-key object are one slot.
    // Refuse a half-present or misdirected slot before considering authority.
    std::shared_ptr<SLE const> sleOld;
    std::shared_ptr<SLE const> occupied;
    if (registered)
    {
        if (sle->getFieldH256(sfManifestID) != canonical.key ||
            !(sleOld = ctx.view.read(canonical)) ||
            sleOld->getAccountID(sfAccount) != id)
            return tefBAD_LEDGER;
    }
    else if (
        (occupied = ctx.view.read(canonical)) &&
        occupied->getAccountID(sfAccount) == id)
        return tefBAD_LEDGER;

    // With one namespace for both lookup directions, this master key may
    // already be another account's active signing key.
    if (occupied)
        return tecDUPLICATE;

    // The account Sequence stops a transaction being replayed. The same
    // manifest can still arrive in a different transaction, so the manifest
    // sequence must also strictly increase.
    if (sleOld)
    {
        if (sleOld->getFieldU32(sfSequence) ==
            std::numeric_limits<std::uint32_t>::max())
        {
            JLOG(ctx.j.warn()) << "SetManifest: New manifest submitted for "
                                  "revoked master. "
                               << id;
            return tefREVOKED_MANIFEST;
        }

        if (newManifest->sequence <= sleOld->getFieldU32(sfSequence))
        {
            JLOG(ctx.j.warn())
                << "SetManifest: Manifest sequence already passed. " << id;
            return tefPAST_MANIFEST_SEQ;
        }
    }

    // Both lookup directions share one namespace and contain the complete
    // manifest. Occupancy therefore enforces the same master/signing role
    // exclusion as ManifestCache without a separate index object.
    if (newManifest->signingKey)
    {
        auto const occupied =
            ctx.view.read(keylet::manifest(*newManifest->signingKey));
        if (occupied && occupied->getAccountID(sfAccount) != id)
        {
            JLOG(ctx.j.warn())
                << "SetManifest: Signing key is already claimed by another "
                   "manifest. "
                << id;
            return tecDUPLICATE;
        }
    }

    return tesSUCCESS;
}

TER
SetManifest::doApply()
{
    auto sle = view().peek(keylet::account(account_));
    if (!sle)
        return tefINTERNAL;

    STObject const& obj = const_cast<ripple::STTx&>(ctx_.tx)
                              .getField(sfManifest)
                              .downcast<STObject>();

    auto const manifest = deserializeManifest(obj, j_);

    // Both of these were established in preflight.
    if (!manifest || calcAccountID(manifest->masterKey) != account_)
        return tefINTERNAL;

    Keylet const canonical = keylet::manifest(manifest->masterKey);
    bool const creating = !sle->isFieldPresent(sfManifestID);
    std::optional<std::uint64_t> ownerNode;

    // A manifest is stored twice so it can be found from either key. Each copy
    // points at the other, and both are erased and rewritten on every update.
    // Only the stable master-key copy is owned and charged a reserve.
    if (sle->isFieldPresent(sfManifestID))
    {
        if (sle->getFieldH256(sfManifestID) != canonical.key)
            return tefBAD_LEDGER;

        auto const sleManifest = view().peek(canonical);
        if (!sleManifest || sleManifest->getAccountID(sfAccount) != account_)
        {
            JLOG(j_.error())
                << "SetManifest: Canonical manifest missing or misowned !! "
                << strHex(canonical.key);
            return tefBAD_LEDGER;
        }

        if (!sleManifest->isFieldPresent(sfOwnerNode))
            return tefBAD_LEDGER;
        ownerNode = sleManifest->getFieldU64(sfOwnerNode);

        if (sleManifest->isFieldPresent(sfSigningPubKey))
        {
            auto const bytes = sleManifest->getFieldVL(sfSigningPubKey);
            if (!publicKeyType(makeSlice(bytes)))
                return tefBAD_LEDGER;

            auto const oldSigning = PublicKey(makeSlice(bytes));
            auto const oldCopy = view().peek(keylet::manifest(oldSigning));
            if (!sleManifest->isFieldPresent(sfManifestID) ||
                sleManifest->getFieldH256(sfManifestID) !=
                    keylet::manifest(oldSigning).key ||
                !oldCopy || oldCopy->getAccountID(sfAccount) != account_ ||
                oldCopy->isFieldPresent(sfOwnerNode) ||
                !oldCopy->isFieldPresent(sfManifestID) ||
                oldCopy->getFieldH256(sfManifestID) != canonical.key)
            {
                JLOG(j_.error()) << "SetManifest: Signing-key copy missing "
                                    "or misdirected !! "
                                 << strHex(canonical.key);
                return tefBAD_LEDGER;
            }
            view().erase(oldCopy);
        }
        else if (sleManifest->isFieldPresent(sfManifestID))
        {
            return tefBAD_LEDGER;
        }

        view().erase(sleManifest);
    }
    else if (view().exists(canonical))
    {
        return tefBAD_LEDGER;
    }

    std::optional<Keylet> signingCopy;
    if (manifest->signingKey)
        signingCopy = keylet::manifest(*manifest->signingKey);

    // Preclaim enforces cross-role uniqueness. Recheck the two actual mutation
    // targets after removing this account's old copies.
    if (view().exists(canonical) ||
        (signingCopy && view().exists(*signingCopy)))
    {
        JLOG(j_.error()) << "SetManifest: Manifest keylet already occupied !! "
                         << strHex(canonical.key);
        return tefBAD_LEDGER;
    }

    if (creating)
    {
        // Registration creates one durable account obligation. The full
        // signing-key copy is derived lookup data and consumes no second
        // reserve. Rotation and revocation retain this same directory entry.
        auto const balance = STAmount((*sle)[sfBalance]).xrp();
        auto const reserve =
            view().fees().accountReserve((*sle)[sfOwnerCount] + 1);
        if (balance < reserve)
            return tecINSUFFICIENT_RESERVE;

        ownerNode = view().dirInsert(
            keylet::ownerDir(account_), canonical, describeOwnerDir(account_));
        if (!ownerNode)
            return tecDIR_FULL;

        adjustOwnerCount(view(), sle, 1, j_);
    }

    if (!ownerNode)
        return tefINTERNAL;

    // Mirror the manifest losslessly, signatures included, so any node can
    // reconstruct and independently verify it (ManifestCache::applyLedger).
    // Field *presence* is copied faithfully: sfVersion is soeDEFAULT in the
    // manifest format, so materialising an absent one would alter the signed
    // payload and break verification.
    auto const write = [&](Keylet const& keylet,
                           std::optional<std::uint64_t> const owner,
                           std::optional<uint256> const other) {
        auto copy = std::make_shared<SLE>(keylet);
        copy->setAccountID(sfAccount, account_);
        if (owner)
            copy->setFieldU64(sfOwnerNode, *owner);
        copy->setFieldU32(sfSequence, obj.getFieldU32(sfSequence));
        copy->setFieldVL(sfPublicKey, obj.getFieldVL(sfPublicKey));
        copy->setFieldVL(sfMasterSignature, obj.getFieldVL(sfMasterSignature));
        if (obj.isFieldPresent(sfVersion))
            copy->setFieldU16(sfVersion, obj.getFieldU16(sfVersion));
        if (obj.isFieldPresent(sfSigningPubKey))
            copy->setFieldVL(sfSigningPubKey, obj.getFieldVL(sfSigningPubKey));
        if (obj.isFieldPresent(sfSignature))
            copy->setFieldVL(sfSignature, obj.getFieldVL(sfSignature));
        if (obj.isFieldPresent(sfDomain))
            copy->setFieldVL(sfDomain, obj.getFieldVL(sfDomain));
        if (other)
            copy->setFieldH256(sfManifestID, *other);
        view().insert(copy);
    };

    write(
        canonical,
        ownerNode,
        signingCopy ? std::optional<uint256>{signingCopy->key} : std::nullopt);
    if (signingCopy)
        write(*signingCopy, std::nullopt, canonical.key);

    sle->setFieldH256(sfManifestID, canonical.key);
    view().update(sle);

    return tesSUCCESS;
}

XRPAmount
SetManifest::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    XRPAmount manifestFee{0};
    if (tx.isFieldPresent(sfManifest))
    {
        STObject const& obj = const_cast<ripple::STTx&>(tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

        // one drop per byte
        manifestFee = XRPAmount{obj.getSerializer().getDataLength()};
    }

    return Transactor::calculateBaseFee(view, tx) + manifestFee;
}

}  // namespace ripple
