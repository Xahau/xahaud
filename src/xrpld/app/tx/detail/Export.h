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

class Config;
class ExportTickets;
class ValidatorKeys;

/** Export: the UNL multisigns transactions for other XRPL-protocol networks
    on behalf of a hook's account, which has the same AccountID there.

    Networks. An export's destination is the NetworkID of the transaction the
    hook supplies (absent: 0, the XRPL; or, where exactly one import VL key
    is trusted, that key's network: Import::soleNetwork). The ledger records
    it (sfRemoteNetworkID) and strips the field for ids of 1024 and below,
    which refuse it. All state is per network: the ltEXPORTER, shadow tickets
    and ltEXPORT_FEE. What comes back names its network by the validator list
    that proves it (Import::vlNetwork).

    Export keys. Each UNLReport validator nominates an ed25519 export key,
    with a proof of possession, in its flag ledger validations. The UNL
    reports it (ttUNL_REPORT ExportKeyReport) and it is recorded, newest
    first, in sfExportKeys on the validator's account: at most maxExportKeys,
    maxKeysPerEpoch per keyRotationPeriod. A new key never replaces an old
    one, so old keys keep signing. Each key recorded bumps
    UNLReport.ExportKeysSeq. A validator without an account is given one,
    with validatorFundingDrops, by the next ttGENESIS_MINT.
    A standalone node has no flag ledger worth waiting for: it adds the
    two reports for its own key to its initial position (RCLConsensus onClose)
    as soon as its account exists, so a few ledgers after funding it, the
    node is its own export signer.

    Exporters. An account's standing on one network is its ltEXPORTER. It is
    listed (keylet::exporterDir(), isExporter) once an Import it signs
    records a signer list there that is one of ours (listsExportKey); from a
    network other than the XRPL that Import records the list and nothing
    else. A list that is not ours, recorded later or found when an export
    closes below quorum, unlists it. asfExporter unlists it everywhere, or
    lists it again wherever its list is ours. For a listed exporter the
    ledger keeps these in step, so a hook tracks none of them:
      - sfExportSignerList: as last proven by an Import of its SignerListSet.
      - sfExportTickets: its unused tickets there (ExportTickets). Below
        ticketLow the ledger exports a TicketCreate for ticketBatch more
        (ttEXPORT_AUTO, paid by the exporter; sfExportTicketPending holds
        its ticket until it returns). The pool's last ticket is the
        ledger's (hookMayUse).
      - sfExportSequence: its Sequence there. With an empty pool the ledger
        exports a TicketCreate sequenced with it instead, once per Sequence
        it knows; the Import of the result, failed or not, records the next.
      - sfExportKeysSeq: the ExportKeysSeq its list was built for. When that
        is behind, the ledger exports a SignerListSet for the current keys,
        signed by the old list, at most once per keyRefreshEvery ledgers and
        only if the list would change.
      - sfBalance: its XRP there, which relays hold an unfunded export back
        on; and sfIndexes: the shadow tickets of its exports in flight.
    Relays ([export_relay]) Import each transaction there that changes any
    of these, free of charge (isLoopback). Such an Import only records: it
    installs no keys, mints nothing, uses no sequence and runs the account's
    hooks only as a collect call.

    An export's life:
    1. A hook calls xport() with a transaction ExportSchema allows
       (NOT_EXPORTABLE otherwise) that is complete (INVALID_ARGUMENT
       otherwise): as with emit(), nothing is filled in for it, so what
       the hook hands over is what the UNL signs. xport_prepare() is how
       a hook completes one (see prepare): it fills in Account,
       SigningPubKey (empty), Sequence (0), OperationLimit, the
       destination where there is only one, Fee (feeUnits x feeUnit: what
       ExportSchema says the transaction pays there, at the unit
       ltEXPORT_FEE holds; see Ingest.h, and xport_fee() for a hook to
       read that record itself) and LastLedgerSequence (`expiry` past the
       ledger last proven there, if any), each only if the hook left it
       out, and gives the result back for the hook to look at, change and
       pass to xport(). A Fee below feeUnits at the reference fee could
       never be included there and is refused when the ttEXPORT applies
       (tecINSUFF_FEE), which is also what catches a signer list that
       grew between emission and then.
       One field is still assigned as the ttEXPORT applies: the
       TicketSequence, from the pool, unless the hook names one. A ticket
       is not a value to work out but something to allot, and that cannot
       be done at emission: the ttEXPORT applies a ledger later, and until
       then every other execution of the hook sees the same pool and
       would be given the same ticket, the later export taking over the
       earlier one's. A ticket the hook names must be known to the ledger
       (ticketUsable). Applying the ttEXPORT creates an ltEXPORTED_TXN,
       with a copy of the signer list, and an ltSHADOW_TICKET. A ticket is
       never both pooled and shadowed.
    2. Each validator holding a listed key applies a ttEXPORT_SIGN to its
       open ledger. Proposed by one validator, it loses its first round and
       lands a ledger later (not being a pseudo-txn, it is retried). It is
       never relayed: it can only be checked against the ledger. Signers
       are kept sorted, so the object always holds a submittable txn.
    3. `window` ledgers on, ttEXPORT_FINAL deletes the object; its
       DeletedNode is the result. At quorum the shadow ticket takes the
       signed transaction, for relays to send. Below quorum the ticket
       returns to the pool and the shadow ticket goes.
    4. The XPOP of whatever used the ticket there is imported by anyone,
       which consumes the shadow ticket.
    5. If nothing has come back once a ledger `expiryGrace` past the
       export's LastLedgerSequence is proven there (ttINGEST), an expiry
       ttEXPORT_FINAL erases the shadow ticket and returns the ticket. (The
       ledger's own exports carry no LastLedgerSequence: they are resent.)

    Callbacks: cbak, once per export, with otxn_id() the id of the ttEXPORT
    (sfExportTxnID; otxn_id(1) is the transaction that brought the news).
    what == 2: the export itself came back (xpop_slot() has it and its
    meta). what == 1: it failed to apply, closed below quorum, expired, or
    something else used its ticket. What came back is told from anything
    else by its id without signatures (sfTransactionHash), since every
    quorum of signatures gives a different signed id. Two limits: after a
    close below quorum the signatures are still public, so until the
    LastLedgerSequence passes somebody else could submit it; and an expiry
    cannot rule out a transaction that landed in time but whose XPOP no
    relay imported within the grace (it can be imported later, without a
    second callback).

    The account there is not only the ledger's: its master and regular keys
    are untouched, and whoever holds them can transact for it, spend its
    tickets or replace the list. The ledger learns of that only by Import.
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

    /** The other network's reference fee: the least feeUnit is, what it is
        until ttINGEST has proven what transactions there actually pay
        (ltEXPORT_FEE, see Ingest.h), and the unit a hook's own Fee is held
        to (preclaim): at least feeUnits of these, above that as it likes. */
    static constexpr XRPAmount xrplBaseFee{10};

    /** xport_prepare() gives an export that sets no LastLedgerSequence one
        this many ledgers past the ledger last proven for its network
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

    /** The quorum of a list of `signers` export keys of weight 1: 80% */
    static constexpr std::uint32_t
    quorumOf(std::size_t signers)
    {
        return static_cast<std::uint32_t>((signers * 4 + 4) / 5);
    }

    /** What one reference fee is taken to cost on network `nid`, in drops:
        the unit its ltEXPORT_FEE holds (the median of what transactions
        there pay per signature, smoothed; see Ingest.h), or xrplBaseFee
        until a ledger has been proven for it. */
    static XRPAmount
    feeUnit(ReadView const& view, std::uint32_t nid);

    /** What `inner`, exported by `account` to network `nid`, must pay there
        in reference fees: one for itself, one for every signer on the
        account's list there (all of them may sign, and each signature is
        charged for) and what ExportSchema says its type pays on top.
        Nothing if the account has no signer list there. */
    static std::optional<std::int64_t>
    feeUnits(
        ReadView const& view,
        AccountID const& account,
        std::uint32_t nid,
        STObject const& inner);

    /** What xport_prepare() does: complete `inner`, a transaction a hook of
        `account` means to export, leaving alone every field it already
        carries. Filled in if absent:
          - NetworkID, with the one network this one knows
            (Import::soleNetwork), if there is exactly one. Otherwise none
            stays none, which is the XRPL;
          - Account, SigningPubKey (empty), Sequence (0) and OperationLimit
            (this network);
          - LastLedgerSequence, `expiry` past the ledger last proven for the
            destination, if one has been (none otherwise: such an export
            cannot expire);
          - Fee, feeUnits x feeUnit.
        Not TicketSequence: see the class comment. False, with `inner`
        possibly part filled, if the account has no signer list on the
        destination: there is nothing to price the export by, and it could
        not apply. */
    static bool
    prepare(
        ReadView const& view,
        Config const& config,
        AccountID const& account,
        STObject& inner);

    /** Whether `inner` is complete as a hook's export from `account` on
        network `here`, the schema aside: from the account, unsigned with an
        empty SigningPubKey, Sequence 0, bound back here by OperationLimit,
        for another network, and paying a fee in drops. What ttEXPORT's
        preflight requires, for xport() to tell the hook first. */
    static bool
    complete(STTx const& inner, AccountID const& account, std::uint32_t here);

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

/** Whether the signer list the ltEXPORTER `ex` holds is one of ours */
bool
holdsOurList(ReadView const& view, SLE const& ex);

/** f(key) for each ltEXPORTER listed in keylet::exporterDir() */
void
forEachExporter(ReadView const& view, auto&& f)
{
    for (auto page = view.read(keylet::exporterDir()); page;)
    {
        for (auto const& key : page->getFieldV256(sfIndexes))
            f(key);
        auto const next = (*page)[~sfIndexNext].value_or(0);
        page = next ? view.read(keylet::page(keylet::exporterDir(), next))
                    : nullptr;
    }
}

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
