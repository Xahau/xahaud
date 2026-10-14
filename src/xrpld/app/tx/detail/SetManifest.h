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

#include <optional>

namespace ripple {

/** Return whether empty outer signatures nominate manifest-only authority. */
bool
hasManifestAuthorityMarkers(STTx const& tx) noexcept;

/** Return whether every outer byte has the canonical SetManifest shape.

    Account is derived from the manifest's master key and NetworkID from
    `networkID`, this server's network. Only the Fee is mirrored from the
    candidate; checkFee pins it against the applying view.
*/
bool
hasCanonicalSetManifestShape(STTx const& tx, std::uint32_t networkID) noexcept;

/** Return the one canonical Fee for a SetManifest carrying `manifest`.

    The Fee is `base * (100 + 10 * manifestBytes)`, where `base` is the voted
    reference fee of the ledger the transaction is checked against, which is
    its parent's. Every node reading the same ledger computes the same value,
    so one manifest maps to one transaction ID per fee setting; local load
    never enters it.

    Returns nullopt when there is no canonical Fee: a zero base, or a product
    that overflows or is not a legal amount.
*/
std::optional<XRPAmount>
canonicalSetManifestFee(XRPAmount base, STObject const& manifest);

/** Encode the transaction that publishes `manifest` on-ledger.

    A manifest transaction carries no account signature, so the whole
    envelope is one exact transaction: Account is the manifest master key's
    account, Sequence is 0, SigningPubKey and TxnSignature are empty, NetworkID
    is this network's, and Fee is the canonical Fee for `openView`'s base fee.
    No other field is allowed. SetManifest::preflight and SetManifest::checkFee
    reject anything else, and every caller that submits a manifest builds it
    here.

    @param manifest Serialized manifest
    @param networkID Network the transaction is for
    @param openView Open ledger the Fee is priced against
    @param j Journal

    @return the hex-encoded transaction, or nullopt if the manifest does not
        parse or does not verify, or there is no canonical Fee
*/
std::optional<std::string>
makeSetManifestTx(
    Slice const& manifest,
    std::uint32_t networkID,
    ReadView const& openView,
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
