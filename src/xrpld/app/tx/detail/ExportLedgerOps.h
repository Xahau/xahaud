#ifndef RIPPLE_TX_EXPORTLEDGEROPS_H_INCLUDED
#define RIPPLE_TX_EXPORTLEDGEROPS_H_INCLUDED

#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/XRPAmount.h>

#include <functional>
#include <optional>

namespace ripple {

/// Shared ledger operations and validation for the export system.
/// Used by both hook-emitted and user-submitted ttEXPORT paths.
namespace ExportLedgerOps {

inline bool
isExportTxn(STTx const& stx)
{
    return stx.getTxnType() == ttEXPORT;
}

inline bool
isPendingExportTxn(STTx const& stx)
{
    return isExportTxn(stx) && stx.isFieldPresent(sfExportedTxn);
}

inline bool
isPendingExportWorkTxn(STTx const& stx)
{
    return isExportTxn(stx) &&
        (stx.isFieldPresent(sfExportedTxn) ||
         stx.isFieldPresent(sfEmitDetails));
}

inline std::optional<STTx>
innerExportedTx(STTx const& stx)
{
    if (!stx.isFieldPresent(sfExportedTxn))
        return std::nullopt;

    try
    {
        auto const& exportedObj = const_cast<STTx&>(stx)
                                      .peekAtField(sfExportedTxn)
                                      .downcast<STObject>();
        Serializer innerSer;
        exportedObj.add(innerSer);
        SerialIter sit(innerSer.slice());
        return STTx(std::ref(sit));
    }
    catch (std::exception const&)
    {
        return std::nullopt;
    }
}

inline std::size_t
exportTxnCount(ReadView const& view)
{
    std::size_t count = 0;
    for (auto const& tx : view.txs)
    {
        if (tx.first && isPendingExportWorkTxn(*tx.first))
            ++count;
    }
    return count;
}

inline bool
isPendingExportEmission(SLE const& sle)
{
    if (sle.getType() != ltEMITTED_TXN || !sle.isFieldPresent(sfEmittedTxn))
        return false;

    auto const& emittedObj = sle.peekAtField(sfEmittedTxn).downcast<STObject>();
    return emittedObj.isFieldPresent(sfTransactionType) &&
        emittedObj.getFieldU16(sfTransactionType) == ttEXPORT &&
        emittedObj.isFieldPresent(sfExportedTxn);
}

inline std::size_t
pendingExportEmissionCount(ReadView const& view)
{
    std::size_t count = 0;
    forEachItem(
        view, keylet::emittedDir(), [&](std::shared_ptr<SLE const> const& sle) {
            if (sle && isPendingExportEmission(*sle))
                ++count;
        });
    return count;
}

inline std::size_t
shadowTicketCount(ReadView const& view, AccountID const& account)
{
    std::size_t count = 0;
    forEachItem(view, account, [&](std::shared_ptr<SLE const> const& sle) {
        if (sle && sle->getType() == ltSHADOW_TICKET)
            ++count;
    });
    return count;
}

inline TER
checkExportTxnLimit(ReadView const& view, beast::Journal j)
{
    auto const pending = exportTxnCount(view);
    if (pending < ExportLimits::maxPendingExports)
        return tesSUCCESS;

    JLOG(j.warn()) << "ExportLedgerOps: export txn limit reached pending="
                   << pending << " max=" << +ExportLimits::maxPendingExports;
    return tecDIR_FULL;
}

inline TER
validateRetryWindow(STTx const& stx, LedgerIndex ledgerSeq, beast::Journal j)
{
    if (!stx.isFieldPresent(sfLastLedgerSequence))
    {
        JLOG(j.warn()) << "ExportLedgerOps: export missing "
                          "sfLastLedgerSequence";
        return temMALFORMED;
    }

    auto const lls = stx.getFieldU32(sfLastLedgerSequence);
    auto const maxLLS =
        static_cast<std::uint64_t>(ledgerSeq) + ExportLimits::maxRetryLedgers;
    if (lls > maxLLS)
    {
        JLOG(j.warn()) << "ExportLedgerOps: export LastLedgerSequence too far "
                          "ahead ledgerSeq="
                       << ledgerSeq << " lastLedgerSequence=" << lls
                       << " maxRetryLedgers=" << ExportLimits::maxRetryLedgers;
        return temMALFORMED;
    }

    return tesSUCCESS;
}

/// Validate that the exported transaction's NetworkID doesn't target
/// the local network. Returns tesSUCCESS if OK, or a TER error code.
///
/// Export only needs to prove the target is not this chain. An explicit
/// sfNetworkID names a target network, so matching the local NETWORK_ID is a
/// self-target. An absent sfNetworkID is the no-NetworkID target encoding used
/// by XRPL mainnet; if this source chain also uses that encoding, absent target
/// identity is ambiguous with self-target and must be rejected.
inline TER
validateNetworkID(
    STTx const& stx,
    std::uint32_t localNetworkID,
    beast::Journal j)
{
    if (stx.isFieldPresent(sfNetworkID) &&
        stx.getFieldU32(sfNetworkID) == localNetworkID)
    {
        JLOG(j.warn()) << "ExportLedgerOps: rejected export targeting "
                          "local NetworkID ("
                       << localNetworkID << ")";
        return temMALFORMED;
    }

    if (!requiresTxNetworkID(localNetworkID) &&
        !stx.isFieldPresent(sfNetworkID))
    {
        JLOG(j.warn()) << "ExportLedgerOps: rejected export with "
                          "ambiguous absent NetworkID";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

/// Validate that the exported transaction's Account matches the
/// expected exporting account.
inline TER
validateExportAccount(
    STTx const& stx,
    AccountID const& expectedAccount,
    beast::Journal j)
{
    if (!stx.isFieldPresent(sfAccount) ||
        stx.getAccountID(sfAccount) != expectedAccount)
    {
        JLOG(j.warn())
            << "ExportLedgerOps: exported txn account doesn't match exporter";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

/// Validate that the target transaction is the unsigned multisign payload that
/// validators will attest and ExportResultBuilder will later materialize.
inline TER
validateExportSigningFields(STTx const& stx, beast::Journal j)
{
    if (!stx.isFieldPresent(sfSigningPubKey) ||
        !stx.getFieldVL(sfSigningPubKey).empty() ||
        stx.isFieldPresent(sfTxnSignature) || stx.isFieldPresent(sfSigners))
    {
        JLOG(j.warn())
            << "ExportLedgerOps: exported tx must use an empty SigningPubKey "
               "without TxnSignature or Signers";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

/// Validate that the exported transaction uses TicketSequence
/// (Sequence must be 0). Exports must use tickets because a bounced
/// tx on the destination chain would jam sequential sequence numbers.
inline TER
validateTicketSequence(STTx const& stx, beast::Journal j)
{
    if (!stx.isFieldPresent(sfTicketSequence))
    {
        JLOG(j.warn())
            << "ExportLedgerOps: exported tx must have sfTicketSequence";
        return temMALFORMED;
    }

    if (stx.getFieldU32(sfSequence) != 0)
    {
        JLOG(j.warn()) << "ExportLedgerOps: exported tx Sequence must be 0 "
                          "when using TicketSequence";
        return temMALFORMED;
    }

    return tesSUCCESS;
}

/// Create an ltSHADOW_TICKET in the account's owner directory.
/// Only created if the exported transaction has sfTicketSequence.
///
/// @param view       The apply view to modify
/// @param account    The exporting account (pays reserve)
/// @param stx        The exported transaction (checked for sfTicketSequence)
/// @param priorBalance Exporting account balance before this transaction fee
/// @param j          Journal for logging
/// @return tesSUCCESS, tecDUPLICATE, tecDIR_FULL, tecINSUFFICIENT_RESERVE,
///         or tefINTERNAL
inline TER
createShadowTicket(
    ApplyView& view,
    AccountID const& account,
    STTx const& stx,
    XRPAmount const& priorBalance,
    beast::Journal j)
{
    //@@start current-shadow-ticket-create
    if (!stx.isFieldPresent(sfTicketSequence))
        return tesSUCCESS;  // No ticket sequence → no shadow ticket needed.

    auto const ticketSeq = stx.getFieldU32(sfTicketSequence);
    auto const key = keylet::shadowTicket(account, ticketSeq);
    auto const intentHash = ExportResultBuilder::exportIntentHash(stx);

    // A shadow ticket is a pending-callback LATCH, not a permanent replay
    // tombstone. This check only rejects a currently-LIVE latch: after an
    // import consumes (erases) it, the same account can re-export the identical
    // inner tx and recreate the same (account, ticketSeq) latch. The recreated
    // latch stores the same intent hash, so the ORIGINAL XPOP passes the Import
    // identity check again, firing the callback once more. Unlike Burn-to-Mint
    // (guarded globally by the monotonic sfImportSequence), the ticket path has
    // no protocol-level replay guard — value-bearing import-callback hooks must
    // dedup on the XPOP target transaction hash or a hook-defined business key
    // in Hook State. A protocol-level exactly-once tombstone (consume-in-place,
    // expiring with the XPOP validity window) is possible future work.
    if (view.exists(key))
    {
        JLOG(j.warn()) << "ExportLedgerOps: shadow ticket already exists for "
                       << account << " seq=" << ticketSeq;
        return tecDUPLICATE;
    }

    auto const pending = shadowTicketCount(view, account);
    if (pending >= ExportLimits::maxPendingExports)
    {
        JLOG(j.warn()) << "ExportLedgerOps: shadow ticket limit reached for "
                       << account << " pending=" << pending
                       << " max=" << +ExportLimits::maxPendingExports;
        return tecDIR_FULL;
    }

    auto sleAccount = view.peek(keylet::account(account));
    if (!sleAccount)
        return tefINTERNAL;

    // Like TicketCreate, use the pre-fee balance so fees may dip into reserve,
    // but the new owned object itself still requires the next owner reserve.
    auto const requiredReserve =
        view.fees().accountReserve(sleAccount->getFieldU32(sfOwnerCount) + 1);
    if (priorBalance < requiredReserve)
    {
        JLOG(j.warn())
            << "ExportLedgerOps: insufficient reserve for shadow ticket"
            << " account=" << account
            << " ownerCount=" << sleAccount->getFieldU32(sfOwnerCount)
            << " required=" << requiredReserve
            << " priorBalance=" << priorBalance;
        return tecINSUFFICIENT_RESERVE;
    }

    auto sle = std::make_shared<SLE>(key);
    sle->setAccountID(sfAccount, account);
    sle->setFieldU32(sfTicketSequence, ticketSeq);
    sle->setFieldH256(sfDigest, intentHash);
    sle->setFieldU32(sfLedgerSequence, view.info().seq);

    auto page = view.dirInsert(
        keylet::ownerDir(account), key, describeOwnerDir(account));

    if (!page)
    {
        JLOG(j.warn())
            << "ExportLedgerOps: owner dir full for shadow ticket, account="
            << account;
        return tecDIR_FULL;
    }

    sle->setFieldU64(sfOwnerNode, *page);
    view.insert(sle);

    // Bump owner count for reserve.
    adjustOwnerCount(view, sleAccount, 1, j);

    JLOG(j.debug()) << "ExportLedgerOps: created shadow ticket for " << account
                    << " seq=" << ticketSeq << " intent=" << intentHash;

    //@@end current-shadow-ticket-create
    return tesSUCCESS;
}

/// Cancel (delete) an ltSHADOW_TICKET. Frees the owner reserve.
/// The account must own the shadow ticket.
///
/// @param view       The apply view to modify
/// @param account    The owning account
/// @param ticketSeq  The ticket sequence to cancel
/// @param j          Journal for logging
/// @return tesSUCCESS or tecNO_ENTRY
inline TER
cancelShadowTicket(
    ApplyView& view,
    AccountID const& account,
    std::uint32_t ticketSeq,
    beast::Journal j)
{
    //@@start current-shadow-ticket-cancel
    auto const key = keylet::shadowTicket(account, ticketSeq);
    auto sle = view.peek(key);

    if (!sle)
    {
        JLOG(j.warn()) << "ExportLedgerOps: no shadow ticket to cancel for "
                       << account << " seq=" << ticketSeq;
        return tecNO_ENTRY;
    }

    // Verify ownership.
    if (sle->getAccountID(sfAccount) != account)
    {
        JLOG(j.warn()) << "ExportLedgerOps: shadow ticket ownership mismatch";
        return tecNO_PERMISSION;
    }

    // Remove from owner directory.
    if (!view.dirRemove(
            keylet::ownerDir(account),
            sle->getFieldU64(sfOwnerNode),
            key,
            false))
    {
        JLOG(j.warn())
            << "ExportLedgerOps: failed to remove shadow ticket from owner dir";
        return tefBAD_LEDGER;
    }

    view.erase(sle);

    // Decrement owner count to free reserve.
    auto sleAccount = view.peek(keylet::account(account));
    if (sleAccount)
        adjustOwnerCount(view, sleAccount, -1, j);

    JLOG(j.debug()) << "ExportLedgerOps: cancelled shadow ticket for "
                    << account << " seq=" << ticketSeq;

    //@@end current-shadow-ticket-cancel
    return tesSUCCESS;
}

}  // namespace ExportLedgerOps
}  // namespace ripple

#endif
