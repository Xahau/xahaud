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
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/tx/applySteps.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

/** Export keys: their nomination by UNL report, rotation on the account,
    crons following the rotation and the export_signer_list RPC (see
    Export.h). */
class ExportKeys_test : public beast::unit_test::suite
{
    using KeyPair = std::pair<PublicKey, SecretKey>;

    static Blob
    blob(PublicKey const& pk)
    {
        return Blob(pk.begin(), pk.end());
    }

    static std::uint32_t
    closeTime(jtx::Env& env)
    {
        return env.closed()->info().closeTime.time_since_epoch().count();
    }

    static void
    closeAt(jtx::Env& env, std::uint32_t t)
    {
        env.close(NetClock::time_point{std::chrono::seconds{t}});
    }

    // the ttUNL_REPORT the UNL makes of `master`'s nomination of `ek`
    static STTx
    exportKeyReport(
        LedgerIndex seq,
        PublicKey const& master,
        KeyPair const& ek,
        std::uint32_t networkID,
        std::optional<PublicKey> provenFor = std::nullopt)
    {
        auto const proof = sign(
            ek.first,
            ek.second,
            exportKeyProofData(provenFor.value_or(master), ek.first, networkID)
                .slice());
        return STTx(ttUNL_REPORT, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.set(([&]() {
                auto inner = std::make_unique<STObject>(sfExportKeyReport);
                inner->setFieldVL(sfPublicKey, master);
                inner->setFieldVL(sfExportKey, ek.first);
                inner->setFieldVL(sfExportKeyProof, proof);
                return inner;
            })());
        });
    }

    static STTx
    activeValidatorReport(LedgerIndex seq, PublicKey const& master)
    {
        return STTx(ttUNL_REPORT, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.set(([&]() {
                auto inner = std::make_unique<STObject>(sfActiveValidator);
                inner->setFieldVL(sfPublicKey, master);
                return inner;
            })());
        });
    }

    // put pseudo-txns in the open ledger, as consensus would, and close it
    static void
    inject(jtx::Env& env, std::vector<STTx> const& txs)
    {
        env.app().openLedger().modify([&](OpenView& view, beast::Journal) {
            for (auto const& tx : txs)
            {
                auto const id = tx.getTransactionID();
                auto s = std::make_shared<Serializer>();
                tx.add(*s);
                env.app().getHashRouter().setFlags(id, SF_PRIVATE2);
                view.rawTxInsert(id, std::move(s), nullptr);
            }
            return true;
        });
        env.close();
    }

    static void
    nominate(jtx::Env& env, PublicKey const& master, KeyPair const& ek)
    {
        inject(
            env,
            {exportKeyReport(
                env.current()->seq(),
                master,
                ek,
                env.app().config().NETWORK_ID)});
    }

    static std::vector<Blob>
    keysOf(jtx::Env& env, jtx::Account const& acc)
    {
        std::vector<Blob> out;
        if (auto const sle = env.le(acc);
            sle && sle->isFieldPresent(sfExportKeys))
            for (auto const& e : sle->getFieldArray(sfExportKeys))
                out.push_back(e.getFieldVL(sfExportKey));
        return out;
    }

    static std::uint32_t
    keysSeq(jtx::Env& env)
    {
        auto const unl = env.le(keylet::UNLReport());
        return unl ? (*unl)[~sfExportKeysSeq].value_or(0) : 0;
    }

    void
    testValidation()
    {
        testcase("validation carries a nomination");

        auto const vk = randomKeyPair(KeyType::secp256k1);
        auto const ek = randomKeyPair(KeyType::ed25519);
        auto const v = std::make_shared<STValidation>(
            NetClock::time_point{std::chrono::seconds{1000}},
            vk.first,
            vk.second,
            calcNodeID(vk.first),
            [&](STValidation& v) {
                v.setFieldH256(sfLedgerHash, uint256{7});
                v.setFieldU32(sfLedgerSequence, 512);
                v.setFlag(vfFullValidation);
                v.setFieldVL(sfExportKey, ek.first.slice());
                v.setFieldVL(sfExportKeyProof, Blob{1, 2, 3});
            });

        Serializer s;
        v->add(s);
        SerialIter sit(s.slice());
        STValidation const back(
            sit, [](PublicKey const& pk) { return calcNodeID(pk); }, true);
        BEAST_EXPECT(back.isValid());
        BEAST_EXPECT(back.getFieldVL(sfExportKey) == blob(ek.first));
        BEAST_EXPECT(back.getFieldVL(sfExportKeyProof) == (Blob{1, 2, 3}));
    }

    void
    testPreflight()
    {
        testcase("export key report preflight");
        using namespace jtx;

        {
            Env env{*this, supported_amendments() | featureExport};
            auto const nid = env.app().config().NETWORK_ID;
            auto const rules = env.current()->rules();
            Account const val{"val"};
            auto const ek = randomKeyPair(KeyType::ed25519);
            auto const seq = env.current()->seq();
            auto const pf = [&](STTx const& tx) {
                return preflight(env.app(), rules, tx, tapNONE, env.journal)
                    .ter;
            };

            BEAST_EXPECT(
                pf(exportKeyReport(seq, val.pk(), ek, nid)) == tesSUCCESS);

            // proof for another master, or another network
            BEAST_EXPECT(
                pf(exportKeyReport(seq, val.pk(), ek, nid, ek.first)) ==
                temBAD_SIGNATURE);
            BEAST_EXPECT(
                pf(exportKeyReport(seq, val.pk(), ek, nid + 1)) ==
                temBAD_SIGNATURE);

            // the master key itself is not an export key
            auto const mk = randomKeyPair(KeyType::ed25519);
            BEAST_EXPECT(
                pf(exportKeyReport(seq, mk.first, mk, nid)) == temMALFORMED);

            // export key reports travel alone
            auto tx = exportKeyReport(seq, val.pk(), ek, nid);
            STObject obj = tx;
            obj.set(([&]() {
                auto inner = std::make_unique<STObject>(sfActiveValidator);
                inner->setFieldVL(sfPublicKey, val.pk());
                return inner;
            })());
            Serializer s;
            obj.add(s);
            SerialIter sit(s.slice());
            BEAST_EXPECT(pf(STTx(sit)) == temMALFORMED);
        }

        {
            Env env{*this, supported_amendments() - featureExport};
            Account const val{"val"};
            BEAST_EXPECT(
                preflight(
                    env.app(),
                    env.current()->rules(),
                    exportKeyReport(
                        env.current()->seq(),
                        val.pk(),
                        randomKeyPair(KeyType::ed25519),
                        env.app().config().NETWORK_ID),
                    tapNONE,
                    env.journal)
                    .ter == temDISABLED);
        }
    }

    void
    testRotation()
    {
        testcase("export key rotation");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureExport};
        Account const val{"val"};
        Account const unfunded{"unfunded"};
        env.fund(XRP(10000), val);
        env.close();

        auto const P = Export::keyRotationPeriod;
        auto const epochOf = [&](std::uint32_t t) { return t / P; };
        auto const headTime = [&]() {
            return env.le(val)->getFieldArray(sfExportKeys)[0].getFieldU32(
                sfCloseTime);
        };

        // start just inside an epoch
        std::uint32_t const b0 = (closeTime(env) / P + 1) * P;
        closeAt(env, b0 + 100);

        // first nomination recorded, stamped with the close time
        auto const k1 = randomKeyPair(KeyType::ed25519);
        nominate(env, val.pk(), k1);
        BEAST_EXPECT(keysOf(env, val) == std::vector<Blob>{blob(k1.first)});
        BEAST_EXPECT(epochOf(headTime()) == epochOf(b0));
        BEAST_EXPECT(keysSeq(env) == 1);

        // reported again every flag ledger: no change
        nominate(env, val.pk(), k1);
        BEAST_EXPECT(keysOf(env, val) == std::vector<Blob>{blob(k1.first)});
        BEAST_EXPECT(keysSeq(env) == 1);

        // a new key in the same epoch replaces the head
        auto const k2 = randomKeyPair(KeyType::ed25519);
        nominate(env, val.pk(), k2);
        BEAST_EXPECT(keysOf(env, val) == std::vector<Blob>{blob(k2.first)});
        BEAST_EXPECT(keysSeq(env) == 2);

        // a new key in a later epoch is pushed
        closeAt(env, b0 + P + 100);
        auto const k3 = randomKeyPair(KeyType::ed25519);
        nominate(env, val.pk(), k3);
        BEAST_EXPECT(
            keysOf(env, val) ==
            (std::vector<Blob>{blob(k3.first), blob(k2.first)}));
        BEAST_EXPECT(keysSeq(env) == 3);

        // an older key never comes back
        nominate(env, val.pk(), k2);
        BEAST_EXPECT(keysOf(env, val).front() == blob(k3.first));
        BEAST_EXPECT(keysSeq(env) == 3);

        // at most maxExportKeys, newest first
        std::vector<KeyPair> later;
        for (std::uint32_t i = 0; i < 20; ++i)
        {
            closeAt(env, b0 + (2 + i) * P + 100);
            later.push_back(randomKeyPair(KeyType::ed25519));
            nominate(env, val.pk(), later.back());
        }
        auto const keys = keysOf(env, val);
        BEAST_EXPECT(keys.size() == Export::maxExportKeys);
        BEAST_EXPECT(keys.front() == blob(later.back().first));
        BEAST_EXPECT(keys.back() == blob(later[later.size() - 16].first));
        BEAST_EXPECT(keysSeq(env) == 23);

        // the account must exist: nothing is created for it
        nominate(env, unfunded.pk(), randomKeyPair(KeyType::ed25519));
        BEAST_EXPECT(!env.le(unfunded));
        BEAST_EXPECT(keysSeq(env) == 23);
    }

    void
    testReportReset()
    {
        testcase("export key reports do not suppress the UNLReport reset");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureExport};
        Account const a{"a"}, b{"b"};
        env.fund(XRP(10000), a, b);
        env.close();

        auto const avs = [&]() {
            std::vector<PublicKey> out;
            for (auto const& v :
                 env.le(keylet::UNLReport())->getFieldArray(sfActiveValidators))
                out.emplace_back(v[sfPublicKey]);
            return out;
        };

        inject(
            env,
            {activeValidatorReport(env.current()->seq(), a.pk()),
             activeValidatorReport(env.current()->seq(), b.pk())});
        BEAST_EXPECT(avs().size() == 2);

        // a later round with only `a` active, its export key report applied
        // in the same ledger: the array must still be rebuilt, not appended
        auto const seq = env.current()->seq();
        inject(
            env,
            {exportKeyReport(
                 seq,
                 a.pk(),
                 randomKeyPair(KeyType::ed25519),
                 env.app().config().NETWORK_ID),
             activeValidatorReport(seq, a.pk())});
        BEAST_EXPECT(avs() == std::vector<PublicKey>{a.pk()});
        BEAST_EXPECT(keysSeq(env) == 1);
    }

    void
    testRotationCron()
    {
        testcase("cron after each export key rotation");
        using namespace jtx;

        auto const P = Export::keyRotationPeriod;
        Account const alice{"alice"};

        {
            Env env{
                *this, supported_amendments() | featureExport | featureCron};
            env.fund(XRP(10000), alice);
            env.close();

            // the ledger picks the times
            env(cron::set(alice),
                txflags(tfCronExportRotation),
                cron::startTime(0),
                fee(XRP(1)),
                ter(temMALFORMED));
            env(cron::set(alice),
                txflags(tfCronExportRotation),
                cron::delay(P),
                cron::repeat(2),
                fee(XRP(1)),
                ter(temMALFORMED));
            env(cron::set(alice),
                txflags(tfCronExportRotation | tfCronUnset),
                fee(XRP(1)),
                ter(temMALFORMED));
            env(cron::set(alice),
                txflags(tfCronExportRotation),
                cron::repeat(257),
                fee(XRP(1)),
                ter(temMALFORMED));

            auto const now = static_cast<std::uint32_t>(
                env.current()->parentCloseTime().time_since_epoch().count());
            env(cron::set(alice),
                txflags(tfCronExportRotation),
                cron::repeat(4),
                fee(XRP(1)));
            env.close();

            auto const cronOf = [&]() {
                return env.le(
                    keylet::child(env.le(alice)->getFieldH256(sfCron)));
            };
            auto const c = cronOf();
            BEAST_EXPECT(c);
            if (!c)
                return;

            // just after the next rotation has settled, then every rotation
            auto const start = c->getFieldU32(sfStartTime);
            BEAST_EXPECT(start == Export::rotationCronTime(now, alice.id()));
            BEAST_EXPECT(start > now);
            BEAST_EXPECT(
                start % P >= Export::rotationSettle &&
                start % P < Export::rotationSettle + Export::rotationSpread);
            BEAST_EXPECT(c->getFieldU32(sfDelaySeconds) == P);
            BEAST_EXPECT(c->getFieldU32(sfRepeatCount) == 4);

            // fires once that time has passed, and rearms a rotation later
            closeAt(env, start + 100);
            env.close();
            bool fired = false;
            for (auto const& [tx, meta] : env.closed()->txs)
                if (tx->getTxnType() == ttCRON && (*tx)[sfOwner] == alice.id())
                    fired = true;
            BEAST_EXPECT(fired);

            auto const next = cronOf();
            BEAST_EXPECT(next);
            if (next)
            {
                BEAST_EXPECT(next->getFieldU32(sfStartTime) == start + P);
                BEAST_EXPECT(next->getFieldU32(sfRepeatCount) == 3);
            }
        }

        {
            Env env{
                *this, (supported_amendments() | featureCron) - featureExport};
            env.fund(XRP(10000), alice);
            env.close();
            env(cron::set(alice),
                txflags(tfCronExportRotation),
                fee(XRP(1)),
                ter(temINVALID_FLAG));
        }
    }

    void
    testSignerListRPC()
    {
        testcase("export_signer_list");
        using namespace jtx;

        Env env{*this, supported_amendments() | featureExport};
        Account const v1{"v1"}, v2{"v2"}, v3{"v3"};
        env.fund(XRP(10000), v1, v2, v3);
        env.close();

        auto const P = Export::keyRotationPeriod;
        std::uint32_t const b0 = (closeTime(env) / P + 1) * P;
        closeAt(env, b0 + 100);

        // v1 and v2 active and nominating, v3 active without a key
        auto const seq = env.current()->seq();
        auto const nid = env.app().config().NETWORK_ID;
        auto const k1 = randomKeyPair(KeyType::ed25519);
        auto const k2a = randomKeyPair(KeyType::ed25519);
        inject(
            env,
            {activeValidatorReport(seq, v1.pk()),
             activeValidatorReport(seq, v2.pk()),
             activeValidatorReport(seq, v3.pk()),
             exportKeyReport(seq, v1.pk(), k1, nid),
             exportKeyReport(seq, v2.pk(), k2a, nid)});

        // v2 rotates an epoch later
        closeAt(env, b0 + P + 100);
        auto const k2b = randomKeyPair(KeyType::ed25519);
        nominate(env, v2.pk(), k2b);

        auto const accountsOf = [](Json::Value const& r) {
            std::set<std::string> out;
            for (auto const& e : r[jss::tx_json][sfSignerEntries.jsonName])
                out.insert(
                    e[sfSignerEntry.jsonName][sfAccount.jsonName].asString());
            return out;
        };
        auto const acc = [](KeyPair const& kp) {
            return toBase58(calcAccountID(kp.first));
        };

        {
            auto const r =
                env.rpc("json", "export_signer_list", "{}")[jss::result];
            BEAST_EXPECT(
                r[jss::tx_json][jss::TransactionType] == "SignerListSet");
            BEAST_EXPECT(
                accountsOf(r) == (std::set<std::string>{acc(k1), acc(k2b)}));
            BEAST_EXPECT(r[jss::tx_json][sfSignerQuorum.jsonName] == 2);
            BEAST_EXPECT(r["export_keys_seq"] == 3);
            BEAST_EXPECT(r["truncated"] == false);
        }

        {
            // as it was in the first epoch
            Json::Value p;
            p["as_of"] = b0 + 1000;
            p["quorum"] = 1;
            p[jss::account] = v1.human();
            auto const r = env.rpc(
                "json", "export_signer_list", to_string(p))[jss::result];
            BEAST_EXPECT(
                accountsOf(r) == (std::set<std::string>{acc(k1), acc(k2a)}));
            BEAST_EXPECT(r[jss::tx_json][sfSignerQuorum.jsonName] == 1);
            BEAST_EXPECT(r[jss::tx_json][jss::Account] == v1.human());
        }

        {
            // before any key, and a bad quorum
            Json::Value p;
            p["as_of"] = b0 - 1;
            BEAST_EXPECT(
                env.rpc("json", "export_signer_list", to_string(p))[jss::result]
                    .isMember(jss::error));
            p["as_of"] = b0 + 1000;
            p["quorum"] = 3;
            BEAST_EXPECT(
                env.rpc("json", "export_signer_list", to_string(p))[jss::result]
                    .isMember(jss::error));
        }
    }

public:
    void
    run() override
    {
        testValidation();
        testPreflight();
        testRotation();
        testReportReset();
        testRotationCron();
        testSignerListRPC();
    }
};

BEAST_DEFINE_TESTSUITE(ExportKeys, app, ripple);

}  // namespace test
}  // namespace ripple
