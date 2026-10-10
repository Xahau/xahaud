//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#ifndef RIPPLE_TX_EXPORT_H_INCLUDED
#define RIPPLE_TX_EXPORT_H_INCLUDED

#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpl/protocol/Indexes.h>
#include <boost/filesystem.hpp>
#include <mutex>

namespace ripple {

class ValidatorKeys;

/** Export: UNL validators multisign transactions for another network on
    behalf of a hook's account, which has the same AccountID there.

    Export keys. Every UNLReport validator nominates its current export key,
    with a proof of possession, in its flag ledger validations. The UNL
    reports it (ttUNL_REPORT ExportKeyReport) and it is recorded, newest first,
    in sfExportKeys on the validator's account: one key per keyRotationPeriod
    epoch, at most maxExportKeys. Each change bumps UNLReport.ExportKeysSeq.
    A validator whose account does not exist yet gets one, with
    validatorFundingDrops, from the next ttGENESIS_MINT.

    Signer list. sfExportSignerList on an account is the signer list it holds
    on the other network, as last proven by an Import of its SignerListSet.
    Hooks never need to track it.

    1. A hook calls xport() (or emits a ttEXPORT) with the transaction.
       Applying it copies the account's sfExportSignerList into a new
       ltEXPORTED_TXN and, if the transaction uses a ticket and is bound back
       here (OperationLimit == NETWORK_ID), an ltSHADOW_TICKET: the XPOP of
       whichever transaction uses that ticket there can be imported once.
       Exported SignerListSets must be so bound, so the list here cannot go
       stale, and one without SignerEntries is completed with the current
       export keys at an 80% quorum.
    2. Every validator holding a listed export key applies a ttEXPORT_SIGN to
       its next open ledger. Each is proposed by one validator only, so it
       loses its first round, but not being a pseudo-txn it is retried into
       every node's next open ledger and lands one ledger later. A signature
       can only be checked against the ledger, so it is never relayed as a
       transaction: peers have it from the proposed set. Signers are kept
       sorted, so the object always holds a submittable transaction.
    3. `window` ledgers after creation every node injects ttEXPORT_FINAL,
       which deletes the object: the DeletedNode's FinalFields are the result.

    Servers with [xrpl_relay] submit exports that reach quorum to XRPL and
    import their XPOPs back (see ExportRelay.cpp).
*/
class Export : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};
    static constexpr std::uint32_t window = 4;

    /** Export keys rotate once per epoch of this many seconds */
    static constexpr std::uint32_t keyRotationPeriod = 90 * 24 * 60 * 60;

    /** The most export keys kept on a validator's account */
    static constexpr std::size_t maxExportKeys = 16;

    /** A tfCronExportRotation cron fires this long after each epoch begins,
        plus up to rotationSpread more, fixed per account, so validators
        offline at the boundary have rotated and exporters do not fan out at
        once */
    static constexpr std::uint32_t rotationSettle = 24 * 60 * 60;
    static constexpr std::uint32_t rotationSpread = 24 * 60 * 60;

    /** A UNLReport validator whose account does not exist yet is created with
        this many drops by the next ttGENESIS_MINT after a flag ledger */
    static constexpr std::int64_t validatorFundingDrops = 100'000'000;

    static constexpr std::uint32_t
    rotationEpoch(std::uint32_t closeTime)
    {
        return closeTime / keyRotationPeriod;
    }

    /** When `account`'s tfCronExportRotation cron next fires after `now` */
    static std::uint32_t
    rotationCronTime(std::uint32_t now, AccountID const& account)
    {
        auto const a = account.data();
        std::uint32_t const spread =
            ((std::uint32_t(a[0]) << 24) | (std::uint32_t(a[1]) << 16) |
             (std::uint32_t(a[2]) << 8) | std::uint32_t(a[3])) %
            rotationSpread;
        std::uint32_t const t =
            rotationEpoch(now) * keyRotationPeriod + rotationSettle + spread;
        return t > now ? t : t + keyRotationPeriod;
    }

    explicit Export(ApplyContext& ctx) : Transactor(ctx)
    {
    }

    static XRPAmount
    calculateBaseFee(ReadView const& view, STTx const& tx);

    static NotTEC
    preflight(PreflightContext const& ctx);

    static TER
    preclaim(PreclaimContext const& ctx);

    TER
    doApply() override;

    void
    preCompute() override;

    /** Sign (if holding a listed export key) and finalize pending exports.
        Called from TxQ::accept; returns true if the open view changed. */
    static bool
    accept(Application& app, OpenView& view, beast::Journal j);
};

using ExportSign = Export;
using ExportFinal = Export;

/** What an export key signs to prove that its holder nominated it */
Serializer
exportKeyProofData(
    PublicKey const& master,
    PublicKey const& exportKey,
    std::uint32_t networkID);

/** One export key per UNLReport validator: its newest recorded at or before
    `asOf` (default: now), sorted by account, at most maxMultiSigners. */
struct ExportSigner
{
    PublicKey validator;
    PublicKey key;
    std::uint32_t closeTime;
    AccountID account;
};

std::vector<ExportSigner>
exportSigners(ReadView const& view, std::optional<std::uint32_t> asOf = {});

/** A validator's export keys: independent random ed25519 keys, kept for as
    long as its account lists them. They live in [export_key_file] (default
    export_keys.txt in the database path), one hex secret per line, oldest
    first, the last being the nominee. As sensitive as the validator token:
    without it the validator cannot sign for exporters listing older keys.
    Independent keys make rotation worth something: a compromise that has
    ended does not reach keys generated after it. */
class ExportKeys
{
public:
    ExportKeys(Config const& config, ValidatorKeys const& vk, beast::Journal j);

    /** The key and proof of possession to nominate in the validation of
        flag ledger `ledger`. Rotates if there is no key, a new epoch began,
        or `ledger` moved past ours (an older backup); forgets keys `ledger`
        no longer lists. Empty unless this server is a validator. */
    std::optional<std::pair<PublicKey, Blob>>
    nominate(ReadView const& ledger);

    /** The held keys whose accounts are listed in sfSignerEntries `entries` */
    std::vector<std::pair<PublicKey, SecretKey>>
    signersFor(STArray const& entries) const;

private:
    struct Key
    {
        PublicKey pk;
        SecretKey sk;
        Blob proof;
    };

    Key
    make(SecretKey const& sk) const;

    void
    save() const;

    beast::Journal const j_;
    std::optional<PublicKey> master_;
    std::uint32_t const networkID_;
    std::optional<boost::filesystem::path> file_;
    mutable std::mutex mutex_;
    std::vector<Key> keys_;  // oldest first, back() is the nominee
};

/** [xrpl_relay]: carries exports to XRPL and their XPOPs back. Stops and
    joins on destruction. Null unless configured. */
struct ExportRelay
{
    virtual ~ExportRelay() = default;
};

std::unique_ptr<ExportRelay>
makeExportRelay(Application& app);

}  // namespace ripple

#endif
