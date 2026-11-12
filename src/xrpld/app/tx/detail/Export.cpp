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

#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/main/Application.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/app/tx/detail/Import.h>
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/ExportSchema.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/st.h>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <unistd.h>

namespace ripple {

static STObject const&
obj(STObject const& o, SField const& f)
{
    return o.peekAtField(f).downcast<STObject>();
}

// Whether `t` is shaped for multisigning by the UNL: from `account`,
// unsigned, not emitted, not a pseudo-txn, not replayable here, and bound
// back here. Every export uses a ticket (one without TicketSequence gets one
// when it applies), so Sequence is 0; except the backstop TicketCreate of an
// exporter whose pool is empty, which is `sequenced` (Sequence set, no
// ticket) instead. Its NetworkID is the destination (Export::destination),
// never this network.
static bool
shaped(
    STTx const& t,
    AccountID const& account,
    std::uint32_t nid,
    bool sequenced)
{
    bool const seqOk = sequenced
        ? (t[sfSequence] != 0 && !t.isFieldPresent(sfTicketSequence))
        : t[sfSequence] == 0;
    return t[sfAccount] == account && t.getSigningPubKey().empty() &&
        !t.isFieldPresent(sfTxnSignature) && !t.isFieldPresent(sfSigners) &&
        !t.isFieldPresent(sfEmitDetails) && !isPseudoTx(t) &&
        Export::destination(t) != nid && seqOk && t[~sfOperationLimit] == nid;
}

// `inner` as a transaction, if it parses under this network's formats, is
// one both networks read the same way (ExportSchema, under `rules`) and is
// `shaped`.
static std::optional<STTx>
exportedTx(
    STObject const& inner,
    AccountID const& account,
    std::uint32_t nid,
    Rules const& rules,
    bool sequenced = false)
{
    try
    {
        Serializer s;
        inner.add(s);
        STTx const t{SerialIter{s.slice()}};
        if (shaped(t, account, nid, sequenced) &&
            ExportSchema::allowed(t, rules))
            return t;
    }
    catch (std::exception const&)
    {
    }
    return std::nullopt;
}

// Whether `inner` carries a fee the other network could take: in drops, and
// not none. Nothing is filled in for a hook's export, so a Fee of 0 is no
// longer a request for one (it still is on the ledger's own exports, which
// are priced as they apply).
static bool
paysFee(STObject const& inner)
{
    if (!inner.isFieldPresent(sfFee))
        return false;
    auto const& fee = inner.getFieldAmount(sfFee);
    return fee.native() && fee.signum() > 0;
}

bool
Export::complete(
    STTx const& inner,
    AccountID const& account,
    std::uint32_t here)
{
    return shaped(inner, account, here, false) && paysFee(inner);
}

static Keylet
exportKeylet(STTx const& tx)
{
    return keylet::exportedTxn(tx[sfLedgerSequence], tx[sfTransactionHash]);
}

// whether `inner` is sequenced there rather than ticketed: only the backstop
// TicketCreate of an exporter whose pool is empty (see Export.h)
static bool
sequenced(STObject const& inner)
{
    return inner[~sfSequence].value_or(0) != 0;
}

// the Sequence or ticket `inner` uses there: what sfExportTicketPending
// holds for an automatic TicketCreate until it returns (the two share one
// number space there, so one field tells them apart by value alone)
static std::uint32_t
seqProxyOf(STObject const& inner)
{
    if (auto const t = inner[~sfTicketSequence])
        return *t;
    return inner[~sfSequence].value_or(0);
}

static bool
listed(STArray const& entries, AccountID const& acc)
{
    return std::any_of(entries.begin(), entries.end(), [&](STObject const& e) {
        return e[sfAccount] == acc;
    });
}

// Each export may return once via Import, which consumes this and calls back
// `hook`. Exporting again with the same ticket repoints it: only one
// transaction can use the ticket on the other network.
static TER
setShadowTicket(
    ApplyView& view,
    AccountID const& acc,
    std::uint32_t nid,
    std::uint32_t ticket,
    uint256 const& txid,
    std::optional<uint256> const& hook,
    std::optional<std::uint32_t> const& lastLedger,
    uint256 const& exportID,
    beast::Journal j)
{
    auto const k = keylet::shadowTicket(acc, nid, ticket);
    if (auto const st = view.peek(k))
    {
        st->setFieldH256(sfTransactionHash, txid);
        (*st)[~sfEmitHookHash] = hook;
        (*st)[~sfLastLedgerSequence] = lastLedger;
        st->setFieldH256(sfExportTxnID, exportID);
        // the export that held the ticket is taken over: what was signed
        // for it is no longer what the ledger waits for
        if (st->isFieldPresent(sfRemoteTxnID))
            st->makeFieldAbsent(sfRemoteTxnID);
        if (st->isFieldPresent(sfExportedTxn))
            st->makeFieldAbsent(sfExportedTxn);
        view.update(st);
        return tesSUCCESS;
    }

    auto const sle = view.peek(keylet::account(acc));
    if (!sle)
        return tefINTERNAL;
    if (sle->getFieldAmount(sfBalance).xrp() <
        view.fees().accountReserve((*sle)[sfOwnerCount] + 1))
        return tecINSUFFICIENT_RESERVE;

    auto const page =
        view.dirInsert(keylet::ownerDir(acc), k, describeOwnerDir(acc));
    if (!page)
        return tecDIR_FULL;

    auto const st = std::make_shared<SLE>(k);
    st->setAccountID(sfAccount, acc);
    st->setFieldU32(sfRemoteNetworkID, nid);
    st->setFieldU32(sfTicketSequence, ticket);
    st->setFieldH256(sfTransactionHash, txid);
    (*st)[~sfEmitHookHash] = hook;
    (*st)[~sfLastLedgerSequence] = lastLedger;
    st->setFieldH256(sfExportTxnID, exportID);
    st->setFieldU64(sfOwnerNode, *page);
    view.insert(st);
    adjustOwnerCount(view, sle, 1, j);
    return tesSUCCESS;
}

// the shadow ticket for `ticket`, if it still names export `txid` (another
// export may have taken the ticket over since)
static bool
eraseShadowTicket(
    ApplyView& view,
    AccountID const& acc,
    std::uint32_t nid,
    std::uint32_t ticket,
    uint256 const& txid)
{
    auto const k = keylet::shadowTicket(acc, nid, ticket);
    auto const st = view.peek(k);
    if (!st || (*st)[sfTransactionHash] != txid)
        return false;
    auto const sle = view.peek(keylet::account(acc));
    if (!sle ||
        !view.dirRemove(keylet::ownerDir(acc), (*st)[sfOwnerNode], k, false))
        return false;
    view.erase(st);
    adjustOwnerCount(
        view, sle, -1, beast::Journal{beast::Journal::getNullSink()});
    view.update(sle);
    return true;
}

// The shadow tickets of an ltEXPORTER whose exports carry a
// LastLedgerSequence are listed on it (sfIndexes), so Export::accept can
// find the ones to expire without walking the owner directory. False if
// the list is full.
static bool
trackExpiry(SLE& ex, uint256 const& key)
{
    auto keys = ex.isFieldPresent(sfIndexes) ? ex.getFieldV256(sfIndexes)
                                             : STVector256(sfIndexes);
    if (std::find(keys.begin(), keys.end(), key) != keys.end())
        return true;
    if (keys.size() >= Export::maxExpiring)
        return false;
    keys.push_back(key);
    ex.setFieldV256(sfIndexes, keys);
    return true;
}

bool
untrackExpiry(SLE& ex, uint256 const& key)
{
    if (!ex.isFieldPresent(sfIndexes))
        return false;
    auto keys = ex.getFieldV256(sfIndexes);
    auto const it = std::find(keys.begin(), keys.end(), key);
    if (it == keys.end())
        return false;
    keys.erase(it);
    if (keys.empty())
        ex.makeFieldAbsent(sfIndexes);
    else
        ex.setFieldV256(sfIndexes, keys);
    return true;
}

// Whether the export behind shadow ticket `st` has failed for good: it has a
// LastLedgerSequence, the ledger proven for its network (ltEXPORT_FEE, which
// ttINGEST keeps moving) is Export::expiryGrace past it, and its result was
// not imported in between or `st` would be gone. Its own signing window here
// must have closed first, so that no export is ever closed twice (a hook can
// set a LastLedgerSequence already in the past).
static bool
expired(ReadView const& view, SLE const& st)
{
    auto const last = st[~sfLastLedgerSequence];
    if (!last || view.seq() <= st[sfPreviousTxnLgrSeq] + 2 * Export::window)
        return false;
    auto const record = view.read(keylet::exportFee(st[sfRemoteNetworkID]));
    return record &&
        std::uint64_t{(*record)[sfLedgerSequence]} >=
        std::uint64_t{*last} + Export::expiryGrace;
}

// The export `txid` on `ticket` will not run: its shadow ticket goes and the
// ticket returns to the pool of `ex` (if it has a record), unless it is above
// the watermark of recorded TicketCreates, a ticket the ledger never saw
// made. False if the shadow ticket does not name that export (any more).
static bool
releaseTicket(
    ApplyView& view,
    std::shared_ptr<SLE> const& ex,
    AccountID const& owner,
    std::uint32_t dest,
    std::uint32_t ticket,
    uint256 const& txid)
{
    if (!eraseShadowTicket(view, owner, dest, ticket, txid))
        return false;
    if (ex)
    {
        untrackExpiry(*ex, keylet::shadowTicket(owner, dest, ticket).key);
        if (ticket < (*ex)[~sfExportTicketNext].value_or(
                         std::numeric_limits<std::uint32_t>::max()))
        {
            auto tickets = ExportTickets::load(*ex);
            tickets.insert(ticket);
            tickets.store(*ex);
        }
    }
    return true;
}

//------------------------------------------------------------------------------

ExportTickets::ExportTickets(STVector256 const& v)
{
    for (auto const& h : v.value())
        for (int i = 0; i < 6; ++i)
        {
            auto const* p = h.data() + 5 * i;
            std::uint32_t const first = (std::uint32_t(p[0]) << 24) |
                (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
                std::uint32_t(p[3]);
            if (p[4] == 0)
                continue;
            ranges_.push_back({first, p[4]});
        }
    normalize();
}

void
ExportTickets::normalize()
{
    std::ranges::sort(ranges_, {}, &Range::first);
    std::vector<Range> out;
    for (auto const& r : ranges_)
    {
        if (r.count == 0)
            continue;
        // overlap or adjacency with the last range: extend it
        if (!out.empty() && r.first <= out.back().first + out.back().count)
        {
            std::uint64_t const end = std::max<std::uint64_t>(
                std::uint64_t(out.back().first) + out.back().count,
                std::uint64_t(r.first) + r.count);
            out.back().count =
                static_cast<std::uint32_t>(end - out.back().first);
            continue;
        }
        out.push_back(r);
    }
    // a run longer than a byte is represented as abutting ranges
    ranges_.clear();
    for (auto r : out)
        while (r.count > 0)
        {
            std::uint32_t const n = std::min<std::uint32_t>(r.count, 255);
            ranges_.push_back({r.first, n});
            r.first += n;
            r.count -= n;
        }
}

void
ExportTickets::add(std::uint32_t first, std::uint32_t count)
{
    if (count == 0 || std::uint64_t(first) + count > 0xFFFFFFFFULL)
        return;
    ranges_.push_back({first, count});
    normalize();
}

bool
ExportTickets::contains(std::uint32_t ticket) const
{
    return std::any_of(ranges_.begin(), ranges_.end(), [&](auto const& r) {
        return ticket >= r.first && ticket < r.first + r.count;
    });
}

bool
ExportTickets::remove(std::uint32_t ticket)
{
    for (auto it = ranges_.begin(); it != ranges_.end(); ++it)
    {
        if (ticket < it->first || ticket >= it->first + it->count)
            continue;
        Range const r = *it;
        ranges_.erase(it);
        if (ticket > r.first)
            ranges_.push_back({r.first, ticket - r.first});
        if (ticket + 1 < r.first + r.count)
            ranges_.push_back({ticket + 1, r.first + r.count - ticket - 1});
        normalize();
        return true;
    }
    return false;
}

std::optional<std::uint32_t>
ExportTickets::take()
{
    if (ranges_.empty())
        return std::nullopt;
    auto const it = std::ranges::min_element(ranges_, {}, [](Range const& r) {
        return std::pair{r.count, r.first};
    });
    std::uint32_t const t = it->first;
    remove(t);
    return t;
}

std::uint32_t
ExportTickets::count() const
{
    std::uint32_t n = 0;
    for (auto const& r : ranges_)
        n += r.count;
    return n;
}

STVector256
ExportTickets::pack() const
{
    STVector256 v(sfExportTickets);
    for (std::size_t i = 0; i < ranges_.size(); i += 6)
    {
        uint256 h;
        for (std::size_t k = 0; k < 6 && i + k < ranges_.size(); ++k)
        {
            auto const& r = ranges_[i + k];
            auto* p = h.data() + 5 * k;
            p[0] = static_cast<std::uint8_t>(r.first >> 24);
            p[1] = static_cast<std::uint8_t>(r.first >> 16);
            p[2] = static_cast<std::uint8_t>(r.first >> 8);
            p[3] = static_cast<std::uint8_t>(r.first);
            p[4] = static_cast<std::uint8_t>(r.count);
        }
        v.push_back(h);
    }
    return v;
}

void
ExportTickets::store(SLE& exporter) const
{
    if (ranges_.empty())
        exporter.makeFieldAbsent(sfExportTickets);
    else
        exporter.setFieldV256(sfExportTickets, pack());
}

ExportTickets
ExportTickets::load(SLE const& exporter)
{
    if (!exporter.isFieldPresent(sfExportTickets))
        return {};
    return ExportTickets(exporter.getFieldV256(sfExportTickets));
}

bool
Export::hookMayUse(
    ExportTickets const& pool,
    std::optional<std::uint32_t> ticket)
{
    // a ticket of the hook's own, outside the pool, is not the ledger's to
    // keep; one from the pool, named or drawn, must not be the last
    if (ticket && !pool.contains(*ticket))
        return true;
    return pool.count() >= 2;
}

bool
Export::ticketUsable(
    ReadView const& view,
    AccountID const& account,
    std::uint32_t nid,
    std::uint32_t ticket)
{
    if (view.exists(keylet::shadowTicket(account, nid, ticket)))
        return true;
    auto const ex = view.read(keylet::exporter(account, nid));
    if (!ex)
        return false;
    auto const pool = ExportTickets::load(*ex);
    return pool.contains(ticket) && hookMayUse(pool, ticket);
}

uint256
exportUnsignedID(STObject const& tx)
{
    try
    {
        // as Export::createExport computed it: the transaction parsed
        // under this network's formats, without what signing adds
        STObject o{tx};
        if (o.isFieldPresent(sfSigners))
            o.makeFieldAbsent(sfSigners);
        if (o.isFieldPresent(sfTxnSignature))
            o.makeFieldAbsent(sfTxnSignature);
        Serializer s;
        o.add(s);
        return STTx{SerialIter{s.slice()}}.getTransactionID();
    }
    catch (std::exception const&)
    {
        return uint256{};
    }
}

TER
setExporter(ApplyView& view, std::shared_ptr<SLE> const& ex, bool on)
{
    bool const was = isExporter(*ex);
    if (on && !was)
    {
        auto const page = view.dirInsert(
            keylet::exporterDir(), ex->key(), [](std::shared_ptr<SLE> const&) {
            });
        if (!page)
            return tecDIR_FULL;
        ex->setFieldU64(sfExportNode, *page);
    }
    else if (!on && was)
    {
        if (!view.dirRemove(
                keylet::exporterDir(), (*ex)[sfExportNode], ex->key(), false))
            return tefBAD_LEDGER;
        ex->makeFieldAbsent(sfExportNode);
    }
    return tesSUCCESS;
}

std::vector<std::shared_ptr<SLE const>>
exportersOf(ReadView const& view, AccountID const& account)
{
    std::vector<std::shared_ptr<SLE const>> out;
    forEachItem(view, account, [&](std::shared_ptr<SLE const> const& sle) {
        if (sle && sle->getType() == ltEXPORTER)
            out.push_back(sle);
    });
    return out;
}

std::shared_ptr<SLE>
exporterOf(
    ApplyView& view,
    std::shared_ptr<SLE> const& acc,
    std::uint32_t nid,
    beast::Journal j)
{
    auto const id = acc->getAccountID(sfAccount);
    auto const k = keylet::exporter(id, nid);
    if (auto const ex = view.peek(k))
        return ex;
    auto const page =
        view.dirInsert(keylet::ownerDir(id), k, describeOwnerDir(id));
    if (!page)
        return nullptr;
    auto const ex = std::make_shared<SLE>(k);
    ex->setAccountID(sfAccount, id);
    ex->setFieldU32(sfRemoteNetworkID, nid);
    ex->setFieldU64(sfOwnerNode, *page);
    // not adjustOwnerCount: `acc` may be an account this very Import is
    // creating, which is not in the view yet for it to update
    auto const count = (*acc)[sfOwnerCount];
    view.adjustOwnerCountHook(id, count, count + 1);
    acc->setFieldU32(sfOwnerCount, count + 1);
    return ex;
}

//------------------------------------------------------------------------------

Serializer
exportKeyProofData(
    PublicKey const& master,
    PublicKey const& exportKey,
    std::uint32_t networkID)
{
    Serializer s;
    s.add32(HashPrefix::exportKeyProof);
    s.addVL(master.slice());
    s.addVL(exportKey.slice());
    s.add32(networkID);
    return s;
}

// f(master, entry) for each export key of each UNLReport validator, newest
// first, until it returns true
static void
forEachExportKey(ReadView const& view, auto&& f)
{
    auto const unl = view.read(keylet::UNLReport());
    if (!unl || !unl->isFieldPresent(sfActiveValidators))
        return;
    for (auto const& v : unl->getFieldArray(sfActiveValidators))
    {
        PublicKey const master(v[sfPublicKey]);
        auto const acc = view.read(keylet::account(calcAccountID(master)));
        if (acc && acc->isFieldPresent(sfExportKeys))
            for (auto const& e : acc->getFieldArray(sfExportKeys))
                if (f(master, e))
                    break;
    }
}

std::vector<ExportSigner>
exportSigners(ReadView const& view, std::optional<std::uint32_t> asOf)
{
    std::vector<ExportSigner> out;
    forEachExportKey(view, [&](PublicKey const& master, STObject const& e) {
        auto const t = e.getFieldU32(sfCloseTime);
        if (asOf && t > *asOf)
            return false;
        // the newest at or before asOf; the same key under two validators
        // would list one account twice
        PublicKey const pk(makeSlice(e.getFieldVL(sfExportKey)));
        if (std::ranges::find(out, pk, &ExportSigner::key) == out.end())
            out.push_back({master, pk, t, calcAccountID(pk)});
        return true;
    });

    std::ranges::sort(out, {}, &ExportSigner::account);
    if (auto const max = STTx::maxMultiSigners(&view.rules()); out.size() > max)
        out.erase(out.begin() + max, out.end());
    return out;
}

bool
listsExportKey(ReadView const& view, STArray const& entries)
{
    bool found = false;
    forEachExportKey(view, [&](PublicKey const&, STObject const& e) {
        // once found, stay found: the callback is called again for every
        // later validator, and returning false here would clear it
        if (found)
            return true;
        PublicKey const pk(makeSlice(e.getFieldVL(sfExportKey)));
        found = listed(entries, calcAccountID(pk));
        return found;
    });
    return found;
}

bool
exportKeysSettled(ReadView const& view)
{
    // under featureExport every validator report stamps the ledger it was
    // applied in (Change::applyUNLReport)
    auto const unl = view.read(keylet::UNLReport());
    return !unl || (*unl)[~sfLedgerSequence] != view.seq();
}

bool
holdsOurList(ReadView const& view, SLE const& ex)
{
    return ex.isFieldPresent(sfExportSignerList) &&
        listsExportKey(
               view,
               obj(ex, sfExportSignerList).getFieldArray(sfSignerEntries));
}

std::uint32_t
exportWeight(STObject const& exported)
{
    std::uint32_t weight = 0;
    auto const& inner = obj(exported, sfExportedTxn);
    if (inner.isFieldPresent(sfSigners))
        for (auto const& s : inner.getFieldArray(sfSigners))
            for (auto const& e : exported.getFieldArray(sfSignerEntries))
                if (e[sfAccount] == s[sfAccount])
                    weight += e[sfSignerWeight];
    return weight;
}

// the signer list `acc` holds on network `nid` (a copy: the SLE read here
// does not outlive this function)
static std::optional<STObject>
signerList(ReadView const& view, AccountID const& acc, std::uint32_t nid)
{
    auto const sle = view.read(keylet::exporter(acc, nid));
    if (sle && sle->isFieldPresent(sfExportSignerList))
    {
        auto const& list = obj(*sle, sfExportSignerList);
        // an empty one records that the list there was deleted (and where),
        // so no later import can bring an older list back
        if (!list.getFieldArray(sfSignerEntries).empty())
            return list;
    }
    return std::nullopt;
}

// what an export by `acc` to `nid` costs: one signature per listed signer
// plus a final
static XRPAmount
fanOutFee(ReadView const& view, AccountID const& acc, std::uint32_t nid)
{
    auto const list = signerList(view, acc, nid);
    std::int64_t const n =
        list ? list->getFieldArray(sfSignerEntries).size() : 0;
    return view.fees().base * (n + 1);
}

XRPAmount
Export::autoFee(ReadView const& view, AccountID const& owner, std::uint32_t nid)
{
    return fanOutFee(view, owner, nid) + view.fees().base;
}

XRPAmount
Export::feeUnit(ReadView const& view, std::uint32_t nid)
{
    auto const record = view.read(keylet::exportFee(nid));
    return XRPAmount{
        record ? std::int64_t{(*record)[sfExportFeeUnit]}
               : xrplBaseFee.drops()};
}

// One reference fee for the transaction and one per signer listed, plus what
// the other network charges this type on top, which only ExportSchema knows
// (an EscrowFinish carrying a Fulfillment, say): without that the export
// would be signed, sent and never clear the other network's fee check.
std::optional<std::int64_t>
Export::feeUnits(
    ReadView const& view,
    AccountID const& account,
    std::uint32_t nid,
    STObject const& inner)
{
    auto const list = signerList(view, account, nid);
    if (!list)
        return std::nullopt;
    return static_cast<std::int64_t>(ExportSchema::feeUnits(
        inner, list->getFieldArray(sfSignerEntries).size(), view.rules()));
}

bool
Export::prepare(
    ReadView const& view,
    Config const& config,
    AccountID const& account,
    STObject& inner)
{
    // The destination: the NetworkID the hook gave, none meaning the XRPL
    // (0), unless exactly one other network is known here at all (one
    // trusted import VL key), in which case none means that one. It stays
    // on the transaction until the export applies, which records it
    // (RemoteNetworkID) and takes it off again for networks of 1024 and
    // below, where the field is not allowed.
    if (!inner.isFieldPresent(sfNetworkID))
        if (auto const sole = Import::soleNetwork(config, view); sole != 0)
            inner[sfNetworkID] = sole;
    auto const dest = destination(inner);

    if (!inner.isFieldPresent(sfAccount))
        inner[sfAccount] = account;
    if (!inner.isFieldPresent(sfSigningPubKey))
        inner[sfSigningPubKey] = Slice{};
    if (!inner.isFieldPresent(sfSequence))
        inner[sfSequence] = 0;
    if (!inner.isFieldPresent(sfOperationLimit))
        inner[sfOperationLimit] = config.NETWORK_ID;

    // The last ledger there that may include it, so that an export which
    // never lands can be told from one that is late, and expired (Export.h,
    // step 5). None until a ledger has been proven for the network.
    if (auto const record = view.read(keylet::exportFee(dest));
        record && !inner.isFieldPresent(sfLastLedgerSequence))
        inner[sfLastLedgerSequence] = (*record)[sfLedgerSequence] + expiry;

    // Priced last, by the transaction as it now stands, and whether or not
    // the hook set a Fee of its own: an account with no signer list there
    // has nothing to prepare an export for.
    auto const units = feeUnits(view, account, dest, inner);
    if (!units)
        return false;
    if (!inner.isFieldPresent(sfFee))
        inner.setFieldAmount(sfFee, feeUnit(view, dest) * *units);
    return true;
}

// the owner of an export: the hook account, or the exporter of an automatic one
static AccountID
ownerOf(STTx const& tx)
{
    return tx.getTxnType() == ttEXPORT_AUTO ? tx[sfOwner] : tx[sfAccount];
}

XRPAmount
Export::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    if (tx.getTxnType() != ttEXPORT)
        return XRPAmount{0};
    return Transactor::calculateBaseFee(view, tx) +
        fanOutFee(view, tx[sfAccount], destination(obj(tx, sfExportedTxn)));
}

NotTEC
Export::preflight(PreflightContext const& ctx)
{
    auto const& tx = ctx.tx;
    if (!ctx.rules.enabled(featureExport))
        return temDISABLED;

    auto const nid = ctx.app.config().NETWORK_ID;

    if (tx.getTxnType() == ttEXPORT)
    {
        if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
            return ret;
        if (tx.getFlags() & tfUniversalMask)
            return temINVALID_FLAG;
        if (!hook::isEmittedTxn(tx) ||
            !exportedTx(obj(tx, sfExportedTxn), tx[sfAccount], nid, ctx.rules))
            return temMALFORMED;
        // Nothing is filled in for a hook, here or when this applies (bar
        // the ticket): the Fee it carries is the Fee the UNL signs.
        if (!paysFee(obj(tx, sfExportedTxn)))
            return temBAD_FEE;
        return preflight2(ctx);
    }

    if (auto const ret = preflight0(ctx); !isTesSuccess(ret))
        return ret;

    // network generated: account zero, free, unsigned, unsequenced, no flags
    if (tx[sfAccount] != beast::zero || tx[sfFee] != beast::zero ||
        tx[sfSequence] != 0 || tx.getFlags() != 0 ||
        !tx.getSigningPubKey().empty() || tx.isFieldPresent(sfTxnSignature) ||
        tx.isFieldPresent(sfSigners) || tx.isFieldPresent(sfTicketSequence) ||
        tx.isFieldPresent(sfPreviousTxnID))
        return temMALFORMED;

    // These skip preflight1, which refuses sfTime on the other types pinned
    // to Sequence 0 (featureJsonTx): with it one would read as time-sequenced,
    // and every validator must produce the same ttEXPORT_SIGN txid.
    if (tx.isFieldPresent(sfTime) || tx.isFieldPresent(sfJsonTxDelta))
        return temMALFORMED;

    if (tx.getTxnType() == ttEXPORT_SIGN)
    {
        // Nothing but what Export::accept builds. It is free and its one
        // check is a signature anybody can copy out of a proposed set, so
        // any other field (Memos, SourceTag, LastLedgerSequence, an explicit
        // Flags of 0...) would be a way to put data into the ledger for
        // nothing, and to give one signature as many txids as wanted.
        for (auto const& f : tx)
        {
            if (f.getSType() == STI_NOTPRESENT)
                continue;
            auto const& n = f.getFName();
            if (n != sfTransactionType && n != sfAccount && n != sfSequence &&
                n != sfFee && n != sfSigningPubKey && n != sfNetworkID &&
                n != sfLedgerSequence && n != sfTransactionHash &&
                n != sfSigner)
                return temMALFORMED;
        }

        auto const& s = obj(tx, sfSigner);
        auto const pk = s[sfSigningPubKey];
        if (!publicKeyType(pk) || s[sfAccount] != calcAccountID(PublicKey(pk)))
            return temMALFORMED;
    }

    if (tx.getTxnType() == ttEXPORT_AUTO)
    {
        // only what the ledger exports for exporters, without a ticket; and
        // sequenced only the backstop TicketCreate (preclaim checks the
        // Sequence against what the ledger knows)
        auto const& inner = obj(tx, sfExportedTxn);
        auto const type = inner[~sfTransactionType].value_or(0);
        if ((type != ttTICKET_CREATE && type != ttSIGNER_LIST_SET) ||
            inner.isFieldPresent(sfTicketSequence) ||
            (sequenced(inner) && type != ttTICKET_CREATE) ||
            !exportedTx(inner, tx[sfOwner], nid, ctx.rules, sequenced(inner)))
            return temMALFORMED;
    }

    return tesSUCCESS;
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    auto const& tx = ctx.tx;
    if (tx.getTxnType() == ttEXPORT || tx.getTxnType() == ttEXPORT_AUTO)
    {
        auto const owner = ownerOf(tx);
        auto const& inner = obj(tx, sfExportedTxn);
        auto const nid = destination(inner);
        if (!signerList(ctx.view, owner, nid))
            return tecNO_TARGET;
        // a ticket the hook names must be one the ledger knows of there
        if (auto const t = inner[~sfTicketSequence]; t &&
            tx.getTxnType() == ttEXPORT &&
            !ticketUsable(ctx.view, owner, nid, *t))
            return tecNO_ENTRY;
        // A hook's Fee is its own to set, but not below what the other
        // network could ever take for this transaction under this list:
        // feeUnits reference fees. xport_prepare() prices by the same
        // count, at emission, a ledger before this; so this is also what
        // stops an export priced for a list that has grown since from
        // being signed and sent for nothing. Refused here, its hook hears
        // at once (cbak, what == 1) instead of at its expiry.
        if (tx.getTxnType() == ttEXPORT)
        {
            auto const units = feeUnits(ctx.view, owner, nid, inner);
            if (!units || !paysFee(inner) ||
                inner.getFieldAmount(sfFee).xrp() < xrplBaseFee * *units)
                return tecINSUFF_FEE;
        }
        if (tx.getTxnType() == ttEXPORT_AUTO)
        {
            auto const ex = ctx.view.read(keylet::exporter(owner, nid));
            auto const acc = ctx.view.read(keylet::account(owner));
            if (!ex || !acc || !isExporter(*ex))
                return tecNO_PERMISSION;
            if (acc->getFieldAmount(sfBalance).xrp() <
                autoFee(ctx.view, owner, nid))
                return tecINSUFFICIENT_FUNDS;
            // The backstop: only for an empty pool, only at the Sequence the
            // ledger knows the account to be at there, and only once per
            // such Sequence. Every node must inject the same one.
            if (sequenced(inner))
            {
                auto const seq = inner[sfSequence];
                if ((*ex)[~sfExportSequence] != seq)
                    return tefPAST_SEQ;
                if (!ExportTickets::load(*ex).empty() ||
                    (*ex)[~sfExportTicketPending] == seq)
                    return tefFAILURE;
            }
        }
        return tesSUCCESS;
    }

    // An expiry: the shadow ticket it names is that export's still, and the
    // ledger proven for its network has left its LastLedgerSequence behind.
    // Every node injects the same one from the same ledger state.
    if (tx.getTxnType() == ttEXPORT_FINAL && tx.isFieldPresent(sfLedgerIndex))
    {
        auto const st =
            ctx.view.read(Keylet{ltSHADOW_TICKET, tx[sfLedgerIndex]});
        if (!st || (*st)[sfAccount] != tx[sfOwner] ||
            (*st)[sfTransactionHash] != tx[sfTransactionHash] ||
            (*st)[~sfLastLedgerSequence] != tx[sfLedgerSequence] ||
            !expired(ctx.view, *st))
            return tefFAILURE;
        return tesSUCCESS;
    }

    auto const sle = ctx.view.read(exportKeylet(tx));
    if (!sle)
        return tefFAILURE;

    if (tx.getTxnType() == ttEXPORT_FINAL)
        return tx[sfOwner] == (*sle)[sfOwner] ? TER{tesSUCCESS}
                                              : TER{tefFAILURE};

    // Only listed accounts may sign: one foreign signer and the other network
    // rejects the whole transaction. Nothing else about the key matters here.
    // Signatures authenticate themselves, sign txns only reach a ledger
    // through trusted proposals, and the list was proven by Import.
    auto const& s = obj(tx, sfSigner);
    PublicKey const pk(s[sfSigningPubKey]);
    if (!listed(sle->getFieldArray(sfSignerEntries), s[sfAccount]))
        return tefBAD_AUTH;

    // Signers are unique and listed, so never more than SignerEntries holds.
    auto const& inner = obj(*sle, sfExportedTxn);
    if (inner.isFieldPresent(sfSigners) &&
        listed(inner.getFieldArray(sfSigners), s[sfAccount]))
        return tefALREADY;

    // XRPL requires fully canonical signatures, which also stops malleated
    // copies of one signature from becoming distinct transactions.
    if (!verify(
            pk,
            buildMultiSigningData(inner, s[sfAccount]).slice(),
            s[sfTxnSignature],
            true))
        return tefBAD_SIGNATURE;

    return tesSUCCESS;
}

TER
Export::createExport(
    AccountID const& owner,
    STObject inner,
    std::optional<uint256> const& hook,
    bool automatic)
{
    auto& view = ctx_.view();
    auto const nid = ctx_.app.config().NETWORK_ID;
    auto const dest = destination(inner);

    auto const acc = view.peek(keylet::account(owner));
    auto const ex = view.peek(keylet::exporter(owner, dest));
    auto const list = signerList(view, owner, dest);
    if (!acc || !ex || !list)
        return tefINTERNAL;

    auto const type = inner.getFieldU16(sfTransactionType);

    // a SignerListSet without entries moves the account onto the current
    // export keys: its old list signs, so keys rotate without lockout
    if (type == ttSIGNER_LIST_SET && !inner.isFieldPresent(sfSignerEntries))
    {
        // Never from a UNLReport that is part-way through being rebuilt:
        // the list would name only the validators reported so far in this
        // ledger, and hand the account there to that handful. The ledger
        // tries again at the exporter's next slot.
        if (!exportKeysSettled(view))
            return tecNO_TARGET;
        auto const signers = exportSigners(view);
        if (signers.empty())
            return tecNO_TARGET;
        STArray entries(sfSignerEntries);
        for (auto const& s : signers)
        {
            entries.push_back(STObject::makeInnerObject(sfSignerEntry));
            entries.back()[sfAccount] = s.account;
            entries.back()[sfSignerWeight] = 1;
        }
        inner.setFieldArray(sfSignerEntries, entries);
        inner[sfSignerQuorum] = quorumOf(signers.size());
    }

    // The ticket: the hook's, or the next from the pool. A hook may not
    // spend the pool's last ticket, which carries the ledger's refill
    // (hookMayUse; xport() tells the hook so before it emits). The backstop
    // TicketCreate of an empty pool is sequenced instead and needs none.
    auto tickets = ExportTickets::load(*ex);
    bool const backstop = sequenced(inner);
    std::optional<std::uint32_t> ticket;
    if (backstop)
    {
        if (!automatic || type != ttTICKET_CREATE)
            return tefINTERNAL;
    }
    else if (auto const t = inner[~sfTicketSequence])
    {
        // the hook's own ticket, pooled or not (its account is trusted on
        // which tickets it holds; a close below quorum re-pools it only if
        // the ledger has seen it created, see ttEXPORT_FINAL)
        if (!automatic && !hookMayUse(tickets, t))
            return tecNO_ENTRY;
        ticket = *t;
        tickets.remove(*ticket);
    }
    else
    {
        if (!automatic && !hookMayUse(tickets, std::nullopt))
            return tecNO_ENTRY;
        auto const pooled = tickets.take();
        if (!pooled)
            return tecNO_ENTRY;
        ticket = *pooled;
        inner[sfTicketSequence] = *ticket;
    }

    // The fee there. A hook's export carries its own, as xport_prepare()
    // priced it or as the hook chose (preflight and preclaim held it to the
    // least the other network could take). The ledger's own exports are
    // priced here, as xport_prepare() would: what a multisigned transaction
    // of its type needs, at the unit the ledger has seen transactions pay
    // there (ltEXPORT_FEE, see Ingest.h), or the reference fee until one is
    // proven. On those a Fee of 0 stands for "fill in", since the format
    // requires the field to be present.
    if (automatic &&
        (!inner.isFieldPresent(sfFee) ||
         inner.getFieldAmount(sfFee) == beast::zero))
        inner.setFieldAmount(
            sfFee,
            feeUnit(view, dest) *
                static_cast<std::int64_t>(ExportSchema::feeUnits(
                    inner,
                    list->getFieldArray(sfSignerEntries).size(),
                    view.rules())));

    // The last ledger there that may include it, so that an export which never
    // lands can be told from one that is late, and expired (Export.h, step 5).
    // It is the hook's to set (xport_prepare() does, once a ledger has been
    // proven for the network); an export without one cannot expire. The
    // ledger's own exports carry none: they are resent until they land.
    std::optional<std::uint32_t> lastLedger;
    if (!automatic)
        lastLedger = inner[~sfLastLedgerSequence];

    // a destination below 1024 refuses NetworkID: it named the network here
    // (Export::destination) and goes no further
    if (dest <= 1024 && inner.isFieldPresent(sfNetworkID))
        inner.makeFieldAbsent(sfNetworkID);

    auto const t = exportedTx(inner, owner, nid, view.rules(), backstop);
    if (!t)
        return tecINTERNAL;

    auto const id = t->getTransactionID();
    auto const k = keylet::exportedTxn(view.seq(), id);
    if (view.exists(k))
        return tecDUPLICATE;

    // an automatic export is paid by its owner, as a hook's is by its account
    if (automatic)
    {
        auto const fee = autoFee(view, owner, dest);
        auto const bal = acc->getFieldAmount(sfBalance).xrp();
        if (bal < fee)
            return tecINSUFFICIENT_FUNDS;
        acc->setFieldAmount(sfBalance, bal - fee);
        ctx_.destroyXRP(fee);
        view.update(acc);
    }

    tickets.store(*ex);
    // every export in flight is listed on the record by its shadow ticket:
    // where Export::accept looks for exports to expire, relays for exports
    // to send, and export_info for what is out
    if (ticket &&
        !trackExpiry(*ex, keylet::shadowTicket(owner, dest, *ticket).key))
        return tecDIR_FULL;
    if (automatic && type == ttTICKET_CREATE)
        ex->setFieldU32(sfExportTicketPending, seqProxyOf(inner));
    if (automatic && type == ttSIGNER_LIST_SET)
    {
        auto const unl = view.read(keylet::UNLReport());
        ex->setFieldU32(
            sfExportKeysSeq, unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0);
    }
    view.update(ex);

    // The XPOP of whatever uses the ticket there may come back once. A
    // sequenced backstop has no ticket: its result returns as a sync import
    // of the exporter's own TicketCreate.
    if (ticket)
        if (auto const ter = setShadowTicket(
                view,
                owner,
                dest,
                *ticket,
                id,
                hook,
                lastLedger,
                ctx_.tx.getTransactionID(),
                j_);
            !isTesSuccess(ter))
            return ter;

    auto const sle = std::make_shared<SLE>(k);
    sle->setAccountID(sfOwner, owner);
    sle->setFieldU32(sfRemoteNetworkID, dest);
    sle->peekFieldObject(sfExportedTxn) = inner;
    sle->setFieldH256(sfTransactionHash, id);
    sle->setFieldU32(sfLedgerSequence, view.seq());
    sle->setFieldArray(sfSignerEntries, list->getFieldArray(sfSignerEntries));
    sle->setFieldU32(sfSignerQuorum, (*list)[sfSignerQuorum]);
    (*sle)[~sfEmitHookHash] = hook;
    // what the hook was given by xport(), and gets back as otxn_id in cbak
    sle->setFieldH256(sfExportTxnID, ctx_.tx.getTransactionID());
    view.insert(sle);
    return tesSUCCESS;
}

TER
Export::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;

    if (tx.getTxnType() == ttEXPORT)
    {
        // the export calls its hook back when it returns or closes below
        // quorum, rather than now
        std::optional<uint256> hook;
        if (auto const& d = obj(tx, sfEmitDetails);
            d.isFieldPresent(sfEmitCallback) &&
            d.getAccountID(sfEmitCallback) == account_)
            hook = d.getFieldH256(sfEmitHookHash);
        auto const ter =
            createExport(account_, obj(tx, sfExportedTxn), hook, false);
        if (isTesSuccess(ter))
            callback_ = HookCallback{account_, uint256{}, 0};
        return ter;
    }

    if (tx.getTxnType() == ttEXPORT_AUTO)
        return createExport(
            tx[sfOwner], obj(tx, sfExportedTxn), std::nullopt, true);

    if (tx.getTxnType() == ttEXPORT_FINAL && tx.isFieldPresent(sfLedgerIndex))
    {
        // An expiry (see preclaim): signed and sent, no longer includable
        // there, and not back. As after a close below quorum, the ticket is
        // released and the hook told it failed.
        auto const st = view.peek(Keylet{ltSHADOW_TICKET, tx[sfLedgerIndex]});
        if (!st)
            return tefINTERNAL;
        AccountID const owner = (*st)[sfAccount];
        std::uint32_t const dest = (*st)[sfRemoteNetworkID];
        std::optional<uint256> const hook = (*st)[~sfEmitHookHash];
        uint256 const exportID = (*st)[~sfExportTxnID].value_or(uint256{});
        auto const ex = view.peek(keylet::exporter(owner, dest));
        if (!releaseTicket(
                view,
                ex,
                owner,
                dest,
                (*st)[sfTicketSequence],
                tx[sfTransactionHash]))
            return tefINTERNAL;
        if (ex)
            view.update(ex);
        if (hook)
            callback_ = HookCallback{owner, *hook, 1, exportID};
        return tesSUCCESS;
    }

    auto const sle = view.peek(exportKeylet(tx));
    if (!sle)
        return tefINTERNAL;

    if (tx.getTxnType() == ttEXPORT_FINAL)
    {
        auto const owner = (*sle)[sfOwner];
        auto const dest = (*sle)[sfRemoteNetworkID];
        auto const& inner = obj(*sle, sfExportedTxn);
        if (exportWeight(*sle) < (*sle)[sfSignerQuorum])
        {
            // Below quorum it was never submitted, so nothing can use its
            // ticket there: back to the pool, shadow ticket gone. Tell the
            // hook, as for an emitted txn that failed, and let the ledger
            // try an automatic export again.
            auto const acc = view.peek(keylet::exporter(owner, dest));
            auto const ticket = inner[~sfTicketSequence];
            if (acc)
            {
                // (a sequenced backstop has no ticket to return)
                if (ticket)
                    releaseTicket(
                        view,
                        acc,
                        owner,
                        dest,
                        *ticket,
                        (*sle)[sfTransactionHash]);
                // its refill, by ticket or (the backstop) by Sequence, is
                // not out any more: the ledger may try again
                if ((*acc)[~sfExportTicketPending] == seqProxyOf(inner))
                    acc->makeFieldAbsent(sfExportTicketPending);
                if (inner[sfTransactionType] == ttSIGNER_LIST_SET &&
                    !sle->isFieldPresent(sfEmitHookHash))
                    acc->makeFieldAbsent(sfExportKeysSeq);
                // Under a list that is not one of ours nobody here can
                // sign, so the ledger stops exporting for it rather than
                // try, and charge it, every autoEvery ledgers for ever. (A
                // list of ours merely short of signatures is retried.)
                if (isExporter(*acc) && exportKeysSettled(view) &&
                    !holdsOurList(view, *acc))
                {
                    if (auto const ter = setExporter(view, acc, false);
                        !isTesSuccess(ter))
                        return ter;
                }
                view.update(acc);
            }
            if (sle->isFieldPresent(sfEmitHookHash))
                callback_ = HookCallback{
                    owner,
                    (*sle)[sfEmitHookHash],
                    1,
                    (*sle)[~sfExportTxnID].value_or(uint256{})};
        }
        else if (auto const ticket = inner[~sfTicketSequence])
        {
            // At quorum the shadow ticket (if it still names this export) takes
            // the transaction as signed, with its id there: the ledger then
            // holds what relays are to send, whenever they start. (Every quorum
            // of signatures has an id of its own, so what comes back is told by
            // its unsigned id, sfTransactionHash; see Import.)
            if (auto const st =
                    view.peek(keylet::shadowTicket(owner, dest, *ticket));
                st &&
                st->getFieldH256(sfTransactionHash) ==
                    sle->getFieldH256(sfTransactionHash))
            {
                Serializer s;
                inner.add(s);
                // (hashed as STTx::getTransactionID does, without parsing
                // it again)
                st->setFieldH256(
                    sfRemoteTxnID,
                    sha512Half(HashPrefix::transactionID, s.slice()));
                st->peekFieldObject(sfExportedTxn) = inner;
                view.update(st);
            }
        }
        view.erase(sle);
        return tesSUCCESS;
    }

    // XRPL requires Signers sorted by account
    auto& inner = sle->peekFieldObject(sfExportedTxn);
    STArray signers = inner.isFieldPresent(sfSigners)
        ? inner.getFieldArray(sfSigners)
        : STArray(sfSigners);
    signers.push_back(obj(tx, sfSigner));
    signers.sort([](STObject const& a, STObject const& b) {
        return a[sfAccount] < b[sfAccount];
    });
    inner.setFieldArray(sfSigners, signers);
    view.update(sle);
    return tesSUCCESS;
}

bool
Export::accept(Application& app, OpenView& view, beast::Journal j)
{
    if (!view.rules().enabled(featureExport))
        return false;

    auto const seq = view.seq();
    auto const nid = app.config().NETWORK_ID;
    bool changed = false;

    // flagged like other injected txns so OpenLedger does not relay them
    auto const inject = [&](TxType type, auto&& assemble) {
        STTx const tx(type, assemble);
        app.getHashRouter().setFlags(tx.getTransactionID(), SF_EMITTED);
        changed |= ripple::apply(app, view, tx, tapNONE, j).applied;
    };

    // Only exports from closed ledgers: their keys sort below this ledger's.
    // Applying (rather than raw-inserting) lets preclaim reject duplicates.
    auto const end = keylet::exportedTxn(seq, beast::zero).key;
    for (auto k = keylet::exportedTxn(0, beast::zero).key;
         auto const next = view.succ(k, end);
         k = *next)
    {
        auto const sle = view.read(Keylet{ltEXPORTED_TXN, *next});
        if (!sle)
            continue;

        auto const created = (*sle)[sfLedgerSequence];
        auto const id = (*sle)[sfTransactionHash];
        auto const& inner = obj(*sle, sfExportedTxn);

        if (seq >= created + window)
        {
            inject(ttEXPORT_FINAL, [&](STObject& o) {
                o[sfAccount] = AccountID();
                o[sfOwner] = (*sle)[sfOwner];
                o[sfLedgerSequence] = created;
                o[sfTransactionHash] = id;
            });
            continue;
        }

        // A validator signs only what the schema allows now: a type an
        // amendment has closed since the export was created is left to close
        // below quorum, and calls its hook back as such.
        if (!ExportSchema::allowed(inner, view.rules()))
            continue;

        // possibly several, if the exporter listed more than one of ours
        for (auto const& key : app.getExportKeys().signersFor(
                 sle->getFieldArray(sfSignerEntries)))
        {
            // not a structured binding: the lambda below captures these
            PublicKey const& pk = key.first;
            SecretKey const& sk = key.second;
            auto const acc = calcAccountID(pk);
            if (inner.isFieldPresent(sfSigners) &&
                listed(inner.getFieldArray(sfSigners), acc))
                continue;

            inject(ttEXPORT_SIGN, [&](STObject& o) {
                o[sfAccount] = AccountID();
                if (nid > 1024)
                    o[sfNetworkID] = nid;
                o[sfLedgerSequence] = created;
                o[sfTransactionHash] = id;
                auto& s = o.peekFieldObject(sfSigner);
                s[sfAccount] = acc;
                s[sfSigningPubKey] = pk.slice();
                s[sfTxnSignature] =
                    sign(pk, sk, buildMultiSigningData(inner, acc).slice());
            });
        }
    }

    // What exporters need: tickets when they run low, a new signer list when
    // the UNL's export keys change. Each exporter is looked at once every
    // autoEvery ledgers, in a slot its directory entry picks, so only that
    // fraction of the exporters is read each ledger (the directory holds
    // account root keys, which say nothing about their accounts).
    auto const unl = view.read(keylet::UNLReport());
    std::uint32_t const keysSeq =
        unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0;
    auto const examine = [&](std::shared_ptr<SLE const> const& acc) {
        if (!acc || acc->getType() != ltEXPORTER || !isExporter(*acc))
            return;

        // Its exports that were signed and sent, have a LastLedgerSequence
        // the ledger proven for that network has left behind, and never
        // came back: each is closed as failed (see Export.h, step 5).
        if (acc->isFieldPresent(sfIndexes))
        {
            std::size_t n = 0;
            for (auto const& key : acc->getFieldV256(sfIndexes))
            {
                auto const st = view.read(Keylet{ltSHADOW_TICKET, key});
                if (!st || !expired(view, *st))
                    continue;
                inject(ttEXPORT_FINAL, [&](STObject& o) {
                    o[sfAccount] = AccountID();
                    o[sfOwner] = (*st)[sfAccount];
                    o[sfLedgerSequence] = *(*st)[~sfLastLedgerSequence];
                    o[sfTransactionHash] = (*st)[sfTransactionHash];
                    o[sfLedgerIndex] = key;
                });
                if (++n >= maxExpirePerSlot)
                    break;
            }
        }

        // (an empty list records that the one there was deleted: preclaim
        // would refuse every automatic export with tecNO_TARGET, each slot)
        if (!acc->isFieldPresent(sfExportSignerList) ||
            obj(*acc, sfExportSignerList)
                .getFieldArray(sfSignerEntries)
                .empty())
            return;
        auto const id = acc->getAccountID(sfAccount);
        auto const dest = (*acc)[sfRemoteNetworkID];

        auto const tickets = ExportTickets::load(*acc);
        auto const automatic = [&](auto&& assemble) {
            inject(ttEXPORT_AUTO, [&](STObject& o) {
                o[sfAccount] = AccountID();
                o[sfOwner] = id;
                o[sfLedgerSequence] = seq;
                auto& in = o.peekFieldObject(sfExportedTxn);
                in[sfAccount] = id;
                in[sfSequence] = 0;
                in[sfOperationLimit] = nid;
                if (dest)
                    in[sfNetworkID] = dest;
                in.setFieldAmount(sfFee, XRPAmount{0});
                in[sfSigningPubKey] = Slice{};
                assemble(in);
            });
        };
        auto const ticketCreate = [&](STObject& in) {
            in[sfTransactionType] = static_cast<std::uint16_t>(ttTICKET_CREATE);
            in[sfTicketCount] = ticketBatch;
        };

        // a TicketCreate needs a ticket of its own, so one must remain
        if (tickets.count() < ticketLow && tickets.count() >= 1 &&
            !acc->isFieldPresent(sfExportTicketPending))
            automatic(ticketCreate);

        // The backstop. With an empty pool nothing ticketed can carry a refill,
        // and nobody else may hold the account's keys there, so the
        // TicketCreate is sequenced with the Sequence the ledger last saw, once
        // per such Sequence (sfExportTicketPending holds it while out; its
        // return, failed or not, records the next). A ticketed refill still out
        // does not hold this back: two batches landing is harmless.
        if (auto const sequence = (*acc)[~sfExportSequence]; tickets.empty() &&
            sequence && (*acc)[~sfExportTicketPending] != *sequence)
            automatic([&](STObject& in) {
                ticketCreate(in);
                in[sfSequence] = *sequence;
            });

        // The UNL's export keys have changed since its list was made. Keys
        // change rarely, in bursts, and the old ones keep signing while
        // validators' accounts list them, so the list is brought up to date at
        // most once every keyRefreshEvery ledgers, at a ledger of the
        // exporter's own, and only if it would differ.
        if (keysSeq != 0 && (*acc)[~sfExportKeysSeq].value_or(0) != keysSeq &&
            tickets.count() >= 2 &&
            (seq + slotOf(acc->key())) % keyRefreshEvery == 0)
        {
            auto const now = exportSigners(view);
            auto const& have = obj(*acc, sfExportSignerList);
            auto const& entries = have.getFieldArray(sfSignerEntries);
            bool const same = now.size() == entries.size() &&
                have[sfSignerQuorum] == quorumOf(now.size()) &&
                std::all_of(now.begin(), now.end(), [&](auto const& s) {
                                  return listed(entries, s.account);
                              });
            if (!now.empty() && !same)
                automatic([&](STObject& in) {
                    in[sfTransactionType] =
                        static_cast<std::uint16_t>(ttSIGNER_LIST_SET);
                    in[sfSignerQuorum] = 0;
                });
        }
    };
    forEachExporter(view, [&](uint256 const& key) {
        if ((seq + slotOf(key)) % autoEvery == 0)
            examine(view.read(Keylet{ltEXPORTER, key}));
    });

    return changed;
}

//------------------------------------------------------------------------------

ExportKeys::ExportKeys(
    Config const& config,
    ValidatorKeys const& vk,
    beast::Journal j)
    : j_(j), networkID_(config.NETWORK_ID)
{
    if (!vk.keys)
        return;
    master_ = vk.keys->masterPublicKey;

    if (auto const& v = config.section(SECTION_EXPORT_KEY_FILE).values();
        !v.empty())
        file_ = boost::filesystem::path(v.front());
    else if (!config.CONFIG_DIR.empty())
        // beside the config and validator token, not in the database
        // directory: operators wipe that to resync, and these keys are
        // unrecoverable for exporters still listing them
        file_ = config.CONFIG_DIR / "export_keys.txt";
    else
        JLOG(j_.warn()) << "ExportKeys: no [export_key_file] or config "
                           "directory, export keys will not persist";

    boost::system::error_code ec;
    if (!file_ || !boost::filesystem::exists(*file_, ec))
        return;
    std::ifstream in(file_->string());
    std::size_t lineNo = 0;
    for (std::string line; std::getline(in, line);)
    {
        ++lineNo;
        boost::algorithm::trim(line);
        if (line.empty())
            continue;
        auto const raw = strUnHex(line);
        if (!raw || raw->size() != 32)
        {
            // Refuse to start rather than silently rotate: a validator that
            // lost its keys can no longer sign for exporters listing them,
            // and the operator should restore a backup, not generate new ones.
            Throw<std::runtime_error>(
                "ExportKeys: malformed key on line " + std::to_string(lineNo) +
                " of " + file_->string() +
                " (expected one 64-character hex secret per line); restore "
                "the file from backup or remove it to generate a new key");
        }
        keys_.push_back(make(SecretKey(makeSlice(*raw))));
    }
    JLOG(j_.info()) << "ExportKeys: loaded " << keys_.size() << " from "
                    << *file_;
    // An empty file is not refused the way a malformed one is: there is no
    // key in it to have been lost. But a file that should have held keys and
    // does not is worth saying so before new ones are made in their place.
    if (keys_.empty())
    {
        JLOG(j_.warn())
            << "ExportKeys: " << *file_
            << " holds no keys; a new export key will be generated and "
               "nominated. If this validator had export keys before, restore "
               "the file from backup: exporters still listing the old keys "
               "cannot be signed for without them.";
    }
}

ExportKeys::Key
ExportKeys::make(SecretKey const& sk) const
{
    auto const pk = derivePublicKey(KeyType::ed25519, sk);
    auto const proof =
        sign(pk, sk, exportKeyProofData(*master_, pk, networkID_).slice());
    return Key{pk, sk, Blob(proof.begin(), proof.end())};
}

bool
ExportKeys::save() const
{
    if (!file_)
        return true;
    // A new owner-only file (O_EXCL: never one somebody else made or holds
    // open), synced, then renamed over the old one: neither a crash nor a
    // local user ever sees the secrets partly written or readable.
    auto const tmp = file_->string() + ".tmp";
    ::unlink(tmp.c_str());
    int const fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    FILE* const out = fd < 0 ? nullptr : ::fdopen(fd, "w");
    if (!out)
    {
        if (fd >= 0)
            ::close(fd);
        JLOG(j_.error()) << "ExportKeys: cannot create " << tmp;
        return false;
    }
    for (auto const& k : keys_)
        std::fprintf(
            out,
            "%s\n",
            strHex(k.sk.data(), k.sk.data() + k.sk.size()).c_str());
    bool const written = std::fflush(out) == 0 && ::fsync(fd) == 0;
    boost::system::error_code ec;
    if (std::fclose(out) != 0 || !written)
        ec = make_error_code(boost::system::errc::io_error);
    else
        boost::filesystem::rename(tmp, *file_, ec);
    if (ec)
    {
        JLOG(j_.error()) << "ExportKeys: cannot write " << *file_ << ": "
                         << ec.message();
        ::unlink(tmp.c_str());
        return false;
    }
    return true;
}

std::optional<std::pair<PublicKey, Blob>>
ExportKeys::nominate(ReadView const& ledger)
{
    if (!master_)
        return std::nullopt;
    std::lock_guard lock(mutex_);

    // the account's keys, newest first, and when the newest was recorded
    auto const sle = ledger.read(keylet::account(calcAccountID(*master_)));
    std::vector<Blob> onLedger;
    std::uint32_t headTime = 0;
    if (sle && sle->isFieldPresent(sfExportKeys))
        for (auto const& e : sle->getFieldArray(sfExportKeys))
        {
            if (onLedger.empty())
                headTime = e.getFieldU32(sfCloseTime);
            onLedger.push_back(e.getFieldVL(sfExportKey));
        }
    auto const at = [&](PublicKey const& pk) {
        return std::find(
            onLedger.begin(), onLedger.end(), Blob(pk.begin(), pk.end()));
    };

    bool rotate = keys_.empty();
    if (!rotate && !onLedger.empty())
    {
        auto const it = at(keys_.back().pk);
        auto const now = static_cast<std::uint32_t>(
            ledger.info().closeTime.time_since_epoch().count());
        // Nominated flag ledger after flag ledger and never recorded: the
        // ledger takes at most Export::maxKeysPerEpoch keys per validator
        // in a rotation epoch, so somebody else is nominating keys for this
        // validator too, and ours has to wait for the next epoch.
        if (it != onLedger.end())
            unrecorded_ = 0;
        else if (++unrecorded_ >= 3)
        {
            JLOG(j_.warn())
                << "ExportKeys: our export key " << strHex(keys_.back().pk)
                << " has been nominated " << unrecorded_
                << " times without being recorded; another server may be "
                   "validating with this token and its own export keys "
                   "(share one [export_key_file]), and no further key is "
                   "recorded for this validator until the next rotation "
                   "epoch";
        }
        // the ledger moved past ours (an older backup), or a new epoch
        bool const superseded = it != onLedger.end() && it != onLedger.begin();
        rotate = superseded ||
            (it == onLedger.begin() &&
             Export::rotationEpoch(now) > Export::rotationEpoch(headTime));
        if (superseded)
        {
            // Another server validating with this token nominated a newer
            // key. Rotating resolves it for now, but if this repeats every
            // flag ledger the two are replacing each other's current key and
            // need to share one [export_key_file].
            JLOG(j_.warn())
                << "ExportKeys: our export key " << strHex(keys_.back().pk)
                << " is no longer the newest on ledger; another "
                   "server may be validating with this token "
                   "(see [export_key_file])";
        }
    }

    auto const n = keys_.size();
    if (rotate)
    {
        keys_.push_back(make(randomKeyPair(KeyType::ed25519).second));
        JLOG(j_.warn()) << "ExportKeys: new export key "
                        << strHex(keys_.back().pk) << " (now holding "
                        << keys_.size() << "); back up "
                        << (file_ ? file_->string() : std::string{"the keys"});
    }

    // forget keys the account no longer lists, except the nominee
    if (sle)
        keys_.erase(
            std::remove_if(
                keys_.begin(),
                std::prev(keys_.end()),
                [&](Key const& k) { return at(k.pk) == onLedger.end(); }),
            std::prev(keys_.end()));

    // Nothing is nominated that is not on disk: once recorded on ledger an
    // exporter may list a key, and a restart that lost it could not sign.
    if ((rotate || keys_.size() != n) && !save())
    {
        if (rotate)
            keys_.pop_back();
        if (keys_.empty())
        {
            JLOG(j_.error()) << "ExportKeys: export key not persisted, not "
                                "nominating until it is";
            return std::nullopt;
        }
        // the file still holds every key kept here, so the last persisted
        // key is safe to keep nominating
        JLOG(j_.error()) << "ExportKeys: export keys not persisted, "
                            "nominating the last persisted key "
                         << strHex(keys_.back().pk);
    }
    return std::make_pair(keys_.back().pk, keys_.back().proof);
}

std::vector<std::pair<PublicKey, SecretKey>>
ExportKeys::signersFor(STArray const& entries) const
{
    std::vector<std::pair<PublicKey, SecretKey>> ret;
    std::lock_guard lock(mutex_);
    for (auto const& k : keys_)
        if (listed(entries, calcAccountID(k.pk)))
            ret.emplace_back(k.pk, k.sk);
    return ret;
}

}  // namespace ripple
