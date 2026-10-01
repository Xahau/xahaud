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

/** Cheap full-envelope shape gate for manifest-only authority.

    Every outer field except Fee is derived, never copied from the candidate:
    Account from the manifest's master key and NetworkID from this server.
    A relayer-chosen Account or NetworkID would otherwise mint fresh txids
    that each reach manifest signature checks. The byte comparison then pins
    every field, its encoded size, and its representation, and rejects any
    extension (Memos, tags, bounds, Signers, future optional fields) before
    either manifest signature is verified. Fee is mirrored here and pinned to
    its one value by checkFee() and, at ingress, checkManifestIngressFee().
 */
bool
hasCanonicalUnsignedSetManifestShape(
    STTx const& tx,
    std::uint32_t networkID) noexcept
{
    try
    {
        auto const& manifest =
            const_cast<STTx&>(tx).getField(sfManifest).downcast<STObject>();
        auto const masterKey = manifest.getFieldVL(sfPublicKey);
        if (!publicKeyType(makeSlice(masterKey)))
            return false;

        auto const canonical = canonicalUnsignedSetManifest(
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

std::optional<std::uint32_t>
onLedgerManifestSequence(ReadView const& view, PublicKey const& masterKey)
{
    // One keylet namespace serves both lookup directions, so another
    // account's signing-key copy can sit at this master key's keylet. Only
    // the master's own copy is a registration.
    auto const sle = view.read(keylet::manifest(masterKey));
    if (!sle || sle->getAccountID(sfAccount) != calcAccountID(masterKey))
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
    if (manifestAuthorityCandidate &&
        !hasCanonicalUnsignedSetManifestShape(tx, ctx.app.config().NETWORK_ID))
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

    // preflight2 already verified the manifest-authorized lane, except in a
    // dry run (simulate), where it skips signature checks. An ordinary
    // account signature authenticates only the envelope, so that lane still
    // needs the manifest's own signatures checked here.
    if ((!manifestAuthorized || (ctx.flags & tapDRY_RUN)) &&
        !manifest->verify())
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
    else if (auto const occupied = ctx.view.read(canonical))
    {
        // With one namespace for both lookup directions, this master key may
        // already be another account's active signing key.
        if (occupied->getAccountID(sfAccount) == id)
            return tefBAD_LEDGER;
        return tecDUPLICATE;
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

std::optional<XRPAmount>
canonicalUnsignedSetManifestFee(XRPAmount base, STObject const& manifest)
{
    // Canonicality requires every relayer to produce the same Fee. The voted
    // reference fee is ledger state that every node reading the same ledger
    // agrees on, so the price follows fee votes without admitting local load.
    // Only the multipliers are fixed; a fee vote changes the canonical txid
    // once, and publishers rebuild exactly one new wrapper.
    //
    // SetManifest is rare operator traffic. The fixed units price its special
    // admission/state work; the per-byte units price signed payload parsing,
    // signature verification, relay, and durable rewriting. Persistent
    // occupancy is charged separately by the owner's reserve.
    constexpr std::int64_t baseUnits = 100;
    constexpr std::int64_t unitsPerByte = 10;
    constexpr auto maxValue = std::numeric_limits<std::int64_t>::max();

    // A zero base would make the wrapper free. Refuse rather than treat that
    // product as canonical.
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

    // Manifest authority covers no outer bytes. Pricing from this view's
    // voted base fee gives every relayer the same Fee, and therefore the same
    // txid, for a given fee setting. If that Fee is below the current load
    // minimum, ordinary checking below returns telINSUF_FEE_P; manifest gossip
    // carries immediate authority independently, and this wrapper can retry
    // unchanged when load falls to establish ledger durability.
    //
    // A mismatch is local, not final: the same bytes are canonical on a node
    // that has reached the fee setting they were built for, or here again if
    // a vote reverts. The check also runs on a closed ledger, so a proposer
    // cannot charge the master account an arbitrary Fee by skipping it.
    auto const canonical =
        canonicalUnsignedSetManifestFee(ctx.view.fees().base, manifest);
    if (!canonical || ctx.tx[sfFee].xrp() != *canonical)
    {
        JLOG(ctx.j.trace()) << "SetManifest: non-canonical unsigned fee: "
                            << to_string(ctx.tx[sfFee].xrp());
        return telMANIFEST_FEE_MISMATCH;
    }

    // Balance remains an ordinary rule. The exact canonical value is already
    // at or above the ordinary base-fee floor by construction.
    return Transactor::checkFee(ctx, baseFee);
}

std::optional<std::string>
makeSetManifestTx(
    Slice const& manifest,
    std::uint32_t networkID,
    XRPAmount base,
    beast::Journal j)
{
    try
    {
        auto const man = deserializeManifest(manifest, j);
        if (!man || !man->verify())
            return std::nullopt;

        SerialIter manifestIter{manifest};
        STObject const manifestObject{manifestIter, sfManifest};

        auto const fee = canonicalUnsignedSetManifestFee(base, manifestObject);
        if (!fee)
            return std::nullopt;

        return serializeHex(canonicalUnsignedSetManifest(
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
