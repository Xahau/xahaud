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
#include <xrpld/app/misc/ValidatorKeys.h>
#include <xrpld/app/tx/applySteps.h>
#include <xrpld/app/tx/detail/Export.h>
#include <xrpld/core/ConfigSections.h>
#include <xrpld/ledger/OpenView.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/STValidation.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/jss.h>
#include <boost/filesystem.hpp>
#include <fstream>

namespace ripple {
namespace test {

/** Export keys: their nomination by UNL report, rotation on the account,
    crons following the rotation and the export_setup RPC (see
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
            auto& r = obj.peekFieldObject(sfExportKeyReport);
            r.setFieldVL(sfPublicKey, master);
            r.setFieldVL(sfExportKey, ek.first);
            r.setFieldVL(sfExportKeyProof, proof);
        });
    }

    static STTx
    activeValidatorReport(LedgerIndex seq, PublicKey const& master)
    {
        return STTx(ttUNL_REPORT, [&](auto& obj) {
            obj.setFieldU32(sfLedgerSequence, seq);
            obj.peekFieldObject(sfActiveValidator)
                .setFieldVL(sfPublicKey, master);
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
            STObject obj = exportKeyReport(seq, val.pk(), ek, nid);
            obj.peekFieldObject(sfActiveValidator)
                .setFieldVL(sfPublicKey, val.pk());
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
    testKeyFile()
    {
        testcase("export key file");
        using namespace jtx;
        namespace fs = boost::filesystem;

        auto const dir = fs::temp_directory_path() /
            fs::unique_path("xahaud-export-keys-%%%%%%%%");
        fs::create_directories(dir);
        struct Cleanup
        {
            fs::path dir;
            ~Cleanup()
            {
                boost::system::error_code ec;
                fs::remove_all(dir, ec);
            }
        } const cleanup{dir};
        auto const file = (dir / "export_keys.txt").string();

        auto cfg = envconfig(validator, "");
        cfg->section(SECTION_EXPORT_KEY_FILE).append(file);
        Env env{*this, std::move(cfg), supported_amendments() | featureExport};
        auto const& config = env.app().config();
        ValidatorKeys const vk(config, env.journal);
        if (!BEAST_EXPECT(vk.keys))
            return;
        auto const master = vk.keys->masterPublicKey;
        auto const id = calcAccountID(master);

        auto const lines = [&]() {
            std::vector<std::string> out;
            std::ifstream in(file);
            for (std::string l; std::getline(in, l);)
                if (!l.empty())
                    out.push_back(l);
            return out;
        };
        auto const entries = [](std::vector<AccountID> const& accs) {
            STArray a(sfSignerEntries);
            for (auto const& acc : accs)
            {
                a.push_back(STObject::makeInnerObject(sfSignerEntry));
                a.back()[sfAccount] = acc;
                a.back()[sfSignerWeight] = 1;
            }
            return a;
        };
        // a view of the closed ledger whose validator account lists `keys`,
        // newest first, each stamped with its close time
        auto const ledgerWith =
            [&](std::vector<std::pair<Blob, std::uint32_t>> const& keys) {
                auto view = std::make_shared<OpenView>(
                    env.closed().get(), env.closed());
                auto acc = std::make_shared<SLE>(keylet::account(id));
                acc->setAccountID(sfAccount, id);
                acc->setFieldAmount(sfBalance, XRPAmount{100'000'000});
                acc->setFieldU32(sfSequence, 1);
                if (!keys.empty())
                {
                    STArray a(sfExportKeys);
                    for (auto const& [k, t] : keys)
                    {
                        a.push_back(
                            STObject::makeInnerObject(sfExportKeyEntry));
                        a.back().setFieldVL(sfExportKey, k);
                        a.back().setFieldU32(sfCloseTime, t);
                    }
                    acc->setFieldArray(sfExportKeys, a);
                }
                if (view->exists(keylet::account(id)))
                    view->rawReplace(acc);
                else
                    view->rawInsert(acc);
                return view;
            };

        auto const P = Export::keyRotationPeriod;
        std::uint32_t const b0 = (closeTime(env) / P + 1) * P;
        closeAt(env, b0 + 100);

        // nothing persisted: the first nomination makes a key and the file
        std::optional<PublicKey> k1;
        {
            ExportKeys ek(config, vk, env.journal);
            auto const n = ek.nominate(*ledgerWith({}));
            if (!BEAST_EXPECT(n))
                return;
            k1 = n->first;
            BEAST_EXPECT(lines().size() == 1);
            BEAST_EXPECT(verify(
                *k1,
                exportKeyProofData(master, *k1, config.NETWORK_ID).slice(),
                makeSlice(n->second),
                true));

            // recorded on ledger in this epoch: the same key again
            auto const again =
                ek.nominate(*ledgerWith({{blob(*k1), b0 + 100}}));
            BEAST_EXPECT(again && again->first == *k1);
            BEAST_EXPECT(lines().size() == 1);
        }

        // reloaded from the file, with the secret
        {
            ExportKeys ek(config, vk, env.journal);
            auto const n = ek.nominate(*ledgerWith({}));
            BEAST_EXPECT(n && n->first == *k1);
            BEAST_EXPECT(
                ek.signersFor(entries({calcAccountID(*k1)})).size() == 1);
            BEAST_EXPECT(ek.signersFor(entries({})).empty());
        }

        // a new epoch rotates: both keys held, the newest nominated
        closeAt(env, b0 + P + 100);
        std::optional<PublicKey> k2;
        {
            ExportKeys ek(config, vk, env.journal);
            auto const n = ek.nominate(*ledgerWith({{blob(*k1), b0 + 100}}));
            if (!BEAST_EXPECT(n && n->first != *k1))
                return;
            k2 = n->first;
            BEAST_EXPECT(lines().size() == 2);
            BEAST_EXPECT(
                ek.signersFor(entries({calcAccountID(*k1), calcAccountID(*k2)}))
                    .size() == 2);
        }

        // another server validating with this token nominated a newer key:
        // ours is no longer the head on ledger, so rotate again
        {
            ExportKeys ek(config, vk, env.journal);
            auto const foreign = randomKeyPair(KeyType::ed25519).first;
            auto const n = ek.nominate(*ledgerWith(
                {{blob(foreign), b0 + P + 100},
                 {blob(*k2), b0 + P + 100},
                 {blob(*k1), b0 + 100}}));
            BEAST_EXPECT(n && n->first != *k1 && n->first != *k2);
            BEAST_EXPECT(lines().size() == 3);
        }

        // keys the account no longer lists are forgotten, except the nominee
        {
            ExportKeys ek(config, vk, env.journal);
            auto const n =
                ek.nominate(*ledgerWith({{blob(*k2), b0 + P + 100}}));
            BEAST_EXPECT(n && n->first != *k2);
            BEAST_EXPECT(lines().size() == 2);
            BEAST_EXPECT(ek.signersFor(entries({calcAccountID(*k1)})).empty());
            BEAST_EXPECT(
                ek.signersFor(entries({calcAccountID(*k2)})).size() == 1);
        }

        // a malformed line refuses to start rather than silently rotating
        {
            std::ofstream out(file, std::ios::app);
            out << "not a key\n";
        }
        BEAST_EXPECT(([&] {
            try
            {
                ExportKeys ek(config, vk, env.journal);
                return false;
            }
            catch (std::runtime_error const&)
            {
                return true;
            }
        })());

        // not a validator: nothing to nominate
        {
            auto plain = envconfig();
            ValidatorKeys const none(*plain, env.journal);
            ExportKeys ek(*plain, none, env.journal);
            BEAST_EXPECT(!ek.nominate(*ledgerWith({})));
        }
    }

    void
    testSignerListRPC()
    {
        testcase("export_setup");
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
            auto const r = env.rpc("json", "export_setup", "{}")[jss::result];
            BEAST_EXPECT(
                r[jss::tx_json][jss::TransactionType] == "SignerListSet");
            BEAST_EXPECT(
                accountsOf(r) == (std::set<std::string>{acc(k1), acc(k2b)}));
            BEAST_EXPECT(r[jss::tx_json][sfSignerQuorum.jsonName] == 2);
            BEAST_EXPECT(r[jss::tx_json][sfOperationLimit.jsonName] == nid);
            BEAST_EXPECT(r["export_keys_seq"] == 3);

            // a Xaman link to the same transaction
            std::string const prefix = "https://xaman.app/detect/";
            auto const url = r["xaman_url"].asString();
            BEAST_EXPECT(url.starts_with(prefix));
            auto const json = strUnHex(url.substr(prefix.size()));
            Json::Value tx;
            BEAST_EXPECT(
                json &&
                Json::Reader().parse(
                    std::string(json->begin(), json->end()), tx) &&
                tx == r[jss::tx_json]);
        }

        {
            // as it was in the first epoch
            Json::Value p;
            p["as_of"] = b0 + 1000;
            p["quorum"] = 1;
            p[jss::account] = v1.human();
            auto const r =
                env.rpc("json", "export_setup", to_string(p))[jss::result];
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
                env.rpc("json", "export_setup", to_string(p))[jss::result]
                    .isMember(jss::error));
            p["as_of"] = b0 + 1000;
            p["quorum"] = 3;
            BEAST_EXPECT(
                env.rpc("json", "export_setup", to_string(p))[jss::result]
                    .isMember(jss::error));
        }

        {
            // the command line form
            auto const r =
                env.rpc("export_setup", v1.human(), "1")[jss::result];
            BEAST_EXPECT(r[jss::tx_json][jss::Account] == v1.human());
            BEAST_EXPECT(r[jss::tx_json][sfSignerQuorum.jsonName] == 1);
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
        testKeyFile();
        testSignerListRPC();
    }
};

BEAST_DEFINE_TESTSUITE(ExportKeys, app, ripple);

}  // namespace test
}  // namespace ripple
