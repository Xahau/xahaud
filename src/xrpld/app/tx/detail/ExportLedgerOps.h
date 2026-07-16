#ifndef RIPPLE_TX_EXPORTLEDGEROPS_H_INCLUDED
#define RIPPLE_TX_EXPORTLEDGEROPS_H_INCLUDED

#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/ledger/ApplyView.h>
#include <xrpld/ledger/Sandbox.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/Log.h>
#include <xrpl/protocol/ExportCommittee.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TER.h>
#include <xrpl/protocol/XRPAmount.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <optional>

namespace ripple {

/// Shared ledger operations and validation for the export system.
/// Used by both hook-emitted and user-submitted ttEXPORT paths.
namespace ExportLedgerOps {

inline NotTEC
validateCommitteeShape(STTx const& stx, beast::Journal j)
{
    if (!stx.isFieldPresent(sfExportUniverseHash) ||
        stx.getFieldH256(sfExportUniverseHash).isZero() ||
        !stx.isFieldPresent(sfExportCommittee))
    {
        JLOG(j.warn()) << "ExportLedgerOps: missing Export committee binding";
        return temMALFORMED;
    }

    auto const& committee = stx.getFieldVL(sfExportCommittee);
    if (committee.empty() ||
        committee.size() > ExportLimits::maxCommitteeMaskBytes)
    {
        JLOG(j.warn()) << "ExportLedgerOps: malformed committee bitmap bytes="
                       << committee.size();
        return temMALFORMED;
    }

    std::size_t selected = 0;
    for (auto const byte : committee)
        selected += std::popcount(byte);
    if (selected == 0 || selected > ExportLimits::maxCommitteeMembers)
    {
        JLOG(j.warn()) << "ExportLedgerOps: invalid committee population="
                       << selected;
        return temMALFORMED;
    }
    return tesSUCCESS;
}

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

inline std::uint16_t
exportLatchCount(SLE const& sle)
{
    return sle.isFieldPresent(sfExportCount) ? sle.getFieldU16(sfExportCount)
                                             : 0;
}

/// Link a new-format Export latch into its owner's directory and the global
/// pending-work directory. All mutations remain in the caller's apply sandbox
/// and are behavior-neutral until the post-validation Export cutover calls it.
inline TER
insertPendingExportLatch(
    ApplyView& view,
    RawView& rawView,
    std::shared_ptr<SLE> const& latch,
    beast::Journal j)
{
    if (!latch || latch->getType() != ltEXPORT_LATCH ||
        !latch->isFieldPresent(sfAccount) ||
        !latch->isFieldPresent(sfTransactionHash) ||
        latch->isFieldPresent(sfExportNode))
        return tefINTERNAL;

    auto const account = latch->getAccountID(sfAccount);
    auto const expected =
        keylet::exportLatch(account, latch->getFieldH256(sfTransactionHash));
    if (latch->key() != expected.key)
        return tefINTERNAL;
    if (view.exists(expected))
        return tecDUPLICATE;

    Sandbox sb{&view};
    auto sleAccount = sb.peek(keylet::account(account));
    if (!sleAccount)
        return tefBAD_LEDGER;

    auto const pendingKey = keylet::pendingExports();
    auto pendingRoot = sb.peek(pendingKey);
    if (pendingRoot && !pendingRoot->isFieldPresent(sfExportCount))
        return tefBAD_LEDGER;

    auto const accountCount = exportLatchCount(*sleAccount);
    auto const globalCount = pendingRoot ? exportLatchCount(*pendingRoot) : 0;
    constexpr auto maxCount = std::numeric_limits<std::uint16_t>::max();
    if (accountCount == maxCount || globalCount == maxCount)
        return tecDIR_FULL;
    if (globalCount >= ExportLimits::maxLiveExportLatches)
        return tecDIR_FULL;
    auto inserted = std::make_shared<SLE>(*latch);
    auto const ownerPage = sb.dirInsert(
        keylet::ownerDir(account), inserted->key(), describeOwnerDir(account));
    if (!ownerPage)
        return tecDIR_FULL;

    auto const pendingPage = sb.dirInsert(
        pendingKey, inserted->key(), [](std::shared_ptr<SLE> const&) {});
    if (!pendingPage)
        return tecDIR_FULL;

    inserted->setFieldU64(sfOwnerNode, *ownerPage);
    inserted->setFieldU64(sfExportNode, *pendingPage);
    sb.insert(inserted);

    sleAccount->setFieldU16(sfExportCount, accountCount + 1);
    sb.update(sleAccount);
    adjustOwnerCount(sb, sleAccount, 1, j);

    pendingRoot = sb.peek(pendingKey);
    if (!pendingRoot)
        return tefBAD_LEDGER;
    pendingRoot->setFieldU16(sfExportCount, globalCount + 1);
    sb.update(pendingRoot);

    sb.apply(rawView);

    return tesSUCCESS;
}

/** Create the enhanced origin-keyed Export latch at intent admission. */
inline TER
createPendingExportLatch(
    ApplyView& view,
    RawView& rawView,
    AccountID const& account,
    STTx const& exportTx,
    STTx const& identityTarget,
    uint256 const& universeHash,
    Blob const& committee,
    XRPAmount const& priorBalance,
    beast::Journal j)
{
    if (!identityTarget.isFieldPresent(sfTicketSequence) ||
        universeHash.isZero() || committee.empty())
        return temMALFORMED;

    auto const origin = exportTx.getTransactionID();
    auto const key = keylet::exportLatch(account, origin);
    if (view.exists(key))
        return tecDUPLICATE;

    auto const accountRoot = view.read(keylet::account(account));
    if (!accountRoot)
        return tefINTERNAL;
    auto const live = exportLatchCount(*accountRoot);
    if (live >= ExportLimits::maxPendingExports)
        return tecDIR_FULL;

    // A destination Ticket is a one-shot authority. Keep at most one live
    // enhanced latch for it even though origin txids are distinct.
    auto const ticketSeq = identityTarget.getFieldU32(sfTicketSequence);
    bool duplicateTicket = false;
    forEachItem(view, account, [&](std::shared_ptr<SLE const> const& sle) {
        if (sle && sle->getType() == ltEXPORT_LATCH &&
            sle->getFieldU32(sfTicketSequence) == ticketSeq)
            duplicateTicket = true;
    });
    if (duplicateTicket)
        return tecDUPLICATE;

    auto const requiredReserve =
        view.fees().accountReserve(accountRoot->getFieldU32(sfOwnerCount) + 1);
    if (priorBalance < requiredReserve)
        return tecINSUFFICIENT_RESERVE;

    auto latch = std::make_shared<SLE>(key);
    latch->setAccountID(sfAccount, account);
    latch->setFieldU32(sfTicketSequence, ticketSeq);
    latch->setFieldH256(sfTransactionHash, origin);
    latch->setFieldH256(
        sfDigest, ExportResultBuilder::exportIntentHash(identityTarget));
    latch->setFieldU32(sfLedgerSequence, view.info().seq);
    latch->setFieldH256(sfExportUniverseHash, universeHash);
    latch->setFieldVL(sfExportCommittee, committee);

    if (!exportTx.isFieldPresent(sfLastLedgerSequence))
        return temMALFORMED;
    auto const publicationEnd = static_cast<std::uint64_t>(view.info().seq) +
        ExportLimits::maxPublicationLedgers;
    if (publicationEnd > std::numeric_limits<std::uint32_t>::max())
        return tefINTERNAL;
    latch->setFieldU32(
        sfLastLedgerSequence, static_cast<std::uint32_t>(publicationEnd));

    return insertPendingExportLatch(view, rawView, latch, j);
}

/// Remove the pending-work link and release its global signing-work slot.
/// The per-account latch count and owner reserve remain held until terminal
/// latch erasure.
inline TER
removePendingExportLink(ApplyView& view, Keylet const& latchKey, beast::Journal)
{
    auto& sb = view;
    auto latch = sb.peek(latchKey);
    if (!latch || latch->getType() != ltEXPORT_LATCH)
        return tecNO_ENTRY;
    if (!latch->isFieldPresent(sfExportNode))
        return tesSUCCESS;

    auto const pendingKey = keylet::pendingExports();
    auto const pendingRoot = sb.peek(pendingKey);
    if (!pendingRoot || !pendingRoot->isFieldPresent(sfExportCount))
        return tefBAD_LEDGER;
    auto const pendingCount = exportLatchCount(*pendingRoot);
    if (pendingCount == 0)
        return tefBAD_LEDGER;
    if (!sb.dirRemove(
            pendingKey, latch->getFieldU64(sfExportNode), latchKey, true))
        return tefBAD_LEDGER;

    latch->makeFieldAbsent(sfExportNode);
    sb.update(latch);
    auto updatedRoot = sb.peek(pendingKey);
    if (!updatedRoot)
        return tefBAD_LEDGER;
    updatedRoot->setFieldU16(sfExportCount, pendingCount - 1);
    sb.update(updatedRoot);
    return tesSUCCESS;
}

/// Erase an enhanced Export latch and every remaining directory link, then
/// release its account count, optional pending-work slot, and owner reserve.
inline TER
eraseExportLatch(
    ApplyView& view,
    RawView& rawView,
    Keylet const& latchKey,
    beast::Journal j)
{
    Sandbox sb{&view};
    auto latch = sb.peek(latchKey);
    if (!latch || latch->getType() != ltEXPORT_LATCH)
        return tecNO_ENTRY;
    if (!latch->isFieldPresent(sfOwnerNode) ||
        !latch->isFieldPresent(sfAccount) ||
        !latch->isFieldPresent(sfTransactionHash))
        return tefBAD_LEDGER;

    auto const account = latch->getAccountID(sfAccount);
    if (keylet::exportLatch(account, latch->getFieldH256(sfTransactionHash))
            .key != latchKey.key)
        return tefBAD_LEDGER;

    auto sleAccount = sb.peek(keylet::account(account));
    auto const pendingKey = keylet::pendingExports();
    auto pendingRoot = sb.peek(pendingKey);
    if (!sleAccount || !pendingRoot ||
        !sleAccount->isFieldPresent(sfExportCount) ||
        !pendingRoot->isFieldPresent(sfExportCount))
        return tefBAD_LEDGER;

    auto const accountCount = exportLatchCount(*sleAccount);
    auto const globalCount = exportLatchCount(*pendingRoot);
    if (accountCount == 0)
        return tefBAD_LEDGER;

    auto const pending = latch->isFieldPresent(sfExportNode);
    if (pending)
    {
        if (globalCount == 0 ||
            !sb.dirRemove(
                pendingKey, latch->getFieldU64(sfExportNode), latchKey, true))
            return tefBAD_LEDGER;
    }

    if (!sb.dirRemove(
            keylet::ownerDir(account),
            latch->getFieldU64(sfOwnerNode),
            latchKey,
            false))
        return tefBAD_LEDGER;

    sleAccount->setFieldU16(sfExportCount, accountCount - 1);
    sb.update(sleAccount);
    adjustOwnerCount(sb, sleAccount, -1, j);

    if (pending)
    {
        pendingRoot = sb.peek(pendingKey);
        if (!pendingRoot)
            return tefBAD_LEDGER;
        pendingRoot->setFieldU16(sfExportCount, globalCount - 1);
        sb.update(pendingRoot);
    }
    sb.erase(latch);

    sb.apply(rawView);

    return tesSUCCESS;
}

/// Stop publication while retaining callback readiness and owner reserve.
inline TER
stopExportPublication(
    ApplyView& view,
    RawView& rawView,
    Keylet const& latchKey,
    beast::Journal j)
{
    Sandbox sb{&view};
    auto latch = sb.peek(latchKey);
    if (!latch || latch->getType() != ltEXPORT_LATCH ||
        !latch->isFieldPresent(sfAccount) ||
        !latch->isFieldPresent(sfTransactionHash))
        return tecNO_ENTRY;

    auto const account = latch->getAccountID(sfAccount);
    auto const origin = latch->getFieldH256(sfTransactionHash);
    if (keylet::exportLatch(account, origin).key != latchKey.key)
        return tefBAD_LEDGER;

    if (auto const ter = removePendingExportLink(sb, latchKey, j);
        !isTesSuccess(ter))
        return ter;

    latch = sb.peek(latchKey);
    if (!latch)
        return tefBAD_LEDGER;
    auto const flags = latch->isFieldPresent(sfFlags)
        ? latch->getFieldU32(sfFlags)
        : std::uint32_t{0};
    latch->setFieldU32(sfFlags, flags | lsfExportCanceled);
    sb.update(latch);
    sb.apply(rawView);
    return tesSUCCESS;
}

/** Apply an owner-authorized lifecycle operation to one exact Export W.

    Retain mode stops publication but preserves callback readiness. Erase mode
    removes the latch and knowingly forfeits any later callback.
 */
inline TER
controlExportLatch(
    ApplyView& view,
    RawView& rawView,
    AccountID const& account,
    uint256 const& origin,
    bool erase,
    beast::Journal j)
{
    auto const key = keylet::exportLatch(account, origin);
    auto const latch = view.read(key);
    if (!latch)
    {
        JLOG(j.warn()) << "ExportLedgerOps: no Export latch for " << account
                       << " origin=" << origin;
        return tecNO_ENTRY;
    }
    if (latch->getType() != ltEXPORT_LATCH ||
        !latch->isFieldPresent(sfAccount) ||
        !latch->isFieldPresent(sfTransactionHash) ||
        latch->getAccountID(sfAccount) != account ||
        latch->getFieldH256(sfTransactionHash) != origin)
        return tefBAD_LEDGER;

    auto const ter = erase ? eraseExportLatch(view, rawView, key, j)
                           : stopExportPublication(view, rawView, key, j);
    if (isTesSuccess(ter))
    {
        JLOG(j.debug()) << "ExportLedgerOps: " << (erase ? "erased" : "stopped")
                        << " Export latch for " << account
                        << " origin=" << origin;
    }
    return ter;
}

/** Deterministically stop expired publication work during paid Export work.

    Expiry releases the global signing-work slot, but the latch and owner
    reserve remain until symmetric completion or an explicit erase election.
 */
inline TER
pruneExpiredExportLatches(
    ApplyView& view,
    RawView& rawView,
    LedgerIndex currentSeq,
    beast::Journal j)
{
    std::vector<Keylet> expired;
    expired.reserve(ExportLimits::maxLiveExportLatches);
    forEachItem(
        view,
        keylet::pendingExports(),
        [&](std::shared_ptr<SLE const> const& latch) {
            if (!latch ||
                expired.size() >= ExportLimits::maxLiveExportLatches ||
                latch->getType() != ltEXPORT_LATCH ||
                !latch->isFieldPresent(sfLastLedgerSequence) ||
                currentSeq <= latch->getFieldU32(sfLastLedgerSequence))
                return;
            expired.push_back(Keylet{ltEXPORT_LATCH, latch->key()});
        });

    for (auto const& key : expired)
    {
        auto const ter = stopExportPublication(view, rawView, key, j);
        if (!isTesSuccess(ter) && ter != tecNO_ENTRY)
            return ter;
    }
    return tesSUCCESS;
}

/** Record callback execution without making witness arrival order observable.
 */
inline TER
recordExportXpop(
    ApplyView& view,
    RawView& rawView,
    Keylet const& latchKey,
    beast::Journal j)
{
    auto latch = view.peek(latchKey);
    if (!latch || latch->getType() != ltEXPORT_LATCH ||
        !latch->isFieldPresent(sfTransactionHash))
        return tecNO_ENTRY;

    auto const flags = latch->isFieldPresent(sfFlags)
        ? latch->getFieldU32(sfFlags)
        : std::uint32_t{0};
    if ((flags & lsfExportXpopSeen) != 0)
        return tecDUPLICATE;

    if (latch->isFieldPresent(sfExportSignatureHash))
        return eraseExportLatch(view, rawView, latchKey, j);

    latch->setFieldU32(sfFlags, flags | lsfExportXpopSeen);
    view.update(latch);
    return tesSUCCESS;
}

/** Record one canonical witness; erase only after XPOP has also arrived. */
inline TER
recordExportWitness(
    ApplyView& view,
    RawView& rawView,
    Keylet const& latchKey,
    uint256 const& witnessHash,
    beast::Journal j)
{
    auto latch = view.peek(latchKey);
    if (!latch || latch->getType() != ltEXPORT_LATCH ||
        !latch->isFieldPresent(sfTransactionHash))
        return tecNO_ENTRY;

    if (latch->isFieldPresent(sfExportSignatureHash))
    {
        if (latch->getFieldH256(sfExportSignatureHash) == witnessHash)
            return tesSUCCESS;
        return tecDUPLICATE;
    }

    auto const flags = latch->isFieldPresent(sfFlags)
        ? latch->getFieldU32(sfFlags)
        : std::uint32_t{0};
    if ((flags & lsfExportXpopSeen) != 0)
        return eraseExportLatch(view, rawView, latchKey, j);

    if (auto const ter = removePendingExportLink(view, latchKey, j);
        !isTesSuccess(ter))
        return ter;
    latch = view.peek(latchKey);
    if (!latch)
        return tefBAD_LEDGER;
    latch->setFieldH256(sfExportSignatureHash, witnessHash);
    view.update(latch);
    return tesSUCCESS;
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

/// Validate the embedded target transaction and its future canonical release
/// projection before accepting an Export intent. The all-zero origin/anchor
/// values are size-equivalent placeholders; live signing later substitutes
/// the real source transaction and validated-ledger identity.
inline TER
validateOriginMemoProjection(
    STTx const& stx,
    std::uint32_t sourceNetworkID,
    beast::Journal j)
{
    auto const targetNetworkID = stx.isFieldPresent(sfNetworkID)
        ? stx.getFieldU32(sfNetworkID)
        : std::uint32_t{0};
    auto const projected = ExportOriginMemo::releaseForm(
        stx,
        ExportOriginMemo::Origin{sourceNetworkID, targetNetworkID, uint256{}},
        ExportOriginMemo::Anchor{0, uint256{}});
    if (projected)
        return tesSUCCESS;

    JLOG(j.warn())
        << "ExportLedgerOps: exported tx cannot form canonical origin Memo"
        << " error=" << static_cast<unsigned>(projected.error());
    return temMALFORMED;
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

}  // namespace ExportLedgerOps
}  // namespace ripple

#endif
