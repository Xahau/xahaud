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
#include <xrpld/app/tx/apply.h>
#include <xrpld/app/tx/applySteps.h>
#include <xrpld/app/tx/detail/ApplyContext.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Sign.h>

namespace ripple {
namespace test {

/** The export transactors (see Export.h): ttEXPORT's checks, its use of the
    account's signer list and shadow tickets, and validators signing and
    finalizing pending exports.

    ttEXPORT is hook-emitted, so its preflight runs as emit() runs it and its
    doApply runs directly; a full hook round trip and the Import of a ticketed
    XPOP are left to the hook and XPOP suites. */
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

        // the pending export is attributed to the exporter
        auto const k = keylet::exportedTxn(view.seq(), txid(first));
        BEAST_EXPECT(view.read(k) && (*view.read(k))[sfOwner] == alice.id());

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

public:
    void
    run() override
    {
        testPreflight();
        testShadowTicket();
        testSignerList();
        testSignAndFinal();
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
