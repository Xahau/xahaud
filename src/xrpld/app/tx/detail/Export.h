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
#include <boost/filesystem/path.hpp>
#include <mutex>

namespace ripple {

class ExportTickets;
class ValidatorKeys;

/** Export: UNL validators multisign transactions for other networks on
    behalf of a hook's account, which has the same AccountID there.

    Networks. Every XRPL-protocol network has an id (0 XRPL, 1 testnet, 2
    devnet, 21337 Xahau). An export names its destination by the NetworkID
    of the transaction the hook supplies, absent meaning 0: that is how such
    a transaction names its network anyway where the id is over 1024, and
    below that (where the other network refuses the field) the ledger strips
    it when the export applies, having recorded where it is bound
    (sfRemoteNetworkID). One convenience: where exactly one import VL key is
    trusted at all, xport() and export_setup take a transaction that names
    no network to be for that key's network rather than for 0
    (Import::soleNetwork), so a network that only knows a testnet, say,
    need not say so each time. An account becomes an exporter on any of
    them the same way, by the Import it signs of its SignerListSet there;
    from a network other than the XRPL that Import records the list and
    does nothing else (no keys installed here, nothing minted).
    Everything the ledger keeps about an account on another network is per
    network: its ltEXPORTER, its shadow tickets, the fee proven there
    (ltEXPORT_FEE). What comes back names its network by the validator list
    that proves it (Import). Validators' export keys are the same on every
    network, so nothing about them is per network.

    Export keys. Every UNLReport validator nominates its current export key,
    with a proof of possession, in its flag ledger validations. The UNL
    reports it (ttUNL_REPORT ExportKeyReport) and it is recorded, newest first,
    in sfExportKeys on the validator's account: one key per keyRotationPeriod
    epoch, at most maxExportKeys. Each change bumps UNLReport.ExportKeysSeq.
    A validator whose account does not exist yet gets one, with
    validatorFundingDrops, from the next ttGENESIS_MINT.

    Exporters. An account's standing on one other network is its ltEXPORTER
    (keylet::exporter(account, nid)). It becomes an exporter there (listed in
    keylet::exporterDir()) when an Import it signs records its signer list on
    that network (sfExportSignerList): the one Import an operator sends by
    hand. Only a list that is one of ours (listsExportKey: it names an
    export key, current or past, of a UNLReport validator) makes an
    exporter. A list recorded later that is not, the owner's own keys or
    none at all, unlists the record again, as does an export closing below
    quorum under such a list: the ledger cannot sign for it, so it stops
    exporting for it rather than try for ever. asfExporter lists it again
    once the list there is ours. From then on the ledger keeps four things
    in step for it, and a hook needs to track none of them:

      - sfExportSignerList, as last proven by an Import of its SignerListSet.
      - sfExportTickets, the tickets it holds unused there, as proven by
        Imports of its TicketCreates (ExportTickets below). Every export uses
        one. Below ticketLow the ledger exports a TicketCreate for ticketBatch
        more (ttEXPORT_AUTO, paid by the exporter); sfExportTicketPending
        holds that export's ticket until it returns. The last pooled ticket
        is the ledger's: a hook cannot draw it (hookMayUse), so a refill can
        always be carried by a ticket.
      - sfExportSequence, its Sequence there, as last proven by an Import:
        the AccountRoot node in every transaction's metadata carries it. This
        is the backstop for a pool that is nonetheless empty (a refill lost
        there, tickets a hook named by itself): with no ticket to carry a
        refill, the ledger exports a TicketCreate sequenced with it instead,
        once per Sequence it knows (sfExportTicketPending then holds that
        Sequence; tickets and sequences share one number space there). The
        Import of its result, succeeded or failed, records the Sequence it
        consumed, so a failed one (reserve there, say) is retried once the
        ledger sees the next Sequence, and nobody needs the account's keys
        to get it going again: funding the account there is enough.
      - sfExportKeysSeq, the UNLReport.ExportKeysSeq its list was built for.
        When the UNL's keys have changed the ledger exports a SignerListSet
        without entries (ttEXPORT_AUTO); its old list signs, so keys rotate
        without lockout, and the Import that brings it back records the new
        list. This is batched, not done change by change: an exporter is
        brought up to date at most once every keyRefreshEvery ledgers, and
        only if its list would differ. A validator's new key goes in front
        of its old ones (at most maxKeysPerEpoch per rotation epoch), and it
        keeps signing with the old ones while its account lists them, so
        nothing is lost by waiting and the other network sees one
        SignerListSet per exporter however many keys turned over.

    It also keeps the account's XRP balance there as last proven by an
    Import (sfBalance), which relays hold an export back on while it cannot
    pay its fee, and the list of its exports in flight (sfIndexes).

    Servers with an [export_relay] section per network watch every exporter
    there and Import, free of charge, each transaction of theirs that changes
    any of the four: the fee it burned there pays for the Import
    (isLoopback). Such an Import records; it never installs keys here, mints
    nothing, consumes no sequence and runs the exporter's hooks only as a
    collect call (hsfCOLLECT on the hook, lsfTshCollect on the account).
    asfExporter unlists the account on every network, and lists it again
    wherever it holds a list.

    1. A hook calls xport() with the transaction: TransactionType and its
       own fields, which must be ones both networks read the same way
       (ExportSchema: a type opened for export by amendment, with the fields
       and flags the other network accepts on it; NOT_EXPORTABLE otherwise).
       A TicketSequence the hook sets itself must be one the ledger knows
       of there (ticketUsable; NO_TICKET otherwise): xport_tickets() and
       xport_ticket() tell it which it holds.
       Account, SigningPubKey, Sequence (0) and OperationLimit are filled in
       at emission; TicketSequence (the lowest ticket of the smallest range
       in sfExportTickets), Fee ((signers + 1) x the unit ltEXPORT_FEE
       holds, see Ingest.h) and LastLedgerSequence (`expiry` past the ledger
       ltEXPORT_FEE last recorded for that network; none while nothing has
       been recorded) when the ttEXPORT applies, unless the hook set them.
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
    5. An export with a LastLedgerSequence that has not come back by the
       time a ledger `expiryGrace` past it is proven for that network
       (ttINGEST) is expired: a ttEXPORT_FINAL naming its shadow ticket
       erases it, returns the ticket to the pool and calls the hook back as
       failed. The other network refuses the transaction from that ledger
       on, so nothing is left waiting on it and no export stays stuck.
       (The ledger's own automatic exports carry no LastLedgerSequence:
       they are resent until they land.)

    Callbacks. In every callback about an export, otxn_id() is the id of
    the ttEXPORT the hook emitted, the 32 bytes xport() wrote: that is how a
    hook tells which of its exports it is hearing about, whichever
    transaction brought the news (otxn_id(1) is that transaction's own id).
    The export records it (sfExportTxnID) on its ltEXPORTED_TXN and shadow
    ticket for the purpose. A hook hears of its export as it would of an
    emitted txn, by cbak, once: what == 2 when the Import of step 4 brings
    back the export itself (xpop_slot() gives the transaction there and its
    meta), or what == 1 if that Import brings back anything else that used
    the ticket (the export then never will run; it is told apart by its id
    without signatures, sfTransactionHash, since any quorum of the UNL's
    signatures makes a transaction with an id of its own there), or if the
    export fails to apply, closes below quorum in step 3 or expires in
    step 5 (the otxn is then the
    ttEXPORT_FINAL, which carries sfLedgerIndex only when it is an expiry).
    A transaction the other network rejects outright never comes back; it
    expires instead, unless it has no LastLedgerSequence (the hook removed
    it, or nothing was known of that network's ledger yet), in which case
    its ticket stays and relays keep resubmitting.
    What what == 1 proves. After an expiry: that the transaction can no
    longer be included there, and that its result had not been imported
    `expiryGrace` ledgers after it last could have been. That is the ledger
    saying it failed; the one thing it cannot rule out is a transaction that
    did land in time and whose XPOP no relay brought back within that grace
    (it can still be imported afterwards, but the hook is not called again).
    After a close below quorum: only that the ledger did not gather a quorum
    on-ledger. Signatures are public as soon as they are proposed, including
    any that reached no ledger before the window closed, so until its
    LastLedgerSequence has passed there the transaction could still be
    assembled and submitted by someone else.

    The account there. Export is one way to act for the account on the other
    network, not the only one. The signer list hands it to the UNL's export
    keys at quorum, but nothing here disables or even sees the account's
    master key or regular key there: whoever holds them can still send
    transactions for it outside any export, spend tickets the ledger counts
    as pooled, or replace the signer list. The ledger learns of a
    TicketCreate, a SignerListSet or a pooled ticket spent that way when it
    is imported, and of nothing else. Whether those keys should be disabled
    there (asfDisableMaster, no regular key) is the account holder's
    decision and depends on what the hook is trusted with; export_setup says
    so too.
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

    /** The other network's reference fee: the least an exported transaction
        pays per signature, and what it pays until ttINGEST has proven what
        transactions there actually pay (ltEXPORT_FEE, see Ingest.h). A hook
        that sets its own Fee is left alone. */
    static constexpr XRPAmount xrplBaseFee{10};

    /** A hook's export that sets no LastLedgerSequence is given one this
        many ledgers past the ledger last proven for its network
        (ltEXPORT_FEE.sfLedgerSequence, kept current by ttINGEST), if any
        has been: long enough for the signing window, the relay and an
        ingest_every of lag in that record. */
    static constexpr std::uint32_t expiry = 256;

    /** An export with a LastLedgerSequence is declared failed once a ledger
        this many past it has been proven for its network and its result
        has not been imported: the time relays are given to bring back a
        result from a ledger at or before the LastLedgerSequence. */
    static constexpr std::uint32_t expiryGrace = 32;

    /** The most exports one account may have in flight on one network (the
        size of its ltEXPORTER's sfIndexes; the other network allows an
        account 250 tickets, and every export in flight holds one) */
    static constexpr std::size_t maxExpiring = 256;

    /** An exporter's signer list is brought up to the UNL's current export
        keys at most once every this many ledgers, at a ledger its directory
        entry picks. Keys change rarely and validators keep signing with the
        old ones for as long as their accounts list them, so however many
        validators re-key in the meantime it costs each exporter, and the
        other network, one SignerListSet. */
    static constexpr std::uint32_t keyRefreshEvery = 4096;

    /** The most export keys recorded for one validator in one rotation
        epoch: its key for the epoch and one replacement (a lost key file).
        A further nomination waits for the next epoch, so a validator, or two
        servers sharing its token, cannot keep the UNL's keys changing. */
    static constexpr std::size_t maxKeysPerEpoch = 2;

    /** The most exports of one exporter expired in one ledger */
    static constexpr std::size_t maxExpirePerSlot = 16;

    /** The network an export is for: the NetworkID of the transaction the
        hook supplied, absent meaning XRPL (see the class comment) */
    static std::uint32_t
    destination(STObject const& inner)
    {
        return inner[~sfNetworkID].value_or(0);
    }

    /** Each exporter is considered for an automatic export once every this
        many ledgers, at a ledger chosen by its entry in keylet::exporterDir()
        (slotOf), so the ledger reads only a fraction of the exporters each
        ledger */
    static constexpr std::uint32_t autoEvery = 16;

    /** The ledgers at which the exporter whose ltEXPORTER has key `key` is
        examined: those where (seq + slotOf(key)) % autoEvery == 0. Taken
        from the directory entry so the object need not be read first. */
    static std::uint32_t
    slotOf(uint256 const& key)
    {
        return (std::uint32_t(key.data()[0]) << 8) | key.data()[1];
    }

    /** Whether a hook may spend `ticket` (or, without one, the next pooled
        ticket) of `pool`: the last pooled ticket is kept for the ledger's own
        TicketCreate, which needs a ticket to carry it. Whether a ticket the
        pool does not hold may be named at all is ticketUsable's to say. */
    static bool
    hookMayUse(ExportTickets const& pool, std::optional<std::uint32_t> ticket);

    /** Whether a hook of `account` may name `ticket` for an export to
        network `nid`: only a ticket the ledger knows of. Either one in its
        pool there (and not the last, see hookMayUse), or one an export of
        its own is in flight on (it has a shadow ticket), which the new
        export then takes over: the way to replace an export that is stuck.
        Any other ticket the ledger never saw created, or saw spent, and an
        export on it could only wait for something that cannot come.
        xport() tells the hook (NO_TICKET); ttEXPORT's preclaim enforces it. */
    static bool
    ticketUsable(
        ReadView const& view,
        AccountID const& account,
        std::uint32_t nid,
        std::uint32_t ticket);

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

    /** What an automatic export to network `nid` costs its owner, debited by
        createExport: the pseudo-transaction itself carries no fee */
    static XRPAmount
    autoFee(ReadView const& view, AccountID const& owner, std::uint32_t nid);

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

    /** Write to an ltEXPORTER, removing the field when empty */
    void
    store(SLE& exporter) const;

    /** Read from an ltEXPORTER; empty if absent */
    static ExportTickets
    load(SLE const& exporter);

private:
    void
    normalize();

    std::vector<Range> ranges_;
};

/** Whether a signer list is "one of ours": whether `entries` lists the
    account of any export key, current or past (everything still in
    sfExportKeys), of any UNLReport validator. Importing such a
    SignerListSet records it as the account's list there but never installs
    it here, as that would let the UNL sign for the account on this network.
    A list that is not ours is the owner's own: the ledger cannot sign for
    it, so the record is not (or no longer) listed as an exporter. */
bool
listsExportKey(ReadView const& view, STArray const& entries);

/** Whether the UNLReport's validators are all there in `view`. The first
    validator report of a ledger resets the list and the rest refill it, so
    part-way through such a ledger a key that is not found may only be not
    found yet: nothing is concluded from its absence then (no record is
    unlisted, no signer list is built from it). */
bool
exportKeysSettled(ReadView const& view);

/** Take the shadow ticket `key` off the list of exports in flight of the
    ltEXPORTER `ex` (sfIndexes). True if it was there; `ex` is then updated
    by the caller. */
bool
untrackExpiry(SLE& ex, uint256 const& key);

/** The id `tx`, a transaction of another network, has without its
    signatures: what an export is known by here from the moment it is made
    (sfTransactionHash), whichever of the UNL's signatures it ends up
    carrying there. Zero if `tx` is not a transaction this network can read. */
uint256
exportUnsignedID(STObject const& tx);

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

/** List or unlist the ltEXPORTER `ex` in keylet::exporterDir() (sfExportNode
    holds its page while listed). `ex` is updated by the caller. */
TER
setExporter(ApplyView& view, std::shared_ptr<SLE> const& ex, bool on);

/** Whether the ltEXPORTER `ex` is listed: the ledger acts for it there */
inline bool
isExporter(SLE const& ex)
{
    return ex.isFieldPresent(sfExportNode);
}

/** The ltEXPORTERs of `account`, one per network it holds a record on */
std::vector<std::shared_ptr<SLE const>>
exportersOf(ReadView const& view, AccountID const& account);

/** The ltEXPORTER of `account` on network `nid`, made if absent (in its
    owner directory, counted against its reserve). `acc` is updated by the
    caller; the result is inserted or updated by the caller too. */
std::shared_ptr<SLE>
exporterOf(
    ApplyView& view,
    std::shared_ptr<SLE> const& acc,
    std::uint32_t nid,
    beast::Journal j);

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
    export_keys.txt beside the config file), one hex secret per line, oldest
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

    /** Write the keys; false if the file could not be written or replaced */
    bool
    save() const;

    beast::Journal const j_;
    std::optional<PublicKey> master_;
    std::uint32_t const networkID_;
    std::optional<boost::filesystem::path> file_;
    mutable std::mutex mutex_;
    std::vector<Key> keys_;         // oldest first, back() is the nominee
    std::uint32_t unrecorded_ = 0;  // flag ledgers our nominee went unrecorded
};

/** [export_relay] sections: carry exports to other networks and exporters'
    activity there back. Stops and joins on destruction. Null in standalone
    mode; otherwise does nothing unless configured. */
struct ExportRelay
{
    virtual ~ExportRelay() = default;
};

std::unique_ptr<ExportRelay>
makeExportRelay(Application& app);

}  // namespace ripple

#endif
