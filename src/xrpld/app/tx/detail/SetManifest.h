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

#ifndef RIPPLE_TX_SETMANIFEST_H_INCLUDED
#define RIPPLE_TX_SETMANIFEST_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/core/Config.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/PublicKey.h>

#include <optional>

namespace ripple {

/** Return whether empty outer signatures nominate manifest-only authority.

    This deliberately includes malformed candidates with attached Signers so
    ingress can reject their non-canonical envelope before multisign work.
*/
bool
hasManifestAuthorityMarkers(STTx const& tx) noexcept;

/** Return whether a SetManifest envelope uses manifest-only authority.

    This is the one shared lane discriminator. An account-signed SetManifest
    follows ordinary transaction signature, sequence, fee, and TxQ rules.
    Only the exact empty outer-signature shape is eligible for the special
    manifest-authorized lane; SetManifest::preflight pins its remaining
    envelope fields.
*/
bool
isUnsignedSetManifest(STTx const& tx) noexcept;

/** Return whether every outer byte has the manifest-authorized shape.

    Account is derived from the manifest's master key and NetworkID from
    `networkID`, this server's network. Only the Fee is mirrored from the
    candidate; checkFee pins it against the applying view.
*/
bool
hasCanonicalUnsignedSetManifestShape(
    STTx const& tx,
    std::uint32_t networkID) noexcept;

/** Return the one canonical Fee for a manifest-authorized update.

    The Fee is `base * (100 + 10 * manifestBytes)`, where `base` is the voted
    reference fee (`fees().base`) of the ledger the transaction is checked
    against. Every node reading the same ledger computes the same value, so
    one manifest maps to one transaction ID per fee setting; local load never
    enters it. Account-signed SetManifest transactions use ordinary fee
    calculation instead.

    Returns nullopt when there is no canonical Fee: a zero base, or a product
    that overflows or is not a legal amount. The function owns serialization
    so callers cannot disagree about the byte count.
*/
std::optional<XRPAmount>
canonicalUnsignedSetManifestFee(XRPAmount base, STObject const& manifest);

/** Return the current on-ledger sequence for a registered master key.

    Absence is the anti-entropy boundary: background publication may update an
    existing registration but must never bootstrap one.
*/
std::optional<std::uint32_t>
onLedgerManifestSequence(ReadView const& view, PublicKey const& masterKey);

/** Encode a manifest-authorized update transaction for `manifest`.

    This lane carries no account signature: the manifest signature is its only
    authority. Consequently the outer transaction cannot leave relayer-chosen
    bytes that produce multiple transaction IDs for the same authorization.
    It can update only an existing manifest slot. Preflight reconstructs the
    one canonical outer envelope and compares its complete serialized bytes;
    preflight0 pins NetworkID and checkFee pins the one computed Fee. No
    relayer-chosen optional field or alternate encoding is admitted.
    Initial registration uses an ordinary account-signed SetManifest instead.

    Returns hex because both callers feed the ordinary tx_blob submission
    path. The manifest is parsed and verified before the shared canonical
    envelope builder serializes it with the canonical Fee for `base`.

    @param manifest Serialized manifest
    @param networkID Network the transaction is for
    @param base Voted reference fee of the open ledger the transaction is
        submitted to
    @param j Journal

    @return the hex-encoded transaction, or nullopt if the manifest does not
        parse or does not verify, or there is no canonical Fee for `base`
*/
std::optional<std::string>
makeSetManifestTx(
    Slice const& manifest,
    std::uint32_t networkID,
    XRPAmount base,
    beast::Journal j);

class SetManifest : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Custom};

    explicit SetManifest(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    // Hides Transactor::checkFee; applySteps dispatches this as T::checkFee.
    static TER
    checkFee(PreclaimContext const& ctx, XRPAmount baseFee);

    static TxConsequences
    makeTxConsequences(PreflightContext const& ctx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const&);

    TER
    doApply() override;
};

}  // namespace ripple

#endif
