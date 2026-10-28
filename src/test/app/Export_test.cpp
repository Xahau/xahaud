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

#include <test/jtx.h>
#include <xrpld/app/ledger/OpenLedger.h>
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/applySteps.h>
#include <xrpld/app/tx/detail/ApplyContext.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/basics/base64.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxMeta.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

/** The export transactors (see Export.h): ttEXPORT's checks, its use of the
    account's signer list and shadow tickets, and validators signing and
    finalizing pending exports.

    ttEXPORT is hook-emitted, so its preflight runs as emit() runs it and its
    doApply runs directly. The Import that brings a ticketed export back is
    tested end to end with XPOPs built here, in the list form ExportRelay
    produces; a full hook round trip is left to the hook suites. */
class Export_test : public beast::unit_test::suite
{
    using KeyPair = std::pair<PublicKey, SecretKey>;

    // a payment from `from` on the other network, ready to be multisigned
    static STObject
    payment(
        AccountID const& from,
        std::uint32_t drops,
        std::optional<std::uint32_t> ticket = std::nullopt,
        std::optional<std::uint32_t> boundTo = std::nullopt)
    {
        STObject t(sfExportedTxn);
        t.setFieldU16(sfTransactionType, ttPAYMENT);
        t.setAccountID(sfAccount, from);
        t.setAccountID(sfDestination, noAccount());
        t.setFieldAmount(sfAmount, XRPAmount{drops});
        t.setFieldAmount(sfFee, XRPAmount{1000});
        t.setFieldU32(sfSequence, ticket ? 0 : 5);
        t.setFieldVL(sfSigningPubKey, Slice{});
        if (ticket)
            t.setFieldU32(sfTicketSequence, *ticket);
        if (boundTo)
            t.setFieldU32(sfOperationLimit, *boundTo);
        return t;
    }

    static uint256
    txid(STObject const& inner)
    {
        Serializer s;
        inner.add(s);
        return STTx{SerialIter{s.slice()}}.getTransactionID();
    }

    static STArray
    signerEntries(std::vector<AccountID> const& accounts)
    {
        STArray a(sfSignerEntries);
        for (auto const& acc : accounts)
        {
            STObject e(sfSignerEntry);
            e.setAccountID(sfAccount, acc);
            e.setFieldU16(sfSignerWeight, 1);
            a.push_back(std::move(e));
        }
        return a;
    }

    // `acc`'s signer list on the other network, as Import records it
    static void
    setSignerList(
        OpenView& view,
        AccountID const& acc,
        STArray const& entries,
        std::uint32_t quorum = 1)
    {
        auto sle = std::make_shared<SLE>(*view.read(keylet::account(acc)));
        auto l = STObject::makeInnerObject(sfExportSignerList);
        l[sfSignerQuorum] = quorum;
        l.setFieldArray(sfSignerEntries, entries);
        l[sfLedgerSequence] = 1;
        l[sfTransactionIndex] = 0;
        sle->peekFieldObject(sfExportSignerList) = std::move(l);
        view.rawReplace(sle);
    }

    // ttEXPORT's doApply, as the hook's emission would reach it
    static TER
    applyExport(jtx::Env& env, OpenView& view, STTx const& tx)
    {
        ApplyContext ctx(
            env.app(),
            view,
            tx,
            tesSUCCESS,
            XRPAmount{0},
            tapNONE,
            env.journal);
        Export ex(ctx);
        auto const ter = ex.doApply();
        if (isTesSuccess(ter))
            ctx.apply(ter);
        return ter;
    }

    // ttEXPORT as a hook would emit it
    static STTx
    exportTx(
        jtx::Env& env,
        AccountID const& from,
        STObject const& inner,
        std::uint32_t flags = 0,
        bool emitted = true)
    {
        auto const nid = env.app().config().NETWORK_ID;
        auto const seq = env.current()->seq();
        return STTx(ttEXPORT, [&](STObject& o) {
            o.setAccountID(sfAccount, from);
            o.setFieldAmount(sfFee, XRPAmount{100000});
            o.setFieldU32(sfSequence, 0);
            o.setFieldVL(sfSigningPubKey, Slice{});
            o.setFieldU32(sfFirstLedgerSequence, seq);
            o.setFieldU32(sfLastLedgerSequence, seq + 5);
            if (flags)
                o.setFieldU32(sfFlags, flags);
            if (nid > 1024)
                o.setFieldU32(sfNetworkID, nid);
            o.set(std::make_unique<STObject>(inner));
            if (emitted)
            {
                auto d = std::make_unique<STObject>(sfEmitDetails);
                d->setFieldU32(sfEmitGeneration, 1);
                d->setFieldU64(sfEmitBurden, 1);
                d->setFieldH256(sfEmitParentTxnID, uint256{1});
                d->setFieldH256(sfEmitNonce, uint256{2});
                d->setFieldH256(sfEmitHookHash, uint256{3});
                d->setAccountID(sfEmitCallback, from);
                o.set(std::move(d));
            }
        });
    }

    static STTx
    signTx(
        std::uint32_t nid,
        std::uint32_t created,
        uint256 const& id,
        KeyPair const& kp,
        Slice const& data)
    {
        auto const acc = calcAccountID(kp.first);
        return STTx(ttEXPORT_SIGN, [&](STObject& o) {
            o[sfAccount] = AccountID();
            if (nid > 1024)
                o[sfNetworkID] = nid;
            o[sfLedgerSequence] = created;
            o[sfTransactionHash] = id;
            auto& s = o.peekFieldObject(sfSigner);
            s[sfAccount] = acc;
            s[sfSigningPubKey] = kp.first.slice();
            s[sfTxnSignature] = sign(kp.first, kp.second, data);
        });
    }

    // a pending export, as ttEXPORT would have left it in ledger `created`
    static Keylet
    pending(
        OpenView& view,
        AccountID const& owner,
        STObject const& inner,
        std::uint32_t created,
        STArray const& entries)
    {
        auto const k = keylet::exportedTxn(created, txid(inner));
        auto const sle = std::make_shared<SLE>(k);
        sle->setAccountID(sfOwner, owner);
        sle->set(std::make_unique<STObject>(inner));
        sle->setFieldH256(sfTransactionHash, txid(inner));
        sle->setFieldU32(sfLedgerSequence, created);
        sle->setFieldArray(sfSignerEntries, entries);
        sle->setFieldU32(sfSignerQuorum, 1);
        view.rawInsert(sle);
        return k;
    }

    static std::vector<AccountID>
    signersOf(ReadView const& view, Keylet const& k)
    {
        std::vector<AccountID> out;
        auto const sle = view.read(k);
        if (!sle)
            return out;
        auto const& inner =
            sle->peekAtField(sfExportedTxn).downcast<STObject>();
        if (inner.isFieldPresent(sfSigners))
            for (auto const& s : inner.getFieldArray(sfSigners))
                out.push_back(s[sfAccount]);
        return out;
    }

    // the metadata node an export transaction left on ltEXPORTED_TXN
    static std::optional<STObject>
    exportNode(OpenView const& view, TxType type, SField const& nodeType)
    {
        for (auto const& [tx, meta] : view.txs)
        {
            if (tx->getTxnType() != type || !meta)
                continue;
            for (auto const& n : meta->getFieldArray(sfAffectedNodes))
                if (n.getFName() == nodeType &&
                    n.getFieldU16(sfLedgerEntryType) == ltEXPORTED_TXN)
                    return n;
        }
        return std::nullopt;
    }

    void
    testPreflight()
    {
        testcase("export preflight");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureExport};
        Account const alice{"alice"};
        auto const nid = env.app().config().NETWORK_ID;
        auto const a = calcAccountID(randomKeyPair(KeyType::ed25519).first);
        auto const b = calcAccountID(randomKeyPair(KeyType::ed25519).first);

        auto const pf = [&](STTx const& tx) {
            return preflight(
                       env.app(),
                       env.current()->rules(),
                       tx,
                       tapPREFLIGHT_EMIT,
                       env.journal)
                .ter;
        };

        auto const good = payment(alice.id(), 10);
        BEAST_EXPECT(pf(exportTx(env, alice.id(), good)) == tesSUCCESS);

        // emitted only
        BEAST_EXPECT(
            isTemMalformed(pf(exportTx(env, alice.id(), good, 0, false))));

        BEAST_EXPECT(
            pf(exportTx(env, alice.id(), good, 0x00010000)) == temINVALID_FLAG);

        // the inner transaction: from the hook account, unsigned, elsewhere
        BEAST_EXPECT(
            pf(exportTx(env, alice.id(), payment(a, 10))) == temMALFORMED);
        {
            auto signed_ = good;
            signed_.setFieldVL(sfTxnSignature, Blob{1, 2, 3});
            BEAST_EXPECT(
                pf(exportTx(env, alice.id(), signed_)) == temMALFORMED);
        }
        {
            auto here = good;
            here.setFieldU32(sfNetworkID, nid);
            BEAST_EXPECT(pf(exportTx(env, alice.id(), here)) == temMALFORMED);
        }

        // a SignerListSet must come back, or the list here would go stale
        {
            STObject sls(sfExportedTxn);
            sls.setFieldU16(sfTransactionType, ttSIGNER_LIST_SET);
            sls.setAccountID(sfAccount, alice.id());
            sls.setFieldAmount(sfFee, XRPAmount{1000});
            sls.setFieldU32(sfSequence, 0);
            sls.setFieldU32(sfSignerQuorum, 0);
            sls.setFieldVL(sfSigningPubKey, Slice{});
            BEAST_EXPECT(pf(exportTx(env, alice.id(), sls)) == temMALFORMED);
            sls.setFieldU32(sfTicketSequence, 3);
            BEAST_EXPECT(pf(exportTx(env, alice.id(), sls)) == temMALFORMED);
            sls.setFieldU32(sfOperationLimit, nid);
            BEAST_EXPECT(pf(exportTx(env, alice.id(), sls)) == tesSUCCESS);
        }

        // one base fee per signer in the account's list
        {
            env.fund(XRP(10000), alice);
            env.close();
            OpenView view(open_ledger, env.closed()->rules(), env.closed());
            auto const fee = [&](std::vector<AccountID> const& signers) {
                setSignerList(view, alice.id(), signerEntries(signers));
                return Export::calculateBaseFee(
                    view, exportTx(env, alice.id(), good));
            };
            BEAST_EXPECT(fee({a, b}) - fee({a}) == view.fees().base);

            // one bound back pays for the free Import that returns it
            setSignerList(view, alice.id(), signerEntries({a}));
            BEAST_EXPECT(
                Export::calculateBaseFee(
                    view,
                    exportTx(env, alice.id(), payment(alice.id(), 1, 7, nid))) -
                    fee({a}) ==
                view.fees().base * 10);
        }

        {
            Env off{*this, supported_amendments() - featureExport};
            BEAST_EXPECT(
                preflight(
                    off.app(),
                    off.current()->rules(),
                    exportTx(off, alice.id(), good),
                    tapPREFLIGHT_EMIT,
                    off.journal)
                    .ter == temDISABLED);
        }
    }

    void
    testShadowTicket()
    {
        testcase("shadow tickets");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureExport};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const nid = env.app().config().NETWORK_ID;
        OpenView view(open_ledger, env.closed()->rules(), env.closed());
        setSignerList(
            view,
            alice.id(),
            signerEntries(
                {calcAccountID(randomKeyPair(KeyType::ed25519).first)}));

        auto const owners = [&]() {
            return view.read(keylet::account(alice.id()))
                ->getFieldU32(sfOwnerCount);
        };
        auto const apply = [&](STObject const& inner) {
            return applyExport(env, view, exportTx(env, alice.id(), inner));
        };
        auto const ticket = [&](std::uint32_t t) {
            return view.read(keylet::shadowTicket(alice.id(), t));
        };

        auto const before = owners();

        // not ticketed, or ticketed but not bound back here: no ticket
        BEAST_EXPECT(apply(payment(alice.id(), 1)) == tesSUCCESS);
        BEAST_EXPECT(apply(payment(alice.id(), 2, 7)) == tesSUCCESS);
        BEAST_EXPECT(apply(payment(alice.id(), 3, 7, nid + 1)) == tesSUCCESS);
        BEAST_EXPECT(!ticket(7));
        BEAST_EXPECT(owners() == before);

        // ticketed and bound back: a ticket, held against the reserve
        auto const first = payment(alice.id(), 4, 7, nid);
        BEAST_EXPECT(apply(first) == tesSUCCESS);
        BEAST_EXPECT(ticket(7));
        BEAST_EXPECT(
            ticket(7) && (*ticket(7))[sfTransactionHash] == txid(first));
        BEAST_EXPECT(owners() == before + 1);

        // both name the hook its Import (or a close below quorum) calls back
        BEAST_EXPECT(ticket(7) && (*ticket(7))[~sfEmitHookHash] == uint256{3});

        // the pending export is attributed to the exporter
        auto const k = keylet::exportedTxn(view.seq(), txid(first));
        BEAST_EXPECT(view.read(k) && (*view.read(k))[sfOwner] == alice.id());
        BEAST_EXPECT(
            view.read(k) && (*view.read(k))[~sfEmitHookHash] == uint256{3});

        // an export not bound back calls back now, so names no hook
        auto const k2 =
            keylet::exportedTxn(view.seq(), txid(payment(alice.id(), 1)));
        BEAST_EXPECT(
            view.read(k2) && !view.read(k2)->isFieldPresent(sfEmitHookHash));

        // the same export again in this ledger
        BEAST_EXPECT(apply(first) == tecDUPLICATE);

        // exporting again with the ticket repoints it
        auto const second = payment(alice.id(), 5, 7, nid);
        BEAST_EXPECT(apply(second) == tesSUCCESS);
        BEAST_EXPECT(
            ticket(7) && (*ticket(7))[sfTransactionHash] == txid(second));
        BEAST_EXPECT(owners() == before + 1);
    }

    void
    testSignerList()
    {
        testcase("export signer list");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureExport};
        Account const alice{"alice"}, bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const nid = env.app().config().NETWORK_ID;
        auto const a = calcAccountID(randomKeyPair(KeyType::ed25519).first);
        OpenView view(open_ledger, env.closed()->rules(), env.closed());
        setSignerList(view, alice.id(), signerEntries({a}), 1);

        // only an account whose signer list is known can export
        auto const preclaim = [&](AccountID const& from) {
            return Export::preclaim(PreclaimContext(
                env.app(),
                view,
                tesSUCCESS,
                exportTx(env, from, payment(from, 1)),
                tapNONE));
        };
        BEAST_EXPECT(preclaim(alice.id()) == tesSUCCESS);
        BEAST_EXPECT(preclaim(bob.id()) == tecNO_TARGET);

        // the pending export snapshots it
        auto const pay = payment(alice.id(), 1);
        BEAST_EXPECT(
            applyExport(env, view, exportTx(env, alice.id(), pay)) ==
            tesSUCCESS);
        if (auto const p =
                view.read(keylet::exportedTxn(view.seq(), txid(pay)));
            BEAST_EXPECT(p))
        {
            BEAST_EXPECT(p->getFieldArray(sfSignerEntries).size() == 1);
            BEAST_EXPECT((*p)[sfSignerQuorum] == 1);
        }

        // one UNL validator with an export key on its account
        auto const master = randomKeyPair(KeyType::secp256k1);
        auto const key = randomKeyPair(KeyType::ed25519);
        {
            auto unl = std::make_shared<SLE>(keylet::UNLReport());
            STArray avs(sfActiveValidators);
            avs.push_back(STObject::makeInnerObject(sfActiveValidator));
            avs.back()[sfPublicKey] = master.first.slice();
            unl->setFieldArray(sfActiveValidators, avs);
            if (view.exists(keylet::UNLReport()))
                view.rawReplace(unl);
            else
                view.rawInsert(unl);

            auto const id = calcAccountID(master.first);
            auto acc = std::make_shared<SLE>(keylet::account(id));
            acc->setAccountID(sfAccount, id);
            acc->setFieldAmount(sfBalance, XRPAmount{100'000'000});
            acc->setFieldU32(sfSequence, 1);
            STArray keys(sfExportKeys);
            keys.push_back(STObject::makeInnerObject(sfExportKeyEntry));
            keys.back().setFieldVL(sfExportKey, key.first.slice());
            keys.back().setFieldU32(sfCloseTime, 0);
            acc->setFieldArray(sfExportKeys, keys);
            view.rawInsert(acc);
        }

        // a SignerListSet without entries moves onto the current export keys
        STObject sls(sfExportedTxn);
        sls.setFieldU16(sfTransactionType, ttSIGNER_LIST_SET);
        sls.setAccountID(sfAccount, alice.id());
        sls.setFieldAmount(sfFee, XRPAmount{1000});
        sls.setFieldU32(sfSequence, 0);
        sls.setFieldU32(sfTicketSequence, 3);
        sls.setFieldU32(sfOperationLimit, nid);
        sls.setFieldU32(sfSignerQuorum, 0);
        sls.setFieldVL(sfSigningPubKey, Slice{});
        BEAST_EXPECT(
            applyExport(env, view, exportTx(env, alice.id(), sls)) ==
            tesSUCCESS);

        auto const st = view.read(keylet::shadowTicket(alice.id(), 3));
        auto const p = st ? view.read(keylet::exportedTxn(
                                view.seq(), (*st)[sfTransactionHash]))
                          : nullptr;
        BEAST_EXPECT(p);
        if (!p)
            return;
        auto const& inner = p->peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(inner[sfSignerQuorum] == 1);
        BEAST_EXPECT(
            inner.getFieldArray(sfSignerEntries).size() == 1 &&
            inner.getFieldArray(sfSignerEntries)[0][sfAccount] ==
                calcAccountID(key.first));
        // signed by the list the account holds now, not the new one
        BEAST_EXPECT(
            p->getFieldArray(sfSignerEntries).size() == 1 &&
            p->getFieldArray(sfSignerEntries)[0][sfAccount] == a);
    }

    void
    testSignAndFinal()
    {
        testcase("export sign and final");
        using namespace jtx;

        Env env{
            *this,
            envconfig(validator, ""),
            supported_amendments() | featureExport};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        for (int i = 0; i < 6; ++i)
            env.close();

        auto const nid = env.app().config().NETWORK_ID;

        // this validator's export key, and another listed one held here
        auto const nominee = env.app().getExportKeys().nominate(*env.closed());
        BEAST_EXPECT(nominee);
        if (!nominee)
            return;
        auto const ours = calcAccountID(nominee->first);
        auto const other = randomKeyPair(KeyType::ed25519);
        auto const stranger = randomKeyPair(KeyType::ed25519);
        auto const entries = signerEntries({ours, calcAccountID(other.first)});

        OpenView view(open_ledger, env.closed()->rules(), env.closed());
        auto const created = env.closed()->seq();
        auto const inner = payment(alice.id(), 10);
        auto const id = txid(inner);
        auto const k = pending(view, alice.id(), inner, created, entries);

        auto const apply = [&](STTx const& tx) {
            return ripple::apply(env.app(), view, tx, tapNONE, env.journal).ter;
        };
        auto const data = [&](KeyPair const& kp) {
            return buildMultiSigningData(inner, calcAccountID(kp.first));
        };

        // the validator signs once
        BEAST_EXPECT(Export::accept(env.app(), view, env.journal));
        BEAST_EXPECT(signersOf(view, k) == std::vector<AccountID>{ours});
        BEAST_EXPECT(!Export::accept(env.app(), view, env.journal));

        // and its signature is attributed to the exporter
        auto const signNode = exportNode(view, ttEXPORT_SIGN, sfModifiedNode);
        BEAST_EXPECT(
            signNode &&
            signNode->peekAtField(sfFinalFields)
                    .downcast<STObject>()
                    .getAccountID(sfOwner) == alice.id());

        // only listed accounts, with good signatures, once each
        BEAST_EXPECT(
            apply(signTx(nid, created, id, stranger, data(stranger).slice())) ==
            tefBAD_AUTH);
        BEAST_EXPECT(
            apply(signTx(nid, created, id, other, data(stranger).slice())) ==
            tefBAD_SIGNATURE);
        BEAST_EXPECT(
            apply(signTx(nid, created, id, other, data(other).slice())) ==
            tesSUCCESS);
        BEAST_EXPECT(
            apply(signTx(nid, created, id, other, data(other).slice())) ==
            tefALREADY);

        // kept sorted, as the other network requires
        auto sorted = std::vector<AccountID>{ours, calcAccountID(other.first)};
        std::sort(sorted.begin(), sorted.end());
        BEAST_EXPECT(signersOf(view, k) == sorted);

        // `window` ledgers on, every node finalizes it
        auto const old = payment(alice.id(), 20);
        auto const createdOld = view.seq() - Export::window;
        auto const kOld = pending(view, alice.id(), old, createdOld, entries);

        auto const finalTx = [&](AccountID const& owner) {
            return STTx(ttEXPORT_FINAL, [&](STObject& o) {
                o[sfAccount] = AccountID();
                o[sfOwner] = owner;
                o[sfLedgerSequence] = createdOld;
                o[sfTransactionHash] = txid(old);
            });
        };
        BEAST_EXPECT(apply(finalTx(ours)) == tefFAILURE);
        BEAST_EXPECT(view.exists(kOld));

        BEAST_EXPECT(Export::accept(env.app(), view, env.journal));
        BEAST_EXPECT(!view.exists(kOld));
        BEAST_EXPECT(view.exists(k));

        // the result is in the DeletedNode, attributed to the exporter
        auto const finalNode = exportNode(view, ttEXPORT_FINAL, sfDeletedNode);
        BEAST_EXPECT(finalNode);
        if (finalNode)
        {
            auto const& ff =
                finalNode->peekAtField(sfFinalFields).downcast<STObject>();
            BEAST_EXPECT(ff.getAccountID(sfOwner) == alice.id());
            BEAST_EXPECT(ff.isFieldPresent(sfExportedTxn));
        }
    }

    // a manifest for `master`, delegating to `signing`
    static std::string
    manifest(KeyPair const& master, KeyPair const& signing)
    {
        STObject st(sfGeneric);
        st[sfSequence] = 1;
        st[sfPublicKey] = master.first;
        st[sfSigningPubKey] = signing.first;
        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(signing.first),
            signing.second);
        sign(
            st,
            HashPrefix::manifest,
            *publicKeyType(master.first),
            master.second,
            sfMasterSignature);
        Serializer s;
        st.add(s);
        return base64_encode(
            std::string(static_cast<char const*>(s.data()), s.size()));
    }

    /** A network Import trusts: a VL whose master key is an [import_vl_keys]
        entry, listing one validator, whose validation closes each XPOP. */
    struct OtherNetwork
    {
        KeyPair vlMaster = randomKeyPair(KeyType::secp256k1);
        KeyPair vlSigning = randomKeyPair(KeyType::secp256k1);
        KeyPair valMaster = randomKeyPair(KeyType::ed25519);
        KeyPair valSigning = randomKeyPair(KeyType::secp256k1);

        std::string
        vlKeyHex() const
        {
            return strHex(vlMaster.first);
        }

        // the XPOP of `tx`, in ledger `seq`, closed at `closeTime`, as the
        // only transaction there (so the proof is one root node), in the
        // list form ExportRelay builds
        Json::Value
        xpop(STTx const& tx, std::uint32_t seq, std::uint32_t closeTime) const
        {
            Serializer txs;
            tx.add(txs);
            TxMeta meta(tx.getTransactionID(), seq);
            meta.setResult(tesSUCCESS, 0);
            Serializer ms;
            meta.getAsObject().add(ms);

            // the leaf, hashed as a tnTRANSACTION_MD SHAMap node is
            Serializer leaf;
            leaf.addVL(txs.slice());
            leaf.addVL(ms.slice());
            leaf.addBitString(tx.getTransactionID());
            auto const leafHash = sha512Half(HashPrefix::txNode, leaf.slice());

            // the root: 16 children, the leaf under the key's first nibble
            Json::Value proof{Json::arrayValue};
            sha512_half_hasher h;
            using beast::hash_append;
            hash_append(h, HashPrefix::innerNode);
            int const branch = tx.getTransactionID().data()[0] >> 4;
            for (int b = 0; b < 16; ++b)
            {
                uint256 const c = b == branch ? leafHash : uint256{};
                proof.append(to_string(c));
                hash_append(h, c);
            }
            uint256 const txroot = static_cast<uint256>(h);

            std::uint64_t const coins = 99'999'999'999'999'000ULL;
            uint256 const phash{1}, acroot{2};
            std::uint32_t const pclose = closeTime - 10;
            uint256 const ledgerHash = sha512Half(
                HashPrefix::ledgerMaster,
                seq,
                coins,
                phash,
                txroot,
                acroot,
                pclose,
                closeTime,
                std::uint8_t(10),
                std::uint8_t(0));

            Json::Value x;
            auto& lg = x[jss::ledger];
            lg[jss::index] = seq;
            lg[jss::coins] = std::to_string(coins);
            lg[jss::phash] = to_string(phash);
            lg[jss::txroot] = to_string(txroot);
            lg[jss::acroot] = to_string(acroot);
            lg[jss::pclose] = pclose;
            lg[jss::close] = closeTime;
            lg[jss::cres] = 10;
            lg[jss::flags] = 0;
            auto& xt = x[jss::transaction];
            xt[jss::blob] = strHex(txs.slice());
            xt[jss::meta] = strHex(ms.slice());
            xt[jss::proof] = proof;

            STValidation const val(
                NetClock::time_point{NetClock::duration{closeTime + 1}},
                valSigning.first,
                valSigning.second,
                calcNodeID(valMaster.first),
                [&](STValidation& v) {
                    v.setFieldH256(sfLedgerHash, ledgerHash);
                    v.setFieldU32(sfLedgerSequence, seq);
                });
            Serializer vs;
            val.add(vs);
            x[jss::validation][jss::data][toBase58(
                TokenType::NodePublic, valSigning.first)] = strHex(vs.slice());

            Json::Value list;
            list[jss::sequence] = 1;
            list[jss::effective] = 0;
            list[jss::expiration] = closeTime + 365 * 24 * 60 * 60;
            auto& entry = list[jss::validators].append(Json::objectValue);
            entry[jss::validation_public_key] = strHex(valMaster.first);
            entry[jss::manifest] = manifest(valMaster, valSigning);
            auto const blob = Json::FastWriter().write(list);
            auto const sig =
                sign(vlSigning.first, vlSigning.second, makeSlice(blob));
            auto& unl = x[jss::validation][jss::unl];
            unl[jss::public_key] = vlKeyHex();
            unl[jss::manifest] = manifest(vlMaster, vlSigning);
            unl[jss::blob] = base64_encode(blob);
            unl[jss::signature] = strHex(sig);
            unl[jss::version] = 1;
            return x;
        }
    };

    void
    testLoopbackImport()
    {
        testcase("loopback import");
        using namespace jtx;

        OtherNetwork other;
        Env env{
            *this,
            network::makeNetworkVLConfig(21337, {other.vlKeyHex()}),
            supported_amendments() | featureExport};
        Account const alice{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const nid = env.app().config().NETWORK_ID;
        auto const a = calcAccountID(randomKeyPair(KeyType::ed25519).first);

        // alice exported three ticketed transactions bound back here, so
        // holds three shadow tickets, each naming the hook to call back
        env.app().openLedger().modify([&](OpenView& view, beast::Journal) {
            setSignerList(view, alice.id(), signerEntries({a}));
            for (std::uint32_t t : {7u, 8u, 9u})
                applyExport(
                    env,
                    view,
                    exportTx(env, alice.id(), payment(alice.id(), t, t, nid)));
            return true;
        });
        env.close();
        for (auto t : {7u, 8u, 9u})
            BEAST_EXPECT(env.le(keylet::shadowTicket(alice.id(), t)));

        auto const sle = [&]() { return env.le(keylet::account(alice.id())); };
        auto const seqBefore = (*sle())[sfSequence];
        auto const ownersBefore = (*sle())[sfOwnerCount];
        auto const balanceBefore = env.balance(alice);

        // what landed there, signed by whoever holds alice's keys there:
        // for an export the UNL, which Import never checks, so any key does
        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto const landed = [&](TxType type,
                                std::optional<std::uint32_t> ticket,
                                KeyPair const& by,
                                auto&& fill) {
            STTx tx(type, [&](STObject& o) {
                o[sfAccount] = alice.id();
                o.setFieldAmount(sfFee, XRPAmount{1000});
                o[sfSequence] = ticket ? 0u : 5u;
                if (ticket)
                    o[sfTicketSequence] = *ticket;
                o[sfOperationLimit] = nid;
                o[sfSigningPubKey] = by.first.slice();
                fill(o);
            });
            tx.sign(by.first, by.second);
            return tx;
        };
        auto const pay = [&](STObject& o) {
            o[sfDestination] = noAccount();
            o.setFieldAmount(sfAmount, XRPAmount{1});
        };
        auto const closeTime = static_cast<std::uint32_t>(
            env.closed()->info().closeTime.time_since_epoch().count());
        auto const import = [&](STTx const& tx, std::uint32_t seq) {
            return import::import(alice, other.xpop(tx, seq, closeTime));
        };

        // a ticketed XPOP must be a loopback: Sequence 0 and Fee 0
        auto const paid = landed(ttPAYMENT, 7, signer, pay);
        env(import(paid, 100), ter(temMALFORMED));
        env(import(paid, 100), seq(0u), ter(temMALFORMED));
        env(import(paid, 100), fee(drops(0)), ter(temMALFORMED));

        // and then it needs a shadow ticket
        env(import(landed(ttPAYMENT, 11, signer, pay), 100),
            seq(0u),
            fee(drops(0)),
            ter(tefNO_TICKET));

        // whereas a plain XPOP, which alice signed there herself, cannot
        // ride on the loopback's free pass
        env(import(
                landed(
                    ttPAYMENT,
                    std::nullopt,
                    KeyPair{alice.pk(), alice.sk()},
                    pay),
                100),
            seq(0u),
            fee(drops(0)),
            ter(tefPAST_SEQ));

        // the export comes back: free, no sequence used, ticket consumed
        env(import(paid, 100), seq(0u), fee(drops(0)), ter(tesSUCCESS));
        env.close();
        BEAST_EXPECT(!env.le(keylet::shadowTicket(alice.id(), 7)));
        BEAST_EXPECT((*sle())[sfSequence] == seqBefore);
        BEAST_EXPECT((*sle())[sfOwnerCount] == ownersBefore - 1);
        BEAST_EXPECT(env.balance(alice) == balanceBefore);
        BEAST_EXPECT(!sle()->isFieldPresent(sfImportSequence));

        // and only once
        env(import(paid, 100), seq(0u), fee(drops(0)), ter(tefNO_TICKET));

        // a SignerListSet coming back is recorded as alice's list there,
        // never installed here
        auto const b = calcAccountID(randomKeyPair(KeyType::ed25519).first);
        auto const sls = [&](std::uint32_t ticket, AccountID const& who) {
            return landed(ttSIGNER_LIST_SET, ticket, signer, [&](STObject& o) {
                o[sfSignerQuorum] = 1;
                o.setFieldArray(sfSignerEntries, signerEntries({who}));
            });
        };
        env(import(sls(8, b), 100), seq(0u), fee(drops(0)), ter(tesSUCCESS));
        env.close();
        BEAST_EXPECT(!env.le(keylet::signers(alice.id())));
        if (auto const l = sle()->isFieldPresent(sfExportSignerList)
                ? &sle()->peekAtField(sfExportSignerList).downcast<STObject>()
                : nullptr;
            BEAST_EXPECT(l))
        {
            BEAST_EXPECT((*l)[sfSignerQuorum] == 1);
            BEAST_EXPECT((*l)[sfLedgerSequence] == 100);
            BEAST_EXPECT((*l)[sfTransactionIndex] == 0);
            BEAST_EXPECT(
                l->getFieldArray(sfSignerEntries).size() == 1 &&
                l->getFieldArray(sfSignerEntries)[0][sfAccount] == b);
        }

        // one from an earlier ledger there arriving later does not win
        env(import(sls(9, a), 99), seq(0u), fee(drops(0)), ter(tesSUCCESS));
        env.close();
        BEAST_EXPECT(!env.le(keylet::shadowTicket(alice.id(), 9)));
        if (auto const l = sle()->isFieldPresent(sfExportSignerList)
                ? &sle()->peekAtField(sfExportSignerList).downcast<STObject>()
                : nullptr;
            BEAST_EXPECT(l))
        {
            BEAST_EXPECT((*l)[sfLedgerSequence] == 100);
            BEAST_EXPECT(
                l->getFieldArray(sfSignerEntries).size() == 1 &&
                l->getFieldArray(sfSignerEntries)[0][sfAccount] == b);
        }
        BEAST_EXPECT((*sle())[sfOwnerCount] == ownersBefore - 3);
    }

public:
    void
    run() override
    {
        testPreflight();
        testShadowTicket();
        testSignerList();
        testSignAndFinal();
        testLoopbackImport();
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
