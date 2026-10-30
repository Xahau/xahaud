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
#include <xrpld/core/Config.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/ledger/View.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/st.h>
#include <boost/algorithm/string/trim.hpp>
#include <fstream>

namespace ripple {

static STObject const&
obj(STObject const& o, SField const& f)
{
    return o.peekAtField(f).downcast<STObject>();
}

// `inner` as a transaction, if it parses under this network's formats and is
// shaped for multisigning by the UNL: from `account`, unsigned, not emitted,
// not a pseudo-txn, not replayable here, and bound back here (every export
// uses a ticket; one without TicketSequence gets one when it applies).
static std::optional<STTx>
exportedTx(STObject const& inner, AccountID const& account, std::uint32_t nid)
{
    try
    {
        Serializer s;
        inner.add(s);
        STTx const t{SerialIter{s.slice()}};
        if (t[sfAccount] == account && t.getSigningPubKey().empty() &&
            !t.isFieldPresent(sfTxnSignature) && !t.isFieldPresent(sfSigners) &&
            !t.isFieldPresent(sfEmitDetails) && !isPseudoTx(t) &&
            t[~sfNetworkID] != nid && t[sfSequence] == 0 &&
            t[~sfOperationLimit] == nid)
            return t;
    }
    catch (std::exception const&)
    {
    }
    return std::nullopt;
}

static Keylet
exportKeylet(STTx const& tx)
{
    return keylet::exportedTxn(tx[sfLedgerSequence], tx[sfTransactionHash]);
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
    std::uint32_t ticket,
    uint256 const& txid,
    std::optional<uint256> const& hook,
    beast::Journal j)
{
    auto const k = keylet::shadowTicket(acc, ticket);
    if (auto const st = view.peek(k))
    {
        st->setFieldH256(sfTransactionHash, txid);
        (*st)[~sfEmitHookHash] = hook;
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
    st->setFieldU32(sfTicketSequence, ticket);
    st->setFieldH256(sfTransactionHash, txid);
    (*st)[~sfEmitHookHash] = hook;
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
    std::uint32_t ticket,
    uint256 const& txid)
{
    auto const k = keylet::shadowTicket(acc, ticket);
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
    std::sort(ranges_.begin(), ranges_.end(), [](auto const& a, auto const& b) {
        return a.first < b.first;
    });
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
    auto const it = std::min_element(
        ranges_.begin(), ranges_.end(), [](auto const& a, auto const& b) {
            return a.count < b.count ||
                (a.count == b.count && a.first < b.first);
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
ExportTickets::store(SLE& acc) const
{
    if (ranges_.empty())
        acc.makeFieldAbsent(sfExportTickets);
    else
        acc.setFieldV256(sfExportTickets, pack());
}

ExportTickets
ExportTickets::load(SLE const& acc)
{
    if (!acc.isFieldPresent(sfExportTickets))
        return {};
    return ExportTickets(acc.getFieldV256(sfExportTickets));
}

TER
setExporter(ApplyView& view, std::shared_ptr<SLE> const& acc, bool on)
{
    auto const id = acc->getAccountID(sfAccount);
    bool const was = acc->isFlag(lsfExporter);
    if (on && !was)
    {
        auto const page = view.dirInsert(
            keylet::exporterDir(),
            keylet::account(id),
            [](std::shared_ptr<SLE> const&) {});
        if (!page)
            return tecDIR_FULL;
        acc->setFieldU64(sfExportNode, *page);
        acc->setFlag(lsfExporter);
    }
    else if (!on && was)
    {
        if (acc->isFieldPresent(sfExportNode) &&
            !view.dirRemove(
                keylet::exporterDir(),
                (*acc)[sfExportNode],
                keylet::account(id),
                false))
            return tefBAD_LEDGER;
        acc->makeFieldAbsent(sfExportNode);
        acc->clearFlag(lsfExporter);
    }
    return tesSUCCESS;
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
        if (std::none_of(out.begin(), out.end(), [&](auto const& s) {
                return s.key == pk;
            }))
            out.push_back({master, pk, t, calcAccountID(pk)});
        return true;
    });

    std::sort(out.begin(), out.end(), [](auto const& a, auto const& b) {
        return a.account < b.account;
    });
    if (auto const max = STTx::maxMultiSigners(&view.rules()); out.size() > max)
        out.erase(out.begin() + max, out.end());
    return out;
}

bool
listsExportKey(ReadView const& view, STArray const& entries)
{
    bool found = false;
    forEachExportKey(view, [&](PublicKey const&, STObject const& e) {
        PublicKey const pk(makeSlice(e.getFieldVL(sfExportKey)));
        return found = listed(entries, calcAccountID(pk));
    });
    return found;
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

// the exporter's signer list on the other network (a copy: the SLE read
// here does not outlive this function)
static std::optional<STObject>
signerList(ReadView const& view, AccountID const& acc)
{
    auto const sle = view.read(keylet::account(acc));
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

// what an export by `acc` costs: one signature per listed signer plus a final
static XRPAmount
fanOutFee(ReadView const& view, AccountID const& acc)
{
    auto const list = signerList(view, acc);
    std::int64_t const n =
        list ? list->getFieldArray(sfSignerEntries).size() : 0;
    return view.fees().base * (n + 1);
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
        fanOutFee(view, tx[sfAccount]);
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
            !exportedTx(obj(tx, sfExportedTxn), tx[sfAccount], nid))
            return temMALFORMED;
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

    if (tx.getTxnType() == ttEXPORT_SIGN)
    {
        auto const& s = obj(tx, sfSigner);
        auto const pk = s[sfSigningPubKey];
        if (!publicKeyType(pk) || s[sfAccount] != calcAccountID(PublicKey(pk)))
            return temMALFORMED;
    }

    if (tx.getTxnType() == ttEXPORT_AUTO)
    {
        // only what the ledger exports for exporters, without a ticket
        auto const& inner = obj(tx, sfExportedTxn);
        auto const type = inner[~sfTransactionType].value_or(0);
        if ((type != ttTICKET_CREATE && type != ttSIGNER_LIST_SET) ||
            inner.isFieldPresent(sfTicketSequence) ||
            !exportedTx(inner, tx[sfOwner], nid))
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
        if (!signerList(ctx.view, owner))
            return tecNO_TARGET;
        if (tx.getTxnType() == ttEXPORT_AUTO)
        {
            auto const sle = ctx.view.read(keylet::account(owner));
            if (!sle || !sle->isFlag(lsfExporter))
                return tecNO_PERMISSION;
            if (sle->getFieldAmount(sfBalance).xrp() <
                fanOutFee(ctx.view, owner) + ctx.view.fees().base)
                return tecINSUFFICIENT_FUNDS;
        }
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

    auto const acc = view.peek(keylet::account(owner));
    auto const list = signerList(view, owner);
    if (!acc || !list)
        return tefINTERNAL;

    auto const type = inner.getFieldU16(sfTransactionType);

    // a SignerListSet without entries moves the account onto the current
    // export keys: its old list signs, so keys rotate without lockout
    if (type == ttSIGNER_LIST_SET && !inner.isFieldPresent(sfSignerEntries))
    {
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
        inner[sfSignerQuorum] =
            static_cast<std::uint32_t>((signers.size() * 4 + 4) / 5);
    }

    // the ticket: the hook's, or the next from the pool
    auto tickets = ExportTickets::load(*acc);
    std::uint32_t ticket;
    if (auto const t = inner[~sfTicketSequence])
    {
        ticket = *t;
        tickets.remove(ticket);
    }
    else
    {
        auto const pooled = tickets.take();
        if (!pooled)
            return tecNO_ENTRY;
        ticket = *pooled;
        inner[sfTicketSequence] = ticket;
    }

    // the fee there: what a multisigned transaction needs. Fee 0 stands for
    // "fill in" since the format requires the field to be present.
    if (!inner.isFieldPresent(sfFee) ||
        inner.getFieldAmount(sfFee) == beast::zero)
        inner.setFieldAmount(
            sfFee,
            xrplBaseFee *
                static_cast<std::int64_t>(
                    list->getFieldArray(sfSignerEntries).size() + 1));

    auto const t = exportedTx(inner, owner, nid);
    if (!t)
        return tecINTERNAL;

    auto const id = t->getTransactionID();
    auto const k = keylet::exportedTxn(view.seq(), id);
    if (view.exists(k))
        return tecDUPLICATE;

    // an automatic export is paid by its owner, as a hook's is by its account
    if (automatic)
    {
        auto const fee = fanOutFee(view, owner) + view.fees().base;
        auto const bal = acc->getFieldAmount(sfBalance).xrp();
        if (bal < fee)
            return tecINSUFFICIENT_FUNDS;
        acc->setFieldAmount(sfBalance, bal - fee);
        ctx_.destroyXRP(fee);
    }

    tickets.store(*acc);
    if (automatic && type == ttTICKET_CREATE)
        acc->setFieldU32(sfExportTicketPending, ticket);
    if (automatic && type == ttSIGNER_LIST_SET)
    {
        auto const unl = view.read(keylet::UNLReport());
        acc->setFieldU32(
            sfExportKeysSeq, unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0);
    }
    view.update(acc);

    if (auto const ter = setShadowTicket(view, owner, ticket, id, hook, j_);
        !isTesSuccess(ter))
        return ter;

    auto const sle = std::make_shared<SLE>(k);
    sle->setAccountID(sfOwner, owner);
    sle->peekFieldObject(sfExportedTxn) = inner;
    sle->setFieldH256(sfTransactionHash, id);
    sle->setFieldU32(sfLedgerSequence, view.seq());
    sle->setFieldArray(sfSignerEntries, list->getFieldArray(sfSignerEntries));
    sle->setFieldU32(sfSignerQuorum, (*list)[sfSignerQuorum]);
    (*sle)[~sfEmitHookHash] = hook;
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

    auto const sle = view.peek(exportKeylet(tx));
    if (!sle)
        return tefINTERNAL;

    if (tx.getTxnType() == ttEXPORT_FINAL)
    {
        auto const owner = (*sle)[sfOwner];
        auto const& inner = obj(*sle, sfExportedTxn);
        if (exportWeight(*sle) < (*sle)[sfSignerQuorum])
        {
            // Below quorum it was never submitted, so nothing can use its
            // ticket there: back to the pool, shadow ticket gone. Tell the
            // hook, as for an emitted txn that failed, and let the ledger
            // try an automatic export again.
            auto const acc = view.peek(keylet::account(owner));
            auto const ticket = inner[~sfTicketSequence];
            if (acc && ticket)
            {
                if (eraseShadowTicket(
                        view, owner, *ticket, (*sle)[sfTransactionHash]))
                {
                    auto tickets = ExportTickets::load(*acc);
                    tickets.insert(*ticket);
                    tickets.store(*acc);
                }
                if ((*acc)[~sfExportTicketPending] == *ticket)
                    acc->makeFieldAbsent(sfExportTicketPending);
                if (inner[sfTransactionType] == ttSIGNER_LIST_SET &&
                    !sle->isFieldPresent(sfEmitHookHash))
                    acc->makeFieldAbsent(sfExportKeysSeq);
                view.update(acc);
            }
            if (sle->isFieldPresent(sfEmitHookHash))
                callback_ = HookCallback{owner, (*sle)[sfEmitHookHash], 1};
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
    // autoEvery ledgers, in a slot its account id picks.
    auto const unl = view.read(keylet::UNLReport());
    std::uint32_t const keysSeq =
        unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0;
    forEachItem(
        view,
        keylet::exporterDir(),
        [&](std::shared_ptr<SLE const> const& acc) {
            if (!acc || acc->getType() != ltACCOUNT_ROOT ||
                !acc->isFlag(lsfExporter) ||
                !acc->isFieldPresent(sfExportSignerList))
                return;
            auto const id = acc->getAccountID(sfAccount);
            auto const slot = (std::uint32_t(id.data()[0]) << 8) | id.data()[1];
            if ((seq + slot) % autoEvery != 0)
                return;

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
                    in.setFieldAmount(sfFee, XRPAmount{0});
                    in[sfSigningPubKey] = Slice{};
                    assemble(in);
                });
            };

            // a TicketCreate needs a ticket of its own, so one must remain
            if (tickets.count() < ticketLow && tickets.count() >= 1 &&
                !acc->isFieldPresent(sfExportTicketPending))
                automatic([&](STObject& in) {
                    in[sfTransactionType] =
                        static_cast<std::uint16_t>(ttTICKET_CREATE);
                    in[sfTicketCount] = ticketBatch;
                });

            if (keysSeq != 0 &&
                (*acc)[~sfExportKeysSeq].value_or(0) != keysSeq &&
                tickets.count() >= 2)
                automatic([&](STObject& in) {
                    in[sfTransactionType] =
                        static_cast<std::uint16_t>(ttSIGNER_LIST_SET);
                    in[sfSignerQuorum] = 0;
                });
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
    else if (auto const db = config.legacy("database_path"); !db.empty())
        file_ = boost::filesystem::path(db) / "export_keys.txt";
    else
        JLOG(j_.warn()) << "ExportKeys: no [export_key_file] or "
                           "database_path, export keys will not persist";

    boost::system::error_code ec;
    if (!file_ || !boost::filesystem::exists(*file_, ec))
        return;
    std::ifstream in(file_->string());
    for (std::string line; std::getline(in, line);)
    {
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
                "ExportKeys: malformed key on line " +
                std::to_string(keys_.size() + 1) + " of " + file_->string() +
                " (expected one 64-character hex secret per line); restore "
                "the file from backup or remove it to generate a new key");
        }
        keys_.push_back(make(SecretKey(makeSlice(*raw))));
    }
    JLOG(j_.info()) << "ExportKeys: loaded " << keys_.size() << " from "
                    << *file_;
}

ExportKeys::Key
ExportKeys::make(SecretKey const& sk) const
{
    auto const pk = derivePublicKey(KeyType::ed25519, sk);
    auto const proof =
        sign(pk, sk, exportKeyProofData(*master_, pk, networkID_).slice());
    return Key{pk, sk, Blob(proof.begin(), proof.end())};
}

void
ExportKeys::save() const
{
    if (!file_)
        return;
    // write then rename, so a crash never leaves a truncated file
    auto const tmp = boost::filesystem::path(file_->string() + ".tmp");
    boost::system::error_code ec;
    {
        std::ofstream out(tmp.string(), std::ios::trunc);
        boost::filesystem::permissions(
            tmp,
            boost::filesystem::owner_read | boost::filesystem::owner_write,
            ec);
        for (auto const& k : keys_)
            out << strHex(k.sk.data(), k.sk.data() + k.sk.size()) << "\n";
        if (!out.flush())
        {
            JLOG(j_.error()) << "ExportKeys: cannot write " << tmp;
            return;
        }
    }
    boost::filesystem::rename(tmp, *file_, ec);
    if (ec)
    {
        JLOG(j_.error()) << "ExportKeys: cannot replace " << *file_ << ": "
                         << ec.message();
    }
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

    if (rotate || keys_.size() != n)
        save();
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
