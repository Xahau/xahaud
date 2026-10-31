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
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Rules.h>
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

    Exporters. An account becomes an exporter (lsfExporter, listed in
    keylet::exporterDir()) when an Import it signs records its signer list on
    the other network (sfExportSignerList): the one Import an operator sends
    by hand. From then on the ledger keeps three things in step for it, and a
    hook needs to track none of them:

      - sfExportSignerList, as last proven by an Import of its SignerListSet.
      - sfExportTickets, the tickets it holds unused there, as proven by
        Imports of its TicketCreates (ExportTickets below). Every export uses
        one. Below ticketLow the ledger exports a TicketCreate for ticketBatch
        more (ttEXPORT_AUTO, paid by the exporter); sfExportTicketPending
        holds that export's ticket until it returns.
      - sfExportKeysSeq, the UNLReport.ExportKeysSeq its list was built for.
        When the UNL's keys change the ledger exports a SignerListSet without
        entries (ttEXPORT_AUTO); its old list signs, so keys rotate without
        lockout, and the Import that brings it back records the new list.

    Servers with [xrpl_relay] watch every exporter on XRPL and Import, free
    of charge, each transaction of theirs that changes any of the three: the
    XRPL fee it burned pays for the Import (isSyncImport). Such an Import
    records; it never installs keys here, mints nothing, consumes no sequence
    and runs the exporter's hooks only as a collect call (hsfCOLLECT on the
    hook, lsfTshCollect on the account). asfExporter clears the flag.

    1. A hook calls xport() with the transaction: TransactionType and its
       own fields. Account, SigningPubKey, Sequence (0) and OperationLimit
       are filled in at emission; TicketSequence (the lowest ticket of the
       smallest range in sfExportTickets) and Fee ((signers + 1) x
       xrplBaseFee) when the ttEXPORT applies, unless the hook set them.
       Applying it copies sfExportSignerList into a new ltEXPORTED_TXN and
       writes an ltSHADOW_TICKET: the XPOP of whichever transaction uses that
       ticket there can be imported once, calling the hook back. A ticket is
       never both pooled and shadowed.
    2. Every validator holding a listed export key applies a ttEXPORT_SIGN to
       its next open ledger. Each is proposed by one validator only, so it
       loses its first round, but not being a pseudo-txn it is retried into
       every node's next open ledger and lands one ledger later. A signature
       can only be checked against the ledger, so it is never relayed as a
       transaction: peers have it from the proposed set. Signers are kept
       sorted, so the object always holds a submittable transaction.
    3. `window` ledgers after creation every node injects ttEXPORT_FINAL,
       which deletes the object: the DeletedNode's FinalFields are the result.
       Below quorum nothing was submitted: the ticket returns to the pool and
       the shadow ticket goes, so an identical export draws the same ticket.
    4. The XPOP of the transaction that used the ticket there is imported
       here by anyone: free, consuming no sequence and running none of the
       account's hooks, it consumes the shadow ticket.

    Callbacks. A hook hears of its export as it would of an emitted txn, by
    cbak, once: what == 2 when the Import of step 4 lands (xpop_slot() gives
    the transaction there and its meta, which may be another transaction that
    used the same ticket), or what == 1 if the export closes below quorum in
    step 3 or fails to apply. A transaction the other network rejects outright
    never comes back, so no callback follows; its ticket is still there, and
    relays keep resubmitting while it is.
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

    /** A UNLReport validator whose account does not exist yet is created with
        this many drops by the next ttGENESIS_MINT after a flag ledger */
    static constexpr std::int64_t validatorFundingDrops = 100'000'000;

    /** The ledger exports a TicketCreate for an exporter holding fewer unused
        tickets than ticketLow, for ticketBatch more (XRPL allows 250 per
        account, so an exporter never holds more than that) */
    static constexpr std::uint32_t ticketLow = 10;
    static constexpr std::uint32_t ticketBatch = 50;

    /** The other network's reference fee: an exported transaction pays
        (signers + 1) times this unless the hook set its Fee */
    static constexpr XRPAmount xrplBaseFee{10};

    /** Each exporter is considered for an automatic export once every this
        many ledgers, at a ledger chosen by its account id, so the ledger does
        not re-examine every exporter every ledger */
    static constexpr std::uint32_t autoEvery = 16;

    static constexpr std::uint32_t
    rotationEpoch(std::uint32_t closeTime)
    {
        return closeTime / keyRotationPeriod;
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
    preCompute() override
    {
        // ttEXPORT_SIGN, ttEXPORT_FINAL and ttEXPORT_AUTO have no account
    }

    /** Sign (if holding a listed export key) and finalize pending exports,
        and export what exporters need. Called from TxQ::accept; returns true
        if the open view changed. */
    static bool
    accept(Application& app, OpenView& view, beast::Journal j);

private:
    TER
    createExport(
        AccountID const& owner,
        STObject inner,
        std::optional<uint256> const& hook,
        bool automatic);
};

using ExportSign = Export;
using ExportFinal = Export;
using ExportAuto = Export;

/** Whether `tx` is an Import by (not necessarily signed by) an existing
    account that is free and uses no sequence: an export coming back, or a
    sync import of an exporter's activity there. Its XRPL fee paid for it.
    (A first Import also has Sequence 0, but no account yet.) */
inline bool
isLoopback(Rules const& rules, STTx const& tx)
{
    return rules.enabled(featureExport) && tx.getTxnType() == ttIMPORT &&
        tx[sfSequence] == 0 && !tx.isFieldPresent(sfTicketSequence);
}

/** The tickets an exporter holds unused on the other network, as kept in
    sfExportTickets: a VECTOR256 of six ranges per element, each a big-endian
    uint32 first ticket and a uint8 count, packed from the front, the last 16
    bits zero. Ranges are sorted, disjoint, non-adjacent (merged) and never
    longer than 255; empty slots only at the tail; no empty elements. XRPL
    allows 250 tickets per account, so the vector stays small however
    fragmented. */
class ExportTickets
{
public:
    struct Range
    {
        std::uint32_t first;
        std::uint32_t count;
        bool
        operator==(Range const&) const = default;
    };

    ExportTickets() = default;
    explicit ExportTickets(STVector256 const& v);

    /** Add [first, first + count) */
    void
    add(std::uint32_t first, std::uint32_t count);

    /** Add one ticket */
    void
    insert(std::uint32_t ticket)
    {
        add(ticket, 1);
    }

    /** Remove one ticket; false if it was not held */
    bool
    remove(std::uint32_t ticket);

    bool
    contains(std::uint32_t ticket) const;

    /** The lowest ticket of the smallest range, removed: so fragments made
        by returned tickets are used up first, and an export repeated after
        closing below quorum draws the ticket it had */
    std::optional<std::uint32_t>
    take();

    std::uint32_t
    count() const;

    bool
    empty() const
    {
        return ranges_.empty();
    }

    std::vector<Range> const&
    ranges() const
    {
        return ranges_;
    }

    STVector256
    pack() const;

    /** Write to an exporter's account root, removing the field when empty */
    void
    store(SLE& acc) const;

    /** Read from an account root; empty if absent */
    static ExportTickets
    load(SLE const& acc);

private:
    void
    normalize();

    std::vector<Range> ranges_;
};

/** Whether `entries` lists the account of any UNLReport validator's export
    key. Importing such a SignerListSet records it as the account's list
    there but never installs it here, as that would let the UNL sign for
    the account on this network. */
bool
listsExportKey(ReadView const& view, STArray const& entries);

/** The weight of the signatures an export has gathered: an ltEXPORTED_TXN,
    or the FinalFields its ttEXPORT_FINAL left in its DeletedNode */
std::uint32_t
exportWeight(STObject const& exported);

/** What an export key signs to prove that its holder nominated it */
Serializer
exportKeyProofData(
    PublicKey const& master,
    PublicKey const& exportKey,
    std::uint32_t networkID);

/** Set or clear lsfExporter on `acc`, keeping keylet::exporterDir() in
    step. `acc` is updated by the caller. */
TER
setExporter(ApplyView& view, std::shared_ptr<SLE> const& acc, bool on);

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

/** [xrpl_relay]: carries exports to XRPL and exporters' activity there back.
    Stops and joins on destruction. Null unless configured. */
struct ExportRelay
{
    virtual ~ExportRelay() = default;
};

std::unique_ptr<ExportRelay>
makeExportRelay(Application& app);

}  // namespace ripple

#endif
