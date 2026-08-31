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
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/XRPAmount.h>
#include <xrpl/protocol/serialize.h>
#include <xrpl/protocol/st.h>

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

bool
isUnsignedSetManifest(STTx const& tx) noexcept
{
    try
    {
        return hasManifestAuthorityMarkers(tx) && !tx.isFieldPresent(sfSigners);
    }
    catch (std::exception const&)
    {
        return false;
    }
}

/** Build the only permitted manifest-authorized outer transaction.

    `fee` is supplied by the caller so preflight can cheaply certify every
    other byte before a ledger fee schedule is available. checkFee() pins that
    final value later.
 */
static STTx
canonicalUnsignedSetManifest(
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

/** Cheap full-envelope shape gate for manifest-only authority.

    The field-count comparison rejects ordinary extension attacks (Memos,
    tags, bounds, Signers, and future optional common fields) before copying
    their contents or verifying either manifest signature. The byte comparison
    then pins every admitted field, its encoded size, and its representation.
    Fee is mirrored here and pinned to its one value by checkFee().
 */
bool
hasCanonicalUnsignedSetManifestShape(STTx const& tx) noexcept
{
    try
    {
        auto const& manifest =
            const_cast<STTx&>(tx).getField(sfManifest).downcast<STObject>();
        auto const canonical = canonicalUnsignedSetManifest(
            manifest,
            tx.getAccountID(sfAccount),
            tx[~sfNetworkID],
            tx[sfFee].xrp());

        if (tx.getCount() != canonical.getCount())
            return false;

        auto const actualBytes = tx.getSerializer();
        auto const canonicalBytes = canonical.getSerializer();
        return actualBytes.slice() == canonicalBytes.slice();
    }
    catch (std::exception const&)
    {
        return false;
    }
}

std::optional<std::uint32_t>
onLedgerManifestSequence(ReadView const& view, PublicKey const& masterKey)
{
    auto const sle = view.read(keylet::manifest(masterKey));
    if (!sle)
        return std::nullopt;
    return sle->getFieldU32(sfSequence);
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

    // Empty single-signing fields nominate manifest-only authority. Check the
    // complete shape even if a relayer also attached Signers: the canonical
    // envelope has none, so that extension is rejected before any crypto.
    bool const manifestAuthorityCandidate = hasManifestAuthorityMarkers(tx);
    if (manifestAuthorityCandidate && !hasCanonicalUnsignedSetManifestShape(tx))
    {
        JLOG(j.warn()) << "SetManifest: non-canonical unsigned envelope.";
        return temMALFORMED;
    }
    bool const manifestAuthorized = isUnsignedSetManifest(tx);

    // Authenticate the cheapest available outer authority before doing any
    // further work. For the manifest-authorized lane this verifies the two
    // manifest signatures exactly once. For the account-authorized lane it
    // rejects a bad outer signature before spending work on the manifest.
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

    // preflight2 already verified the manifest-authorized lane. An ordinary
    // account signature authenticates only the envelope, so that lane still
    // needs the manifest's own signatures checked here.
    if (!manifestAuthorized && !manifest->verify())
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
    if (registered)
    {
        if (sle->getFieldH256(sfManifestID) != canonical.key ||
            !(sleOld = ctx.view.read(canonical)) ||
            sleOld->getAccountID(sfAccount) != id)
            return tefBAD_LEDGER;
    }
    else if (ctx.view.exists(canonical))
    {
        return tefBAD_LEDGER;
    }

    // Manifest-only authority may rotate or revoke an existing registration,
    // but it cannot create one. Validator operators already provision public
    // identity metadata; an account is comparable one-time setup and supplies
    // explicit consent plus the fee anchor.
    if (isUnsignedSetManifest(ctx.tx) && !registered)
    {
        JLOG(ctx.j.trace())
            << "SetManifest: unsigned envelope cannot create manifest slot. "
            << id;
        return tefBAD_AUTH;
    }

    // Replay protection. A byte-identical resubmission is rejected as
    // tefALREADY by checkPriorTxAndLastLedger, but the same manifest can
    // still arrive under a different txid: the canonical unsigned sfFee is
    // one exact value per ledger (checkFee) yet tracks the fee schedule
    // across ledgers, and the account-signed lane chooses its own envelope
    // outright. The strictly-increasing sequence test below covers all of
    // those, both within this ledger and in every later one. Either result
    // is tef, so a replay is never included and never claims a fee.
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

    // On-chain equivalent of the badMasterKey/badEphemeralKey sanity checks in
    // ManifestCache::applyManifest. Separate ledger namespaces no longer make
    // master/signing-key reuse collide accidentally, so preserve that global
    // key-role exclusion explicitly.
    if (ctx.view.exists(keylet::manifestSigningKey(newManifest->masterKey)))
    {
        JLOG(ctx.j.warn())
            << "SetManifest: Master key is already another manifest's "
               "signing key. "
            << id;
        return tecDUPLICATE;
    }

    if (newManifest->signingKey)
    {
        if (ctx.view.exists(keylet::manifest(*newManifest->signingKey)))
        {
            JLOG(ctx.j.warn())
                << "SetManifest: Signing key is already another manifest's "
                   "master key. "
                << id;
            return tecDUPLICATE;
        }

        auto const sleIndex =
            ctx.view.read(keylet::manifestSigningKey(*newManifest->signingKey));
        if (sleIndex && sleIndex->getFieldH256(sfManifestID) != canonical.key)
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

    // preclaim is the public stateful refusal. Keep the mutation boundary
    // independently fail-closed so no future alternate apply path can turn an
    // unsigned manifest into a first registration.
    if (isUnsignedSetManifest(ctx_.tx) && !sle->isFieldPresent(sfManifestID))
        return view().exists(keylet::manifest(manifest->masterKey))
            ? tefBAD_LEDGER
            : tefINTERNAL;

    Keylet const canonical = keylet::manifest(manifest->masterKey);
    bool const creating = !sle->isFieldPresent(sfManifestID);
    std::optional<std::uint64_t> ownerNode;

    // One complete manifest lives at the stable master-key keylet because
    // locally trusted masters are the common reconciliation path on every
    // validated ledger. The active signing key has only a thin pointer: that
    // inverse lookup is needed on the comparatively rare validation-cache
    // miss. Validate and erase the old pair before publishing the replacement.
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

        ownerNode = sleManifest->getFieldU64(sfOwnerNode);

        if (sleManifest->isFieldPresent(sfSigningPubKey))
        {
            auto const bytes = sleManifest->getFieldVL(sfSigningPubKey);
            if (!publicKeyType(makeSlice(bytes)))
                return tefBAD_LEDGER;

            auto const oldSigning = PublicKey(makeSlice(bytes));
            auto const oldIndex =
                view().peek(keylet::manifestSigningKey(oldSigning));
            if (!oldIndex ||
                oldIndex->getFieldH256(sfManifestID) != canonical.key)
            {
                JLOG(j_.error()) << "SetManifest: Signing-key index missing "
                                    "or misdirected !! "
                                 << strHex(canonical.key);
                return tefBAD_LEDGER;
            }
            view().erase(oldIndex);
        }

        view().erase(sleManifest);
    }
    else if (view().exists(canonical))
    {
        return tefBAD_LEDGER;
    }

    std::optional<Keylet> signingIndex;
    if (manifest->signingKey)
        signingIndex = keylet::manifestSigningKey(*manifest->signingKey);

    // Preclaim enforces cross-role uniqueness. Recheck the actual mutation
    // targets after removing this account's old index.
    if (view().exists(canonical) ||
        (signingIndex && view().exists(*signingIndex)) ||
        view().exists(keylet::manifestSigningKey(manifest->masterKey)) ||
        (manifest->signingKey &&
         view().exists(keylet::manifest(*manifest->signingKey))))
    {
        JLOG(j_.error()) << "SetManifest: Manifest keylet already occupied !! "
                         << strHex(canonical.key);
        return tefBAD_LEDGER;
    }

    if (creating)
    {
        // Registration creates one durable account obligation. The thin
        // signing-key index is derived lookup data and consumes no second
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
    auto sleManifest = std::make_shared<SLE>(canonical);
    sleManifest->setAccountID(sfAccount, account_);
    sleManifest->setFieldU64(sfOwnerNode, *ownerNode);
    sleManifest->setFieldU32(sfSequence, obj.getFieldU32(sfSequence));
    sleManifest->setFieldVL(sfPublicKey, obj.getFieldVL(sfPublicKey));
    sleManifest->setFieldVL(
        sfMasterSignature, obj.getFieldVL(sfMasterSignature));
    if (obj.isFieldPresent(sfVersion))
        sleManifest->setFieldU16(sfVersion, obj.getFieldU16(sfVersion));
    if (obj.isFieldPresent(sfSigningPubKey))
        sleManifest->setFieldVL(
            sfSigningPubKey, obj.getFieldVL(sfSigningPubKey));
    if (obj.isFieldPresent(sfSignature))
        sleManifest->setFieldVL(sfSignature, obj.getFieldVL(sfSignature));
    if (obj.isFieldPresent(sfDomain))
        sleManifest->setFieldVL(sfDomain, obj.getFieldVL(sfDomain));
    view().insert(sleManifest);

    if (signingIndex)
    {
        auto sleIndex = std::make_shared<SLE>(*signingIndex);
        sleIndex->setFieldH256(sfManifestID, canonical.key);
        view().insert(sleIndex);
    }

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

XRPAmount
canonicalUnsignedSetManifestFee(Rules const& rules, STObject const& manifest)
{
    // Canonicality requires every relayer to produce the same Fee, so voted
    // base fees and local load cannot enter this calculation. The active
    // ruleset does: a future pricing amendment may replace these constants,
    // at which point old wrappers become non-canonical and publishers rebuild
    // exactly one new txid.
    (void)rules;

    // SetManifest is rare operator traffic. The base prices its special
    // admission/state work; the byte component prices signed payload parsing,
    // signature verification, relay, and durable rewriting. Persistent
    // occupancy is charged separately by the owner's reserve.
    constexpr XRPAmount::value_type baseDrops = 1'000;
    constexpr XRPAmount::value_type dropsPerByte = 100;
    auto const manifestBytes = manifest.getSerializer().getDataLength();

    // Refuse overflow rather than accidentally making an enormous manifest
    // cheap. Normal manifest limits make this unreachable for honest input.
    if (manifestBytes >
        static_cast<std::size_t>(
            (std::numeric_limits<XRPAmount::value_type>::max() - baseDrops) /
            dropsPerByte))
        Throw<std::overflow_error>("SetManifest canonical fee overflow");
    return XRPAmount{
        baseDrops +
        static_cast<XRPAmount::value_type>(manifestBytes) * dropsPerByte};
}

TER
SetManifest::checkFee(PreclaimContext const& ctx, XRPAmount baseFee)
{
    // Account-signed SetManifest transactions use ordinary fee semantics.
    // Their outer signature authenticates the chosen Fee, and they may enter
    // TxQ like any other account transaction.
    if (!isUnsignedSetManifest(ctx.tx))
        return Transactor::checkFee(ctx, baseFee);

    STObject const& manifest =
        const_cast<STTx&>(ctx.tx).getField(sfManifest).downcast<STObject>();

    // Manifest authority covers no outer bytes. A ruleset-fixed base plus
    // exact payload bytes gives every relayer the same Fee and therefore the
    // same txid, independent of fee votes or local load. If that Fee is below
    // the current minimum, ordinary checking below returns telINSUF_FEE_P and
    // anti-entropy retries this same transaction later.
    if (ctx.tx[sfFee].xrp() !=
        canonicalUnsignedSetManifestFee(ctx.view.rules(), manifest))
    {
        JLOG(ctx.j.trace()) << "SetManifest: non-canonical unsigned fee: "
                            << to_string(ctx.tx[sfFee].xrp());
        return temBAD_FEE;
    }

    // Balance remains an ordinary rule. The exact canonical value is already
    // at or above the ordinary base-fee floor by construction.
    return Transactor::checkFee(ctx, baseFee);
}

std::optional<std::string>
makeSetManifestTx(
    Slice const& manifest,
    std::uint32_t networkID,
    Rules const& rules,
    beast::Journal j)
{
    try
    {
        auto const man = deserializeManifest(manifest, j);
        if (!man || !man->verify())
            return std::nullopt;

        SerialIter manifestIter{manifest};
        STObject const manifestObject{manifestIter, sfManifest};

        return serializeHex(canonicalUnsignedSetManifest(
            manifestObject,
            calcAccountID(man->masterKey),
            networkID > 1024 ? std::optional<std::uint32_t>{networkID}
                             : std::nullopt,
            canonicalUnsignedSetManifestFee(rules, manifestObject)));
    }
    catch (std::exception const& e)
    {
        JLOG(j.warn()) << "makeSetManifestTx: " << e.what();
        return std::nullopt;
    }
}

}  // namespace ripple
