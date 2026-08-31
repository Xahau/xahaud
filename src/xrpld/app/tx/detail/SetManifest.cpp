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
#include <xrpl/protocol/Quality.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/XRPAmount.h>  // mulRatio
#include <xrpl/protocol/serialize.h>
#include <xrpl/protocol/st.h>

namespace ripple {

bool
isUnsignedSetManifest(STTx const& tx) noexcept
{
    try
    {
        return tx.getTxnType() == ttMANIFEST_SET &&
            tx.isFieldPresent(sfSigningPubKey) &&
            tx.getSigningPubKey().empty() &&
            tx.isFieldPresent(sfTxnSignature) && tx.getSignature().empty() &&
            !tx.isFieldPresent(sfSigners);
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

    // 4. The manifest signature is the only authority in this lane, so the
    // outer transaction must not be malleable by its relayer. Pin every
    // optional field that ordinary account signing would otherwise
    // authenticate. preflight0 already pins NetworkID to the configured
    // network; sfFee is pinned to one computed value in checkFee(), where the
    // ledger fee schedule is available.
    if (isUnsignedSetManifest(tx) &&
        (tx.getFieldU32(sfSequence) != 0 || tx.isFieldPresent(sfFlags) ||
         tx.isFieldPresent(sfSourceTag) || tx.isFieldPresent(sfPreviousTxnID) ||
         tx.isFieldPresent(sfLastLedgerSequence) ||
         tx.isFieldPresent(sfAccountTxnID) ||
         tx.isFieldPresent(sfOperationLimit) || tx.isFieldPresent(sfMemos) ||
         tx.isFieldPresent(sfTicketSequence) ||
         tx.isFieldPresent(sfEmitDetails) ||
         tx.isFieldPresent(sfFirstLedgerSequence) ||
         tx.isFieldPresent(sfHookParameters) || tx.isFieldPresent(sfHookName)))
    {
        JLOG(j.warn())
            << "SetManifest: envelope must be unsigned with Sequence 0.";
        return temMALFORMED;
    }

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

/** The canonical unsigned sfFee: the same 1.2x headroom Submit applies.

    Kept in one place so the value Submit writes and the value preclaim will
    accept cannot drift apart.
*/
static XRPAmount
manifestFeeCeiling(XRPAmount baseFee)
{
    return mulRatio(baseFee, 12, 10, /*roundUp*/ true);
}

TER
SetManifest::checkFee(PreclaimContext const& ctx, XRPAmount baseFee)
{
    // Account-signed SetManifest transactions use ordinary fee semantics.
    // Their outer signature authenticates the chosen Fee, and they may enter
    // TxQ like any other account transaction.
    if (!isUnsignedSetManifest(ctx.tx))
        return Transactor::checkFee(ctx, baseFee);

    // The manifest signature does not cover the outer transaction. Accepting
    // a relayer-chosen Fee would make one manifest authorization produce many
    // transaction IDs, defeating transaction-level verification caching and
    // relay suppression. Every valid relayer therefore derives this same Fee
    // from the ledger fee schedule and manifest size. The 20% headroom remains
    // as one exact value, not a malleable band. Account-signed transactions
    // returned above and authenticate their independently chosen Fee normally.
    if (ctx.tx[sfFee].xrp() != manifestFeeCeiling(baseFee))
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
    ReadView const& openView,
    beast::Journal j)
{
    try
    {
        auto const man = deserializeManifest(manifest, j);
        if (!man || !man->verify())
            return std::nullopt;

        auto const encode = [&](XRPAmount fee) {
            return serializeHex(STTx(ttMANIFEST_SET, [&](STObject& obj) {
                obj.setAccountID(sfAccount, calcAccountID(man->masterKey));
                obj.setFieldU32(sfSequence, 0);
                obj.setFieldU32(sfNetworkID, networkID);
                obj.setFieldAmount(sfFee, fee);
                obj.setFieldVL(sfSigningPubKey, std::vector<std::uint8_t>{});
                obj.setFieldVL(sfTxnSignature, std::vector<std::uint8_t>{});

                // sfManifest is soeREQUIRED, so STObject::set(SOTemplate) has
                // already materialised it as a present, empty object. Fill
                // that one in: emitting a second is a duplicate field, which
                // STObject::set(SerialIter&) rejects on the way back in.
                SerialIter mit{manifest};
                obj.peekFieldObject(sfManifest).set(mit);
            }));
        };

        // calculateBaseFee() takes a parsed transaction, so encode once with a
        // placeholder fee purely to have something to price. The resulting fee
        // does not depend on the placeholder: it is derived from the length of
        // the manifest object and the ledger's base fee.
        auto const priced = strUnHex(encode(XRPAmount{0}));
        if (!priced || priced->empty())
            return std::nullopt;

        SerialIter sit{makeSlice(*priced)};
        STTx const probe{std::ref(sit)};

        // Submit the ceiling exactly. preclaim rejects anything above it, and
        // the floor rises with network load, so the ceiling is both always
        // acceptable and the value most likely to still clear the floor by the
        // time the transaction is applied.
        return encode(
            manifestFeeCeiling(SetManifest::calculateBaseFee(openView, probe)));
    }
    catch (std::exception const& e)
    {
        JLOG(j.warn()) << "makeSetManifestTx: " << e.what();
        return std::nullopt;
    }
}

}  // namespace ripple
