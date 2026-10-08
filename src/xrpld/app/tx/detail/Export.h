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

namespace ripple {

/** Export: the UNL multisigns transactions for another network on behalf of
    a hook's account.

    1. A hook emits ttEXPORT carrying the transaction (Account = hook account,
       empty SigningPubKey). Applying it creates an ltEXPORTED_TXN and, if the
       transaction uses a TicketSequence and is bound back to this network
       (OperationLimit == NETWORK_ID), a shadow ticket that lets its XPOP be
       imported exactly once.
    2. Every UNL validator with an on-ledger manifest applies a ttEXPORT_SIGN
       to its next open ledger. Each is proposed by one validator only, so it
       loses its first round, but not being a pseudo-txn it is retried into
       every node's next open ledger and lands one ledger later. Signers are
       kept sorted inside the exported transaction, so the object always holds
       a submittable multisigned transaction.
    3. `window` ledgers after creation every node injects ttEXPORT_FINAL, which
       deletes the object: the DeletedNode's FinalFields are the result.
*/
class Export : public Transactor
{
public:
    static constexpr ConsequencesFactoryType ConsequencesFactory{Normal};
    static constexpr std::uint32_t window = 4;

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

    /** Sign (if a validator) and finalize pending exports. Called from
        TxQ::accept; returns true if the open view changed. */
    static bool
    accept(Application& app, OpenView& view, beast::Journal j);
};

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
