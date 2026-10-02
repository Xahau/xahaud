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
#include <xrpl/basics/StringUtilities.h>  // strUnHex
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SystemParameters.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/XRPAmount.h>
#include <xrpl/protocol/serialize.h>
#include <xrpl/protocol/st.h>

#include <limits>

namespace ripple {

bool
hasManifestAuthorityMarkers(STTx const& tx) noexcept
{
    try
    {
        return tx.getTxnType() == ttMANIFEST_SET &&
            tx.isFieldPresent(sfSigningPubKey) &&
            tx.getSigningPubKey().empty() &&
            tx.isFieldPresent(sfTxnSignature) && tx.getSignature().empty();
    }
    catch (std::exception const&)
    {
        return false;
    }
}

/** Build the only permitted outer transaction for `manifest`. */
static STTx
canonicalSetManifest(
    STObject const& manifest,
    AccountID const& account,
    std::optional<std::uint32_t> networkID,
    XRPAmount fee)
{
    return STTx(ttMANIFEST_SET, [&](STObject& obj) {
        obj.setAccountID(sfAccount, account);
        obj.setFieldU32(sfSequence, 0);
        if (networkID)
            obj.setFieldU32(sfNetworkID, *networkID);
        obj.setFieldAmount(sfFee, fee);
        obj.setFieldVL(sfSigningPubKey, Blob{});
        obj.setFieldVL(sfTxnSignature, Blob{});

        obj.peekFieldObject(sfManifest) = manifest;
    });
}

/** The NetworkID a canonical envelope carries for this server's network.

    Networks above 1024 require the field and legacy networks forbid it
    (preflight0), so the canonical envelope follows the same rule.
 */
static std::optional<std::uint32_t>
canonicalNetworkID(std::uint32_t networkID)
{
    if (networkID > 1024)
        return networkID;
    return std::nullopt;
}

bool
hasCanonicalSetManifestShape(STTx const& tx, std::uint32_t networkID) noexcept
{
    try
    {
        auto const& manifest =
            const_cast<STTx&>(tx).getField(sfManifest).downcast<STObject>();
        auto const masterKey = manifest.getFieldVL(sfPublicKey);
        if (!publicKeyType(makeSlice(masterKey)))
            return false;

        auto const canonical = canonicalSetManifest(
            manifest,
            calcAccountID(PublicKey(makeSlice(masterKey))),
            canonicalNetworkID(networkID),
            tx[sfFee].xrp());

        auto const actualBytes = tx.getSerializer();
        auto const canonicalBytes = canonical.getSerializer();
        return actualBytes.slice() == canonicalBytes.slice();
    }
    catch (std::exception const&)
    {
        return false;
    }
}

std::optional<XRPAmount>
canonicalSetManifestFee(XRPAmount base, STObject const& manifest)
{
    // Canonicality requires every relayer to produce the same Fee. The voted
    // reference fee is ledger state that every node reading the same ledger
    // agrees on, so the price follows fee votes without admitting local load.
    // Only the multipliers are fixed; a fee vote changes the canonical txid
    // once.
    constexpr std::int64_t baseUnits = 100;
    constexpr std::int64_t unitsPerByte = 10;
    constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();

    // A zero base would make the transaction free. Refuse rather than treat
    // that product as canonical.
    if (base <= beast::zero)
        return std::nullopt;

    auto const manifestBytes = manifest.getSerializer().getDataLength();
    if (manifestBytes >
        static_cast<std::size_t>((maxValue - baseUnits) / unitsPerByte))
        return std::nullopt;
    auto const units =
        baseUnits + static_cast<std::int64_t>(manifestBytes) * unitsPerByte;
    if (units > maxValue / base.drops())
        return std::nullopt;

    XRPAmount const fee{base.drops() * units};
    if (!isLegalAmount(fee))
        return std::nullopt;
    return fee;
}

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

    // The envelope carries no account signature: authority comes solely from
    // the manifest's own master and ephemeral signatures, which do not cover
    // it. So the whole envelope is one exact transaction per manifest, with
    // Account derived from the manifest's master key and NetworkID from this
    // server, and only the Fee mirrored here (checkFee() pins it). Checked
    // before any signature work, so a relayer cannot vary a field to mint
    // fresh txids that each cost manifest signature checks.
    if (!hasCanonicalSetManifestShape(tx, ctx.app.config().NETWORK_ID))
    {
        JLOG(j.warn()) << "SetManifest: non-canonical envelope.";
        return temMALFORMED;
    }

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

    return preflight2(ctx);
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

    // Replay protection. A byte-identical resubmission is rejected as
    // tefALREADY by checkPriorTxAndLastLedger, but the canonical Fee follows
    // the voted base fee, so the same manifest can arrive under a different
    // txid after a fee change. The strictly-increasing sequence test below
    // covers that, both within this ledger and in every later one. Either
    // result is tef, so a replay is never included and never claims a fee.
    if (sle->isFieldPresent(sfManifestID))
    {
        // A dangling sfManifestID is a corrupt ledger; doApply reports it.
        if (auto const sleOld = ctx.view.read(
                Keylet{ltMANIFEST, sle->getFieldH256(sfManifestID)}))
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
    }

    // On-chain equivalent of the badMasterKey/badEphemeralKey sanity checks in
    // ManifestCache::applyManifest. keylet::manifest(masterKey) is 1:1 with the
    // account, but keylet::manifest(signingKey) is not: without this, a
    // manifest naming another validator's key as its ephemeral key would
    // collide with -- and clobber -- that validator's object.
    if (newManifest->signingKey)
    {
        auto const sleEph =
            ctx.view.read(keylet::manifest(*newManifest->signingKey));
        if (sleEph && sleEph->getAccountID(sfAccount) != id)
        {
            JLOG(ctx.j.warn())
                << "SetManifest: Ephemeral key already claimed by another "
                   "account. "
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

    // A manifest is stored twice so it can be found from either key:
    //   keylet::manifest(masterKey)  -> obj1, sfManifestID -> obj2
    //   keylet::manifest(signingKey) -> obj2, sfManifestID -> obj1
    // A revoked manifest has no signing key, so it exists only as obj1 with no
    // sfManifestID. Both copies are erased and rewritten on every update so
    // they can never drift apart.
    if (sle->isFieldPresent(sfManifestID))
    {
        uint256 const firstID = sle->getFieldH256(sfManifestID);
        auto const sleMan1 = view().peek(Keylet{ltMANIFEST, firstID});
        if (!sleMan1 || sleMan1->getAccountID(sfAccount) != account_)
        {
            JLOG(j_.error()) << "SetManifest: Old manifest object missing or "
                                "misowned (ID1) !! "
                             << strHex(firstID);
            return tefBAD_LEDGER;
        }

        // Absent when the previous manifest was a revocation.
        if (sleMan1->isFieldPresent(sfManifestID))
        {
            uint256 const secondID = sleMan1->getFieldH256(sfManifestID);
            auto const sleMan2 = view().peek(Keylet{ltMANIFEST, secondID});
            if (secondID == firstID || !sleMan2 ||
                sleMan2->getAccountID(sfAccount) != account_)
            {
                JLOG(j_.error())
                    << "SetManifest: Old manifest object missing, misowned or "
                       "self-referential (ID2) !! "
                    << strHex(secondID);
                return tefBAD_LEDGER;
            }
            view().erase(sleMan2);
        }

        view().erase(sleMan1);
    }

    Keylet const klMan1 = keylet::manifest(manifest->masterKey);
    std::optional<Keylet> klMan2;
    if (!manifest->revoked() && manifest->signingKey)
        klMan2 = keylet::manifest(*manifest->signingKey);

    // Neither key may still be occupied: preclaim rejects an ephemeral key held
    // by another account, and the block above cleared this account's own
    // copies.
    if (view().exists(klMan1) || (klMan2 && view().exists(*klMan2)))
    {
        JLOG(j_.error()) << "SetManifest: Manifest keylet already occupied !! "
                         << strHex(klMan1.key);
        return tefBAD_LEDGER;
    }

    // Mirror the manifest losslessly, signatures included, so any node can
    // reconstruct and independently verify it (ManifestCache::applyLedger).
    // Field *presence* is copied faithfully: sfVersion is soeDEFAULT in the
    // manifest format, so materialising an absent one would alter the signed
    // payload and break verification.
    auto const write = [&](Keylet const& kl,
                           std::optional<uint256> const& other) {
        auto sleMan = std::make_shared<SLE>(kl);
        sleMan->setAccountID(sfAccount, account_);
        sleMan->setFieldU32(sfSequence, obj.getFieldU32(sfSequence));
        sleMan->setFieldVL(sfPublicKey, obj.getFieldVL(sfPublicKey));
        sleMan->setFieldVL(
            sfMasterSignature, obj.getFieldVL(sfMasterSignature));
        if (obj.isFieldPresent(sfVersion))
            sleMan->setFieldU16(sfVersion, obj.getFieldU16(sfVersion));
        if (obj.isFieldPresent(sfSigningPubKey))
            sleMan->setFieldVL(
                sfSigningPubKey, obj.getFieldVL(sfSigningPubKey));
        if (obj.isFieldPresent(sfSignature))
            sleMan->setFieldVL(sfSignature, obj.getFieldVL(sfSignature));
        if (obj.isFieldPresent(sfDomain))
            sleMan->setFieldVL(sfDomain, obj.getFieldVL(sfDomain));
        if (other)
            sleMan->setFieldH256(sfManifestID, *other);
        view().insert(sleMan);
    };

    write(klMan1, klMan2 ? std::optional<uint256>{klMan2->key} : std::nullopt);
    if (klMan2)
        write(*klMan2, klMan1.key);

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
        STObject const& obj = const_cast<ripple::STTx&>(tx)
                                  .getField(sfManifest)
                                  .downcast<STObject>();

        // one drop per byte
        manifestFee = XRPAmount{obj.getSerializer().getDataLength()};
    }

    return Transactor::calculateBaseFee(view, tx) + manifestFee;
}

TER
SetManifest::checkFee(PreclaimContext const& ctx, XRPAmount baseFee)
{
    // The envelope carries no account signature, so a relayer-chosen Fee would
    // be both a malleable field and a way to charge the master account. The
    // canonical Fee is priced from this view's voted base fee, which is the
    // parent ledger's: one exact value per fee setting. A mismatch is local,
    // not final: the same bytes are canonical on a server that has reached
    // the fee setting they were built for. The check also runs on a closed
    // ledger, so a proposer cannot charge the master account an arbitrary Fee
    // by skipping it.
    STObject const& manifest =
        const_cast<STTx&>(ctx.tx).getField(sfManifest).downcast<STObject>();
    auto const canonical =
        canonicalSetManifestFee(ctx.view.fees().base, manifest);
    if (!canonical || ctx.tx[sfFee].xrp() != *canonical)
    {
        JLOG(ctx.j.trace()) << "SetManifest: non-canonical fee: "
                            << to_string(ctx.tx[sfFee].xrp());
        return telMANIFEST_FEE_MISMATCH;
    }

    // Floor and balance are the ordinary rules.
    return Transactor::checkFee(ctx, baseFee);
}

std::optional<std::string>
makeSetManifestTx(
    Slice const& manifest,
    std::uint32_t networkID,
    ReadView const& openView,
    beast::Journal j)
{
    try
    {
        auto const man = deserializeManifest(manifest, j);
        if (!man || !man->verify())
            return std::nullopt;

        SerialIter manifestIter{manifest};
        STObject const manifestObject{manifestIter, sfManifest};

        auto const fee =
            canonicalSetManifestFee(openView.fees().base, manifestObject);
        if (!fee)
            return std::nullopt;

        return serializeHex(canonicalSetManifest(
            manifestObject,
            calcAccountID(man->masterKey),
            canonicalNetworkID(networkID),
            *fee));
    }
    catch (std::exception const& e)
    {
        JLOG(j.warn()) << "makeSetManifestTx: " << e.what();
        return std::nullopt;
    }
}

}  // namespace ripple
