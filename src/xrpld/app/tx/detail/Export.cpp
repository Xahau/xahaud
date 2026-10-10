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
// not a pseudo-txn, not replayable here. SignerListSets must come back.
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
            t[~sfNetworkID] != nid &&
            (t.getTxnType() != ttSIGNER_LIST_SET ||
             (t.isFieldPresent(sfTicketSequence) &&
              t[~sfOperationLimit] == nid)))
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

// A ticketed export bound back here may return once via Import, which
// consumes this and calls back `hook`. Exporting again with the same ticket
// repoints it: only one transaction can use the ticket on the other network.
static TER
setShadowTicket(
    ApplyView& view,
    AccountID const& acc,
    std::uint32_t ticket,
    uint256 const& txid,
    std::optional<uint256> const& hook,
    beast::Journal j)
{
    auto const setHook = [&](SLE& st) {
        if (hook)
            st.setFieldH256(sfEmitHookHash, *hook);
        else if (st.isFieldPresent(sfEmitHookHash))
            st.makeFieldAbsent(sfEmitHookHash);
    };

    auto const k = keylet::shadowTicket(acc, ticket);
    if (auto const st = view.peek(k))
    {
        st->setFieldH256(sfTransactionHash, txid);
        setHook(*st);
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
    setHook(*st);
    st->setFieldU64(sfOwnerNode, *page);
    view.insert(st);
    adjustOwnerCount(view, sle, 1, j);
    return tesSUCCESS;
}

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

std::vector<ExportSigner>
exportSigners(ReadView const& view, std::optional<std::uint32_t> asOf)
{
    std::vector<ExportSigner> out;
    auto const unl = view.read(keylet::UNLReport());
    if (!unl || !unl->isFieldPresent(sfActiveValidators))
        return out;

    for (auto const& v : unl->getFieldArray(sfActiveValidators))
    {
        PublicKey const master(v[sfPublicKey]);
        auto const acc = view.read(keylet::account(calcAccountID(master)));
        if (!acc || !acc->isFieldPresent(sfExportKeys))
            continue;

        // newest first: the first at or before asOf
        for (auto const& e : acc->getFieldArray(sfExportKeys))
        {
            auto const t = e.getFieldU32(sfCloseTime);
            auto const key = e.getFieldVL(sfExportKey);
            if (asOf && t > *asOf)
                continue;
            PublicKey const pk(makeSlice(key));
            if (!std::any_of(out.begin(), out.end(), [&](auto const& s) {
                    return s.key == pk;
                }))
                out.push_back({master, pk, t, calcAccountID(pk)});
            break;
        }
    }

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
    auto const unl = view.read(keylet::UNLReport());
    if (!unl || !unl->isFieldPresent(sfActiveValidators))
        return false;
    for (auto const& v : unl->getFieldArray(sfActiveValidators))
    {
        PublicKey const master(v[sfPublicKey]);
        auto const acc = view.read(keylet::account(calcAccountID(master)));
        if (acc && acc->isFieldPresent(sfExportKeys))
            for (auto const& e : acc->getFieldArray(sfExportKeys))
            {
                auto const key = e.getFieldVL(sfExportKey);
                if (listed(entries, calcAccountID(PublicKey(makeSlice(key)))))
                    return true;
            }
    }
    return false;
}

std::uint32_t
exportWeight(SLE const& exported)
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

// the exporter's signer list on the other network
static STObject const*
signerList(ReadView const& view, AccountID const& acc)
{
    auto const sle = view.read(keylet::account(acc));
    return sle && sle->isFieldPresent(sfExportSignerList)
        ? &obj(*sle, sfExportSignerList)
        : nullptr;
}

XRPAmount
Export::calculateBaseFee(ReadView const& view, STTx const& tx)
{
    if (tx.getTxnType() != ttEXPORT)
        return XRPAmount{0};

    // an export fans out into one signature per listed signer plus a final,
    // and one bound back pays for the free Import that returns it
    auto const list = signerList(view, tx[sfAccount]);
    std::int64_t const n =
        list ? list->getFieldArray(sfSignerEntries).size() : 0;
    auto const& inner = obj(tx, sfExportedTxn);
    std::int64_t const back = inner.isFieldPresent(sfTicketSequence) &&
            inner.isFieldPresent(sfOperationLimit)
        ? 10
        : 0;
    return Transactor::calculateBaseFee(view, tx) +
        view.fees().base * (n + 1 + back);
}

NotTEC
Export::preflight(PreflightContext const& ctx)
{
    auto const& tx = ctx.tx;
    if (!ctx.rules.enabled(featureExport))
        return temDISABLED;

    if (tx.getTxnType() == ttEXPORT)
    {
        if (auto const ret = preflight1(ctx); !isTesSuccess(ret))
            return ret;
        if (tx.getFlags() & tfUniversalMask)
            return temINVALID_FLAG;
        if (!hook::isEmittedTxn(tx) ||
            !exportedTx(
                obj(tx, sfExportedTxn),
                tx[sfAccount],
                ctx.app.config().NETWORK_ID))
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

    return tesSUCCESS;
}

TER
Export::preclaim(PreclaimContext const& ctx)
{
    auto const& tx = ctx.tx;
    if (tx.getTxnType() == ttEXPORT)
        return signerList(ctx.view, tx[sfAccount]) ? TER{tesSUCCESS}
                                                   : TER{tecNO_TARGET};

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
Export::doApply()
{
    auto& view = ctx_.view();
    auto const& tx = ctx_.tx;
    auto const nid = ctx_.app.config().NETWORK_ID;

    if (tx.getTxnType() == ttEXPORT)
    {
        auto const list = signerList(view, account_);
        STObject inner = obj(tx, sfExportedTxn);

        // a SignerListSet without entries moves the account onto the current
        // export keys: its old list signs, so keys rotate without lockout
        if (inner.getFieldU16(sfTransactionType) == ttSIGNER_LIST_SET &&
            !inner.isFieldPresent(sfSignerEntries))
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

        auto const t = exportedTx(inner, account_, nid);
        if (!t || !list)
            return tefINTERNAL;

        auto const id = t->getTransactionID();
        auto const k = keylet::exportedTxn(view.seq(), id);
        if (view.exists(k))
            return tecDUPLICATE;

        // Bound back here, the export calls its hook back when it returns or
        // closes below quorum, rather than now
        auto const ticket = (*t)[~sfTicketSequence];
        bool const bound = ticket && (*t)[~sfOperationLimit] == nid;
        std::optional<uint256> hook;
        if (auto const& d = obj(tx, sfEmitDetails); bound &&
            d.isFieldPresent(sfEmitCallback) &&
            d.getAccountID(sfEmitCallback) == account_)
            hook = d.getFieldH256(sfEmitHookHash);
        if (bound)
        {
            if (auto const ter =
                    setShadowTicket(view, account_, *ticket, id, hook, j_);
                !isTesSuccess(ter))
                return ter;
        }

        auto const sle = std::make_shared<SLE>(k);
        sle->setAccountID(sfOwner, account_);
        sle->peekFieldObject(sfExportedTxn) = inner;
        sle->setFieldH256(sfTransactionHash, id);
        sle->setFieldU32(sfLedgerSequence, view.seq());
        sle->setFieldArray(
            sfSignerEntries, list->getFieldArray(sfSignerEntries));
        sle->setFieldU32(sfSignerQuorum, (*list)[sfSignerQuorum]);
        if (hook)
            sle->setFieldH256(sfEmitHookHash, *hook);
        view.insert(sle);
        if (bound)
            callback_ = HookCallback{account_, uint256{}, 0};
        return tesSUCCESS;
    }

    auto const sle = view.peek(exportKeylet(tx));
    if (!sle)
        return tefINTERNAL;

    if (tx.getTxnType() == ttEXPORT_FINAL)
    {
        // Below quorum it was never submitted. Tell the hook, as for an
        // emitted txn that failed, but keep the shadow ticket: identical
        // content exported again could still land.
        if (sle->isFieldPresent(sfEmitHookHash) &&
            exportWeight(*sle) < (*sle)[sfSignerQuorum])
            callback_ =
                HookCallback{(*sle)[sfOwner], (*sle)[sfEmitHookHash], 1};
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

void
Export::preCompute()
{
    XRPL_ASSERT(
        (account_ == beast::zero) == (ctx_.tx.getTxnType() != ttEXPORT),
        "ripple::Export::preCompute : account matches txn type");
}

bool
Export::accept(Application& app, OpenView& view, beast::Journal j)
{
    if (!view.rules().enabled(featureExport))
        return false;

    auto const seq = view.seq();
    auto const nid = app.config().NETWORK_ID;
    bool changed = false;

    auto const inject = [&](TxType type, auto&& assemble) {
        changed |=
            ripple::apply(app, view, STTx(type, assemble), tapNONE, j).applied;
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
            Throw<std::runtime_error>(
                "ExportKeys: malformed key in " + file_->string());
        keys_.push_back(make(SecretKey(makeSlice(*raw))));
    }
    JLOG(j_.info()) << "ExportKeys: loaded " << keys_.size();
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
        JLOG(j_.error()) << "ExportKeys: cannot replace " << *file_ << ": "
                         << ec.message();
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
        rotate = (it != onLedger.end() && it != onLedger.begin()) ||
            (it == onLedger.begin() &&
             Export::rotationEpoch(now) > Export::rotationEpoch(headTime));
    }

    auto const n = keys_.size();
    if (rotate)
    {
        keys_.push_back(make(randomKeyPair(KeyType::ed25519).second));
        JLOG(j_.info()) << "ExportKeys: new export key "
                        << strHex(keys_.back().pk);
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
