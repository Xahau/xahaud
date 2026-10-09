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
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/Serializer.h>

namespace ripple {

/** Export: validators multisign transactions for another network on behalf
    of a hook's account, using export keys the hook chooses.

    Export keys. Every UNLReport validator nominates its current export key,
    with a proof of possession, in its flag ledger validations. The UNL then
    reports it (ttUNL_REPORT ExportKeyReport) and the key is recorded, newest
    first, in sfExportKeys on the validator's master account (at most
    maxExportKeys). Keys rotate on epochs of keyRotationPeriod: a key reported
    in a later epoch than the current head is pushed, one reported in the same
    epoch replaces the head, so the array always spans maxExportKeys epochs.
    Each change bumps UNLReport.ExportKeysSeq.

    1. A hook emits ttEXPORT carrying the transaction (Account = hook account,
       empty SigningPubKey) and sfSignerEntries, the signer list its account
       holds on the other network. Applying it creates an ltEXPORTED_TXN and,
       if the transaction uses a TicketSequence and is bound back to this
       network (OperationLimit == NETWORK_ID), a shadow ticket that lets its
       XPOP be imported exactly once.
    2. Every validator holding an export key whose account is listed applies a
       ttEXPORT_SIGN to its next open ledger. Each is proposed by one validator
       only, so it loses its first round, but not being a pseudo-txn it is
       retried into every node's next open ledger and lands one ledger later.
       Only listed accounts can sign: the other network rejects a multisigned
       transaction if any signer is missing from its signer list. Signers are
       kept sorted, so the object always holds a submittable transaction.
    3. `window` ledgers after creation every node injects ttEXPORT_FINAL, which
       deletes the object: the DeletedNode's FinalFields are the result.
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

    /** A CronSet with tfCronExportRotation fires this long after each epoch
        begins, so validators offline at the boundary have rotated too, */
    static constexpr std::uint32_t rotationSettle = 24 * 60 * 60;

    /** plus up to this much more, fixed per account, so that exporters do not
        all fan out into UNL-signed exports at once */
    static constexpr std::uint32_t rotationSpread = 24 * 60 * 60;

    /** A UNLReport validator whose account does not exist yet is created with
        this many drops by the next ttGENESIS_MINT after a flag ledger */
    static constexpr std::int64_t validatorFundingDrops = 100'000'000;

    static constexpr std::uint32_t
    rotationEpoch(std::uint32_t closeTime)
    {
        return closeTime / keyRotationPeriod;
    }

    /** When `account`'s tfCronExportRotation cron next fires after `now`:
        rotationSettle plus its spread after an epoch boundary, so repeating
        it every keyRotationPeriod lands after each rotation. */
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

/** What an export key signs to prove that its holder nominated it */
Serializer
exportKeyProofData(
    PublicKey const& master,
    PublicKey const& exportKey,
    std::uint32_t networkID);

using ExportSign = Export;
using ExportFinal = Export;

/** Shadow tickets are HookState entries under a reserved namespace on the
    exporting account, keyed by TicketSequence. */
inline uint256 const shadowTicketNS =
    uint256::fromVoid("RESERVED NAMESPACE SHADOW TICKET");

inline Keylet
shadowTicket(AccountID const& acc, std::uint32_t ticketSeq)
{
    return keylet::hookState(acc, uint256(ticketSeq), shadowTicketNS);
}

}  // namespace ripple

#endif
