#ifndef RIPPLE_TX_EXPORTLEDGEROPS_H_INCLUDED
#define RIPPLE_TX_EXPORTLEDGEROPS_H_INCLUDED

#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/TER.h>

namespace ripple {

/// Shared ledger operations and validation for the export system.
/// Used by both the hook xport() API (inline path) and the
/// Export transactor (user-submitted ttEXPORT path).
namespace ExportLedgerOps {

/// Validate that the exported transaction's NetworkID doesn't target
/// the local network. Returns tesSUCCESS if OK, or a TER error code.
///
/// Rules (per upstream rippled Transactor.cpp):
///   - Networks <= 1024: sfNetworkID must NOT be present on txns
///   - Networks > 1024:  sfNetworkID is REQUIRED and must match
///
/// So: if exported tx has sfNetworkID matching local → self-target.
///     if local NETWORK_ID is 0 (unconfigured) and tx has no
///     sfNetworkID → can't distinguish self from cross-chain, reject.
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

    if (localNetworkID == 0 && !stx.isFieldPresent(sfNetworkID))
    {
        JLOG(j.warn()) << "ExportLedgerOps: rejected export with "
                          "unconfigured NETWORK_ID";
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

/// Create an ltEXPORTED_TXN entry in the global exportedDir().
/// Enforces maxPendingExports directory cap.
/// Marks the txn hash as SF_BAD in the hash router so it cannot
/// enter consensus on this chain.
///
/// @param view     The apply view to modify
/// @param app      Application reference (for hash router)
/// @param stx      The serialized transaction to export
/// @param txnId    Hash of the exported transaction
/// @param j        Journal for logging
/// @return tesSUCCESS or tecDIR_FULL
inline TER
createExportedTxn(
    ApplyView& view,
    Application& app,
    STTx const& stx,
    uint256 const& txnId,
    beast::Journal j)
{
    // Mark as SF_BAD so this txn never enters consensus on this chain.
    app.getHashRouter().setFlags(txnId, SF_BAD);

    auto exportedId = keylet::exportedTxn(txnId);
    auto sleExported = view.peek(exportedId);

    if (sleExported)
    {
        // Already exists — duplicate export, skip.
        JLOG(j.debug()) << "ExportLedgerOps: ltEXPORTED_TXN already exists for "
                        << txnId;
        return tesSUCCESS;
    }

    // Enforce maxPendingExports on the exported directory.
    {
        Keylet const expDirKey{keylet::exportedDir()};
        std::size_t dirSize = 0;
        std::shared_ptr<SLE const> sleDirNode;
        unsigned int uDirEntry{0};
        uint256 dirEntry{beast::zero};
        if (cdirFirst(view, expDirKey.key, sleDirNode, uDirEntry, dirEntry))
        {
            do
            {
                ++dirSize;
            } while (
                cdirNext(view, expDirKey.key, sleDirNode, uDirEntry, dirEntry));
        }

        if (dirSize >= ExportLimits::maxPendingExports)
        {
            JLOG(j.warn()) << "ExportLedgerOps: export directory at cap ("
                           << ExportLimits::maxPendingExports
                           << "), rejecting export " << txnId;
            return tecDIR_FULL;
        }
    }

    sleExported = std::make_shared<SLE>(exportedId);

    // Serialize the STTx into an sfExportedTxn inner object.
    ripple::Serializer s;
    stx.add(s);
    SerialIter sit(s.slice());
    sleExported->emplace_back(ripple::STObject(sit, sfExportedTxn));

    auto page =
        view.dirInsert(keylet::exportedDir(), exportedId, [&](SLE::ref sle) {
            (*sle)[sfFlags] = lsfEmittedDir;
        });

    if (page)
    {
        (*sleExported)[sfOwnerNode] = *page;
        (*sleExported)[sfLedgerSequence] = view.info().seq;
        (*sleExported)[sfTransactionHash] = txnId;
        view.insert(sleExported);
        JLOG(j.debug()) << "ExportLedgerOps: created ltEXPORTED_TXN for "
                        << txnId;
        return tesSUCCESS;
    }

    JLOG(j.warn()) << "ExportLedgerOps: directory full when inserting "
                   << txnId;
    return tecDIR_FULL;
}

/// Create an ltSHADOW_TICKET in the account's owner directory.
/// Only created if the exported transaction has sfTicketSequence.
///
/// @param view       The apply view to modify
/// @param account    The exporting account (pays reserve)
/// @param stx        The exported transaction (checked for sfTicketSequence)
/// @param txnId      Hash of the exported transaction
/// @param j          Journal for logging
/// @return tesSUCCESS, tecDIR_FULL, or tefINTERNAL
inline TER
createShadowTicket(
    ApplyView& view,
    AccountID const& account,
    STTx const& stx,
    uint256 const& txnId,
    beast::Journal j)
{
    if (!stx.isFieldPresent(sfTicketSequence))
        return tesSUCCESS;  // No ticket sequence → no shadow ticket needed.

    auto const ticketSeq = stx.getFieldU32(sfTicketSequence);
    auto const key = keylet::shadowTicket(account, ticketSeq);

    if (view.exists(key))
    {
        JLOG(j.warn()) << "ExportLedgerOps: shadow ticket already exists for "
                       << account << " seq=" << ticketSeq;
        return tefINTERNAL;
    }

    auto sle = std::make_shared<SLE>(key);
    sle->setAccountID(sfAccount, account);
    sle->setFieldU32(sfTicketSequence, ticketSeq);
    sle->setFieldH256(sfTransactionHash, txnId);
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
    auto sleAccount = view.peek(keylet::account(account));
    if (sleAccount)
        adjustOwnerCount(view, sleAccount, 1, j);

    JLOG(j.debug()) << "ExportLedgerOps: created shadow ticket for " << account
                    << " seq=" << ticketSeq << " tx=" << txnId;

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

    return tesSUCCESS;
}

}  // namespace ExportLedgerOps
}  // namespace ripple

#endif
