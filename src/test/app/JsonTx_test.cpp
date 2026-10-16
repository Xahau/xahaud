//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2014 Ripple Labs Inc.

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
#include <test/jtx/TestHelpers.h>
#include <test/jtx/acctdelete.h>
#include <test/jtx/permissioned_domains.h>
#include <test/jtx/ticket.h>
#include <test/jtx/token.h>
#include <xrpld/app/ledger/LocalTxs.h>
#include <xrpld/app/misc/CanonicalTXSet.h>
#include <xrpld/app/misc/HashRouter.h>
#include <xrpld/app/tx/apply.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/STParsedJSON.h>
#include <xrpl/protocol/jss.h>

#include <functional>
#include <optional>
#include <string>

namespace ripple {
namespace test {

/** Tests featureJsonTx end to end: the submit RPC's { tx, sig } form, the
    relay/consensus signature path, and sfTime as the stand-in for both the
    account Sequence and LastLedgerSequence, with sfLastTxnTime on the
    AccountRoot as its replay protection.
*/
struct JsonTx_test : public beast::unit_test::suite
{
    //--------------------------------------------------------------------------
    // helpers

    // The open ledger's parent close time in milliseconds since the ripple
    // epoch: what checkPriorTxAndLastLedger compares sfTime against.
    static std::uint64_t
    nowMs(jtx::Env& env)
    {
        return static_cast<std::uint64_t>(env.current()
                                              ->parentCloseTime()
                                              .time_since_epoch()
                                              .count()) *
            1000;
    }

    static std::optional<std::uint64_t>
    lastTxnTime(jtx::Env& env, jtx::Account const& a)
    {
        auto const sle = env.le(a);
        if (!sle || !sle->isFieldPresent(sfLastTxnTime))
            return std::nullopt;
        return sle->getFieldU64(sfLastTxnTime);
    }

    // A Payment laid out the way a wallet would show it to its signer.
    static std::string
    paymentText(
        jtx::Account const& from,
        jtx::Account const& to,
        std::string const& amount,
        std::optional<std::uint32_t> seq,
        std::optional<std::uint64_t> time,
        std::string const& extra = "")
    {
        std::string s = "{\n  \"TransactionType\": \"Payment\",\n";
        s += "  \"Account\": \"" + from.human() + "\",\n";
        s += "  \"Destination\": \"" + to.human() + "\",\n";
        s += "  \"Amount\": " + amount + ",\n";
        s += "  \"Fee\": \"12\",\n";
        if (seq)
            s += "  \"Sequence\": " + std::to_string(*seq) + ",\n";
        if (time)
            s += "  \"Time\": \"" + jsontx_iso_str(*time) + "\",\n";
        s += extra;
        s += "  \"SigningPubKey\": \"" + strHex(from.pk().slice()) + "\"\n}";
        return s;
    }

    static Buffer
    jsonSign(jtx::Account const& signer, std::string const& text)
    {
        return sign(
            signer.pk(), signer.sk(), makeSlice(jsontx_signing_data(text)));
    }

    static Json::Value
    submitJson(jtx::Env& env, Json::Value const& params)
    {
        return env.rpc("json", "submit", to_string(params))[jss::result];
    }

    static Json::Value
    submitJson(jtx::Env& env, std::string const& text, Buffer const& sig)
    {
        Json::Value p;
        p[jss::tx] = text;
        p[jss::sig] = strHex(sig);
        return submitJson(env, p);
    }

    static Json::Value
    submitBlob(jtx::Env& env, STTx const& tx)
    {
        return env.rpc(
            "submit", strHex(tx.getSerializer().slice()))[jss::result];
    }

    static std::string
    engine(Json::Value const& jr)
    {
        return jr.isMember(jss::engine_result)
            ? jr[jss::engine_result].asString()
            : "error:" + jr[jss::error].asString() + ":" +
                jr[jss::error_exception].asString() +
                jr[jss::error_message].asString();
    }

    // sfTime on a jtx transaction. The jtx JSON goes through STParsedJSON,
    // where a UInt64 is spelled as hex.
    static Json::Value
    withTime(Json::Value jv, std::uint64_t ms)
    {
        jv[sfTime.jsonName] = jsontx_u64_str(sfTime, ms);
        return jv;
    }

    // The STTx a jtx transaction would submit, so it can be replayed.
    static STTx
    stx(jtx::JTx const& jt)
    {
        return *jt.stx;
    }

    template <class F>
    static STTx
    mutate(STTx const& tx, F&& f)
    {
        STObject obj(tx);
        f(obj);
        Serializer ser;
        obj.add(ser);
        SerialIter si(ser.slice());
        return STTx(si);
    }

    /** Applies `tx` to a throwaway copy of the open ledger and reports the
        TER. Nothing is retained. */
    static TER
    applyDirect(jtx::Env& env, std::shared_ptr<STTx const> const& tx)
    {
        TER ret = tesSUCCESS;
        env.app().openLedger().modify([&](OpenView& view, beast::Journal j) {
            ret = ripple::apply(env.app(), view, *tx, tapNONE, j).ter;
            return false;
        });
        return ret;
    }

    //--------------------------------------------------------------------------

    void
    testDisabled(FeatureBitset features)
    {
        testcase("amendment disabled");
        using namespace jtx;

        Env env{*this, features - featureJsonTx};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        // the RPC says the feature is not live rather than that the
        // transaction is unsigned
        auto const text = paymentText(alice, bob, "\"1000000\"", 0, nowMs(env));
        auto const jr = submitJson(env, text, jsonSign(alice, text));
        BEAST_EXPECT(jr[jss::error] == "notEnabled");

        // sfTime on an ordinary binary transaction
        env(withTime(noop(alice), nowMs(env)), ter(temDISABLED));
        env(withTime(noop(alice), nowMs(env)), seq(0), ter(temDISABLED));

        // a delta on an ordinary binary transaction never gets past
        // checkValidity, whatever else it carries
        auto const withDelta =
            mutate(stx(env.jt(noop(alice))), [](STObject& o) {
                o.setFieldVL(sfJsonTxDelta, Slice("\x01\x00\x04", 3));
            });
        auto const r = submitBlob(env, withDelta);
        BEAST_EXPECT(r[jss::error] == "invalidTransaction");
        BEAST_EXPECT(
            r[jss::error_exception].asString().find("JsonTx is not enabled") !=
            std::string::npos);

        // nothing was applied
        env.close();
        BEAST_EXPECT(!lastTxnTime(env, alice));
    }

    void
    testActivationBoundary(FeatureBitset features)
    {
        testcase("signature cache across the activation boundary");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        // Build the transaction the submit RPC would, without submitting it.
        auto const text = paymentText(alice, bob, "\"1000000\"", 0, nowMs(env));
        auto const [san, diff] = sanitize_jsontx(text);
        Json::Value jv;
        BEAST_EXPECT(Json::Reader{}.parse(san, jv));
        auto const ms = jsontx_iso(jv[sfTime.fieldName].asString());
        jv.removeMember(sfTime.fieldName);
        STParsedJSONObject parsed("tx_json", jv);
        if (!BEAST_EXPECT(parsed.object))
            return;
        parsed.object->setFieldU64(sfTime, ms);
        parsed.object->setFieldVL(sfTxnSignature, jsonSign(alice, text));
        parsed.object->setFieldVL(sfJsonTxDelta, makeSlice(diff));
        STTx const tx(std::move(*parsed.object));

        // Asked first under rules without the amendment: SigBad, and that
        // answer must not be cached - PeerImp checks against the validated
        // ledger's rules and consensus against the open ledger's, so across
        // activation the same id is asked both ways.
        Rules const before{std::unordered_set<uint256, beast::uhash<>>{}};
        auto& router = env.app().getHashRouter();
        BEAST_EXPECT(
            checkValidity(router, tx, before, env.app().config()).first ==
            Validity::SigBad);
        BEAST_EXPECT(
            checkValidity(
                router, tx, env.current()->rules(), env.app().config())
                .first == Validity::Valid);

        // A bad JsonTx is cached as bad once the rules allow it at all.
        auto const bad = mutate(tx, [](STObject& o) {
            o.setFieldAmount(sfAmount, STAmount(XRPAmount(2)));
        });
        BEAST_EXPECT(
            checkValidity(
                router, bad, env.current()->rules(), env.app().config())
                .first == Validity::SigBad);
        BEAST_EXPECT(
            router.getFlags(bad.getTransactionID()) & SF_PRIVATE1);  // SIGBAD
    }

    void
    testSubmit(FeatureBitset features)
    {
        testcase("submit { tx, sig }");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const aliceSeq = env.seq(alice);
        auto const bobBalance = env.balance(bob).value();

        // time-sequenced: Sequence 0, no ledger query needed to sign
        auto const t1 = nowMs(env);
        auto const text = paymentText(alice, bob, "\"1000000\"", 0, t1);
        auto const jr = submitJson(env, text, jsonSign(alice, text));
        BEAST_EXPECT(engine(jr) == "tesSUCCESS");

        // what went on the wire carries the delta and reconstructs the
        // exact text that was signed
        {
            auto const blob = strUnHex(jr[jss::tx_blob].asString());
            if (BEAST_EXPECT(blob))
            {
                SerialIter si(makeSlice(*blob));
                STTx const wire(si);
                BEAST_EXPECT(wire.isFieldPresent(sfJsonTxDelta));
                BEAST_EXPECT(wire.isTimeSequenced());
                BEAST_EXPECT(jsontx_verify(wire) == text);
            }
        }
        env.close();

        BEAST_EXPECT(env.balance(bob).value() == bobBalance + XRP(1).value());
        // the account Sequence is neither checked nor consumed
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        BEAST_EXPECT(lastTxnTime(env, alice) == t1);

        // sequence-sequenced JsonTx, no Time at all: an ordinary
        // transaction with a plaintext signature
        {
            auto const t = paymentText(alice, bob, "\"1000000\"", aliceSeq, {});
            BEAST_EXPECT(
                engine(submitJson(env, t, jsonSign(alice, t))) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(env.seq(alice) == aliceSeq + 1);
            BEAST_EXPECT(lastTxnTime(env, alice) == t1);
        }

        // The review case: a bare integer above 32 bits. jsoncpp refused
        // these before canonicalization ever ran.
        {
            auto const t = paymentText(alice, bob, "5000000000", 0, nowMs(env));
            BEAST_EXPECT(
                engine(submitJson(env, t, jsonSign(alice, t))) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(
                env.balance(bob).value() == bobBalance + XRP(5002).value());
        }

        // compact, with the members in any order and any case
        {
            auto const t = std::string("{\"sigNingPubKey\":\"") +
                strHex(alice.pk().slice()) + "\",\"account\":\"" +
                alice.human() + "\",\"destination\":\"" + bob.human() +
                "\",\"amount\":\"1\",\"fee\":\"12\",\"sequence\":0,\"time\":"
                "\"" +
                jsontx_iso_str(nowMs(env)) +
                "\",\"transactiontype\":\"Payment\"}";
            BEAST_EXPECT(
                engine(submitJson(env, t, jsonSign(alice, t))) == "tesSUCCESS");
            env.close();
        }

        // With a Time, Sequence can be left out altogether: it is 0.
        {
            auto const before = env.seq(alice);
            auto const t = nowMs(env);
            auto const x = paymentText(alice, bob, "\"1\"", std::nullopt, t);
            BEAST_EXPECT(x.find("Sequence") == std::string::npos);
            auto const r = submitJson(env, x, jsonSign(alice, x));
            BEAST_EXPECT(engine(r) == "tesSUCCESS");
            BEAST_EXPECT(r[jss::tx_json][jss::Sequence] == 0);
            env.close();
            BEAST_EXPECT(env.seq(alice) == before);
            BEAST_EXPECT(lastTxnTime(env, alice) == t);
        }

        // So it can with a TicketSequence, which needs Sequence 0 too.
        {
            std::uint32_t const tkt = env.seq(alice) + 1;
            env(ticket::create(alice, 1));
            env.close();
            auto const x = paymentText(
                alice,
                bob,
                "\"1\"",
                std::nullopt,
                std::nullopt,
                "  \"TicketSequence\": " + std::to_string(tkt) + ",\n");
            BEAST_EXPECT(
                engine(submitJson(env, x, jsonSign(alice, x))) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(!env.le(keylet::ticket(alice, tkt)));
        }

        // binary-signed transactions may use sfTime too
        {
            auto const t = nowMs(env);
            env(withTime(noop(alice), t), seq(0));
            env.close();
            BEAST_EXPECT(lastTxnTime(env, alice) == t);
        }
    }

    void
    testSubmitErrors(FeatureBitset features)
    {
        testcase("submit { tx, sig } errors");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const carol{"carol", KeyType::ed25519};
        Account const dave{"dave", KeyType::secp256k1};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob, carol, dave);
        env.close();

        auto const now = nowMs(env);
        auto const text = paymentText(alice, bob, "\"1000000\"", 0, now);
        auto const good = jsonSign(alice, text);

        auto const failsWith = [&](Json::Value const& jr,
                                   std::string const& needle) {
            auto const e = engine(jr);
            return e.find(needle) != std::string::npos;
        };

        // half a JsonTx
        {
            Json::Value p;
            p[jss::tx] = text;
            BEAST_EXPECT(submitJson(env, p)[jss::error] == "invalidParams");
            Json::Value q;
            q[jss::sig] = strHex(good);
            BEAST_EXPECT(submitJson(env, q)[jss::error] == "invalidParams");
        }

        // two submission forms at once
        {
            Json::Value p;
            p[jss::tx] = text;
            p[jss::sig] = strHex(good);
            p[jss::tx_blob] = "00";
            BEAST_EXPECT(submitJson(env, p)[jss::error] == "invalidParams");
        }

        // the preimage must arrive as the signer's own text
        {
            Json::Value p;
            Json::Value obj;
            BEAST_EXPECT(Json::Reader{}.parse(text, obj));
            p[jss::tx] = obj;
            p[jss::sig] = strHex(good);
            BEAST_EXPECT(failsWith(submitJson(env, p), "must both be strings"));
        }

        // not hex
        {
            Json::Value p;
            p[jss::tx] = text;
            p[jss::sig] = "not hex";
            BEAST_EXPECT(failsWith(submitJson(env, p), "bad signature"));
        }

        // someone else's signature over the same text
        BEAST_EXPECT(failsWith(
            submitJson(env, text, jsonSign(carol, text)),
            "signature does not verify"));

        // Domain separation: a signature over the bare text - what a
        // wallet's "sign message" feature would hand back - is refused.
        BEAST_EXPECT(failsWith(
            submitJson(
                env, text, sign(alice.pk(), alice.sk(), makeSlice(text))),
            "signature does not verify"));

        // a signature over one byte less
        BEAST_EXPECT(failsWith(
            submitJson(env, text, jsonSign(alice, text + " ")),
            "signature does not verify"));

        // framing the signer might not see
        {
            auto const t = text + " // approve login";
            BEAST_EXPECT(
                failsWith(submitJson(env, t, jsonSign(alice, t)), "comments"));
        }
        {
            auto const t = paymentText(
                alice, bob, "\"1000000\"", 0, now, "  \"Fee\": \"13\",\n");
            BEAST_EXPECT(
                failsWith(submitJson(env, t, jsonSign(alice, t)), "duplicate"));
        }
        {
            auto const t = paymentText(alice, bob, "\"1\\u0030\"", 0, now);
            BEAST_EXPECT(
                failsWith(submitJson(env, t, jsonSign(alice, t)), "escape"));
        }

        // bare integers stop at 2^53 - 1; past that the value is a string
        {
            auto const t = paymentText(alice, bob, "9007199254740992", 0, now);
            BEAST_EXPECT(
                failsWith(submitJson(env, t, jsonSign(alice, t)), "2^53"));
        }

        // fields the signer cannot have had in front of them
        {
            auto const t = paymentText(
                alice,
                bob,
                "\"1000000\"",
                0,
                now,
                "  \"TxnSignature\": \"00\",\n");
            BEAST_EXPECT(failsWith(
                submitJson(env, t, jsonSign(alice, t)), "must not appear"));
        }

        // No Sequence and nothing else to sequence it: refused up front with a
        // clear reason rather than a template error, and never autofilled
        {
            auto const t =
                paymentText(alice, bob, "\"1\"", std::nullopt, std::nullopt);
            BEAST_EXPECT(failsWith(
                submitJson(env, t, jsonSign(alice, t)), "no Sequence"));
        }

        // the key must be in the preimage
        {
            auto const t = std::string("{\"TransactionType\":\"Payment\",") +
                "\"Account\":\"" + alice.human() + "\",\"Destination\":\"" +
                bob.human() +
                "\",\"Amount\":\"1\",\"Fee\":\"12\",\"Sequence\":0}";
            BEAST_EXPECT(failsWith(
                submitJson(env, t, jsonSign(alice, t)), "SigningPubKey"));
        }

        // only ed25519
        {
            auto const t = paymentText(dave, bob, "\"1000000\"", 0, now);
            BEAST_EXPECT(failsWith(
                submitJson(
                    env,
                    t,
                    sign(
                        dave.pk(),
                        dave.sk(),
                        makeSlice(jsontx_signing_data(t)))),
                "ed25519"));
        }

        // A valid signature proves only that the key signed the text. Tying
        // the key to the account is still Transactor::checkSingleSign's job.
        {
            std::string t = paymentText(alice, bob, "\"1000000\"", 0, now);
            auto const from = strHex(alice.pk().slice());
            auto const to = strHex(carol.pk().slice());
            t.replace(t.find(from), from.size(), to);
            BEAST_EXPECT(
                engine(submitJson(env, t, jsonSign(carol, t))) ==
                "tefBAD_AUTH");
        }

        // a Time that is not the strict toISOString spelling
        {
            std::string t = paymentText(alice, bob, "\"1000000\"", 0, now);
            auto const iso = jsontx_iso_str(now);
            t.replace(t.find(iso), iso.size(), iso.substr(0, 19) + "Z");
            BEAST_EXPECT(
                failsWith(submitJson(env, t, jsonSign(alice, t)), "Time"));
        }

        // after all of that, nothing applied
        env.close();
        BEAST_EXPECT(!lastTxnTime(env, alice));
    }

    void
    testReplay(FeatureBitset features)
    {
        testcase("sfTime replay protection");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const t = nowMs(env);
        auto const text = paymentText(alice, bob, "\"1000000\"", 0, t);
        auto const sig = jsonSign(alice, text);
        BEAST_EXPECT(engine(submitJson(env, text, sig)) == "tesSUCCESS");

        // the same transaction again in the same ledger: refused either as a
        // duplicate or, since the open ledger already recorded its Time, as
        // a replay
        {
            auto const e = engine(submitJson(env, text, sig));
            BEAST_EXPECT(e == "tefPAST_SEQ" || e == "tefALREADY");
        }
        env.close();

        // and in a later one: nothing but sfLastTxnTime stops it now
        BEAST_EXPECT(engine(submitJson(env, text, sig)) == "tefPAST_SEQ");

        // a different transaction with the same Time, or an earlier one
        for (auto const at : {t, t - 1})
        {
            auto const x = paymentText(alice, bob, "\"2\"", 0, at);
            BEAST_EXPECT(
                engine(submitJson(env, x, jsonSign(alice, x))) ==
                "tefPAST_SEQ");
        }

        // one millisecond later is fine
        {
            auto const x = paymentText(alice, bob, "\"2\"", 0, t + 1);
            BEAST_EXPECT(
                engine(submitJson(env, x, jsonSign(alice, x))) == "tesSUCCESS");
        }
        env.close();
        BEAST_EXPECT(lastTxnTime(env, alice) == t + 1);

        // binary-signed, time-sequenced: replaying the identical blob
        {
            auto const jt = env.jt(withTime(noop(alice), t + 2), seq(0));
            BEAST_EXPECT(engine(submitBlob(env, stx(jt))) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(engine(submitBlob(env, stx(jt))) == "tefPAST_SEQ");
        }

        // Several in one ledger, submitted oldest first, all apply ...
        {
            auto const base = nowMs(env) + 10;
            for (int i = 0; i < 3; ++i)
            {
                auto const x = paymentText(alice, bob, "\"3\"", 0, base + i);
                BEAST_EXPECT(
                    engine(submitJson(env, x, jsonSign(alice, x))) ==
                    "tesSUCCESS");
            }
            env.close();
            BEAST_EXPECT(lastTxnTime(env, alice) == base + 2);
        }

        // ... but a later Time applied first shuts out the earlier one.
        {
            auto const base = nowMs(env) + 10;
            auto const late = paymentText(alice, bob, "\"4\"", 0, base + 5);
            auto const early = paymentText(alice, bob, "\"4\"", 0, base);
            BEAST_EXPECT(
                engine(submitJson(env, late, jsonSign(alice, late))) ==
                "tesSUCCESS");
            BEAST_EXPECT(
                engine(submitJson(env, early, jsonSign(alice, early))) ==
                "tefPAST_SEQ");
            env.close();
        }

        // Sequence 0 without a Time is exactly what it always was
        env(noop(alice), seq(0), ter(tefPAST_SEQ));
    }

    void
    testWindow(FeatureBitset features)
    {
        testcase("sfTime validity window");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        // jtx's clock starts at the ripple epoch; get far enough past it that
        // the window arithmetic below cannot wrap
        while (nowMs(env) <= txTimeMaxAgeMs + txTimeMaxFutureMs)
            env.close();
        auto const now = nowMs(env);

        // older than txTimeMaxAgeMs: expired, as a past LastLedgerSequence
        env(withTime(noop(alice), now - txTimeMaxAgeMs - 1),
            seq(0),
            ter(tefMAX_LEDGER));
        // exactly at the edge is still inside
        env(withTime(noop(alice), now - txTimeMaxAgeMs), seq(0));

        // further ahead than txTimeMaxFutureMs: not yet, but retriable
        env(withTime(noop(alice), now + txTimeMaxFutureMs + 1),
            seq(0),
            ter(terPRE_SEQ));
        // at the edge is inside - and pushes sfLastTxnTime that far ahead
        env(withTime(noop(alice), now + txTimeMaxFutureMs), seq(0));
        BEAST_EXPECT(lastTxnTime(env, alice) == now + txTimeMaxFutureMs);

        // which is the bound on how long a fast clock can lock an account
        // out of time-sequenced transactions
        env(withTime(noop(alice), now + 1), seq(0), ter(tefPAST_SEQ));

        // a Time with no preimage spelling, or anywhere near the window
        env(withTime(noop(alice), jsontx_max_time + 1),
            seq(0),
            ter(temMALFORMED));
        env(withTime(noop(alice), ~std::uint64_t{0}),
            seq(0),
            ter(temMALFORMED));
        // near the top of the range, which must not overflow the window test
        env(withTime(noop(alice), jsontx_max_time), seq(0), ter(terPRE_SEQ));
        env.close();

        // The LastLedgerSequence stand-in: a transaction signed now and held
        // back can never apply once the network is past its window.
        Account const carol{"carol", KeyType::ed25519};
        env.fund(XRP(10000), carol);
        env.close();
        auto const heldTime = nowMs(env);
        auto const held = paymentText(carol, bob, "\"1\"", 0, heldTime);
        auto const heldSig = jsonSign(carol, held);
        while (nowMs(env) <= heldTime + txTimeMaxAgeMs)
            env.close();
        BEAST_EXPECT(engine(submitJson(env, held, heldSig)) == "tefMAX_LEDGER");
        BEAST_EXPECT(!lastTxnTime(env, carol));
    }

    void
    testSequencing(FeatureBitset features)
    {
        testcase("sfTime alongside Sequence and Tickets");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        // On a transaction with a Sequence, a Time is only the validity
        // window. The Sequence is its replay protection, so it neither checks
        // nor records sfLastTxnTime: a Time older than sfLastTxnTime, or one
        // already used, is no obstacle.
        auto const t = nowMs(env);
        auto const s = env.seq(alice);
        env(withTime(noop(alice), t + 5), seq(0));
        BEAST_EXPECT(lastTxnTime(env, alice) == t + 5);
        env(withTime(noop(alice), t));
        env(withTime(noop(alice), t));
        BEAST_EXPECT(env.seq(alice) == s + 2);
        BEAST_EXPECT(lastTxnTime(env, alice) == t + 5);

        // ... while a time-sequenced one is still held to it
        env(withTime(noop(alice), t + 5), seq(0), ter(tefPAST_SEQ));

        // ... and it still needs the right sequence
        env(withTime(noop(alice), t + 1), seq(s), ter(tefPAST_SEQ));
        env(withTime(noop(alice), t + 1), seq(s + 50), ter(terPRE_SEQ));

        // the same for a Ticket: it consumes the Ticket, and its Time is only
        // a window
        std::uint32_t const tkt = env.seq(alice) + 1;
        env(ticket::create(alice, 1));
        env.close();
        env(withTime(noop(alice), t + 2), ticket::use(tkt));
        env.close();
        BEAST_EXPECT(!env.le(keylet::ticket(alice, tkt)));
        BEAST_EXPECT(lastTxnTime(env, alice) == t + 5);

        // time-sequenced TicketCreate: tickets start at the (unconsumed)
        // account sequence, and the sequence moves past them
        {
            auto const before = env.seq(alice);
            env(withTime(ticket::create(alice, 2), t + 6), seq(0));
            env.close();
            BEAST_EXPECT(env.le(keylet::ticket(alice, before)));
            BEAST_EXPECT(env.le(keylet::ticket(alice, before + 1)));
            BEAST_EXPECT(env.seq(alice) == before + 2);
            // and ordinary sequencing carries on from there
            env(noop(alice));
            env.close();
        }

        // A tec claims the fee and still records the Time, so it cannot be
        // replayed to charge the fee again.
        {
            Account const nobody{"nobody"};
            auto const jt =
                env.jt(withTime(pay(alice, nobody, drops(1)), t + 10), seq(0));
            BEAST_EXPECT(
                engine(submitBlob(env, stx(jt))) == "tecNO_DST_INSUF_NATIVE");
            env.close();
            BEAST_EXPECT(lastTxnTime(env, alice) == t + 10);
            BEAST_EXPECT(engine(submitBlob(env, stx(jt))) == "tefPAST_SEQ");
        }

        // The window applies whatever sequences the transaction.
        env(withTime(noop(alice), nowMs(env) + txTimeMaxFutureMs + 1),
            ter(terPRE_SEQ));
    }

    void
    testObjectIds(FeatureBitset features)
    {
        testcase("objects created by time-sequenced transactions");
        using namespace jtx;
        using namespace std::chrono_literals;

        // Every time-sequenced transaction has Sequence 0. Objects keyed by
        // (account, sequence) would collide, and inserting over an existing
        // key is a LogicError on every node that applies the ledger. seqID()
        // keys them by transaction id instead.
        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        Account const gw{"gw"};
        env.fund(XRP(100000), alice, bob, gw);
        env.close();

        auto const owners = [&] {
            return env.le(alice)->getFieldU32(sfOwnerCount);
        };
        auto const t = nowMs(env);
        auto const before = owners();

        // two offers in the same ledger
        auto const o1 =
            env.jt(withTime(offer(alice, gw["USD"](10), XRP(10)), t), seq(0));
        auto const o2 = env.jt(
            withTime(offer(alice, gw["USD"](20), XRP(20)), t + 1), seq(0));
        BEAST_EXPECT(engine(submitBlob(env, stx(o1))) == "tesSUCCESS");
        BEAST_EXPECT(engine(submitBlob(env, stx(o2))) == "tesSUCCESS");
        env.close();
        BEAST_EXPECT(owners() == before + 2);
        BEAST_EXPECT(
            env.le(keylet::offer(alice.id(), stx(o1).getTransactionID())));
        BEAST_EXPECT(
            env.le(keylet::offer(alice.id(), stx(o2).getTransactionID())));
        BEAST_EXPECT(!env.le(keylet::offer(alice.id(), 0u)));

        // Offers keyed by transaction id have no sequence to cancel them by;
        // OfferCancel takes the offer's ledger index instead.
        {
            Json::Value jv;
            jv[jss::TransactionType] = jss::OfferCancel;
            jv[jss::Account] = alice.human();
            jv[sfOfferID.jsonName] = to_string(
                keylet::offer(alice.id(), stx(o1).getTransactionID()).key);
            env(jv);
            env.close();
            BEAST_EXPECT(
                !env.le(keylet::offer(alice.id(), stx(o1).getTransactionID())));
            BEAST_EXPECT(owners() == before + 1);
        }

        // two escrows, and one of them finished by EscrowID
        auto const e1 = env.jt(
            withTime(escrow(alice, bob, XRP(10)), t + 2),
            seq(0),
            finish_time(env.now() + 10s));
        auto const e2 = env.jt(
            withTime(escrow(alice, bob, XRP(10)), t + 3),
            seq(0),
            finish_time(env.now() + 10s));
        BEAST_EXPECT(engine(submitBlob(env, stx(e1))) == "tesSUCCESS");
        BEAST_EXPECT(engine(submitBlob(env, stx(e2))) == "tesSUCCESS");
        env.close();
        BEAST_EXPECT(owners() == before + 3);
        BEAST_EXPECT(!env.le(keylet::escrow(alice.id(), 0u)));
        auto const escrowKey =
            keylet::escrow(alice.id(), stx(e1).getTransactionID());
        BEAST_EXPECT(env.le(escrowKey));
        env.close();
        env.close();
        {
            Json::Value jv;
            jv[jss::TransactionType] = jss::EscrowFinish;
            jv[jss::Account] = bob.human();
            jv[jss::Owner] = alice.human();
            jv[sfEscrowID.jsonName] = to_string(escrowKey.key);
            env(jv);
            env.close();
            BEAST_EXPECT(!env.le(escrowKey));
            BEAST_EXPECT(owners() == before + 2);
        }

        // two NFTs, with FirstNFTokenSequence taken from the untouched
        // account sequence as for a Ticket
        auto const acctSeq = env.seq(alice);
        env(withTime(token::mint(alice, 0), t + 4), seq(0));
        env(withTime(token::mint(alice, 0), t + 5), seq(0));
        env.close();
        BEAST_EXPECT(
            env.le(alice)->getFieldU32(sfFirstNFTokenSequence) == acctSeq);
        BEAST_EXPECT(env.le(alice)->getFieldU32(sfMintedNFTokens) == 2);
        BEAST_EXPECT(env.seq(alice) == acctSeq);
    }

    void
    testDenied(FeatureBitset features)
    {
        testcase("transactions that may not be time-sequenced");
        using namespace jtx;

        // Each type below checks its own amendment before preflight1, so all
        // of them must be on for the refusals under test to be reached.
        auto const all = features | featureMPTokensV1 |
            featurePermissionedDomains | featureCredentials |
            featureOnChainManifests;
        Env env{*this, all};
        Account const alice{"alice", KeyType::ed25519};
        env.fund(XRP(10000), alice);
        env.close();
        auto const now = nowMs(env);

        // keyed by the raw sequence, not seqID()
        {
            Json::Value jv;
            jv[jss::TransactionType] = "MPTokenIssuanceCreate";
            jv[jss::Account] = alice.human();
            env(withTime(jv, now), seq(0), ter(temBAD_SEQUENCE));
            // with a sequence it is still fine
            env(withTime(jv, now));
        }

        // the same for a permissioned domain, keyed (account, sequence) too
        {
            Account const issuer{"issuer"};
            env.fund(XRP(1000), issuer);
            env.close();
            auto const jv = pdomain::setTx(
                alice.id(), pdomain::Credentials{{issuer, "abcd"}});
            env(withTime(jv, nowMs(env)), seq(0), ter(temBAD_SEQUENCE));
        }

        // Sequence 0 already means something else for these
        auto const pinned = [&](TxType type) {
            return std::make_shared<STTx const>(type, [&](STObject& o) {
                o.setAccountID(sfAccount, alice.id());
                o.setFieldU32(sfSequence, 0);
                o.setFieldAmount(sfFee, XRP(1));
                o.setFieldVL(sfSigningPubKey, Blob{});
                o.setFieldVL(sfTxnSignature, Blob{});
                o.setFieldU64(sfTime, now);
            });
        };
        BEAST_EXPECT(applyDirect(env, pinned(ttMANIFEST_SET)) == temMALFORMED);

        // Emitted transactions carry their own replay protection. Checked
        // in preflight1, ahead of the emitted-transaction bypass.
        {
            auto const tx =
                std::make_shared<STTx const>(ttACCOUNT_SET, [&](STObject& o) {
                    o.setAccountID(sfAccount, alice.id());
                    o.setFieldU32(sfSequence, 0);
                    o.setFieldAmount(sfFee, XRP(1));
                    o.setFieldVL(sfSigningPubKey, Blob{});
                    o.makeFieldPresent(sfEmitDetails);
                    o.setFieldU64(sfTime, now);
                });
            BEAST_EXPECT(applyDirect(env, tx) == temMALFORMED);
        }

        // Pseudo-transactions skip preflight1; Change and Cron check for
        // themselves.
        {
            auto const tx =
                std::make_shared<STTx const>(ttAMENDMENT, [&](STObject& o) {
                    o.setAccountID(sfAccount, AccountID());
                    o.setFieldAmount(sfFee, STAmount());
                    o.setFieldVL(sfSigningPubKey, Blob{});
                    o.setFieldU32(sfSequence, 0);
                    o.setFieldH256(sfAmendment, featureJsonTx);
                    o.setFieldU32(sfLedgerSequence, env.current()->seq());
                    o.setFieldU64(sfTime, now);
                });
            BEAST_EXPECT(applyDirect(env, tx) == temMALFORMED);
        }
        for (SField const* f :
             {static_cast<SField const*>(&sfTime),
              static_cast<SField const*>(&sfJsonTxDelta)})
        {
            auto const tx =
                std::make_shared<STTx const>(ttCRON, [&](STObject& o) {
                    o.setAccountID(sfAccount, AccountID());
                    o.setFieldAmount(sfFee, STAmount());
                    o.setFieldVL(sfSigningPubKey, Blob{});
                    o.setFieldU32(sfSequence, 0);
                    o.setAccountID(sfOwner, alice.id());
                    o.setFieldU32(sfLedgerSequence, env.current()->seq());
                    if (*f == sfTime)
                        o.setFieldU64(sfTime, now);
                    else
                        o.setFieldVL(sfJsonTxDelta, Slice("\x01\x00\x04", 3));
                });
            BEAST_EXPECT(applyDirect(env, tx) == temMALFORMED);
        }

        // oversize delta is refused before anything tries to decode it
        {
            auto const tx = mutate(stx(env.jt(noop(alice))), [](STObject& o) {
                o.setFieldVL(sfJsonTxDelta, Blob(jsontx_max_diff + 1, 0));
            });
            BEAST_EXPECT(
                applyDirect(env, std::make_shared<STTx const>(tx)) ==
                temMALFORMED);
        }
    }

    void
    testSequenceKeyedObjects(FeatureBitset features)
    {
        testcase("objects keyed by sequence: time, emitted and tickets");
        using namespace jtx;

        auto const all = features | featureMPTokensV1 |
            featurePermissionedDomains | featureCredentials;

        // Everything created through seqID() is keyed by transaction id when
        // the SeqProxy is sequence(0), so two in one ledger never collide.
        {
            Env env{*this, all};
            Account const alice{"alice", KeyType::ed25519};
            Account const bob{"bob"};
            env.fund(XRP(100000), alice, bob);
            env.close();
            auto const t = nowMs(env);

            // payment channels
            auto const chan = [&](std::uint64_t at) {
                Json::Value jv;
                jv[jss::TransactionType] = jss::PaymentChannelCreate;
                jv[jss::Account] = alice.human();
                jv[jss::Destination] = bob.human();
                jv[jss::Amount] = XRP(10).value().getJson(JsonOptions::none);
                jv[sfSettleDelay.jsonName] = 100;
                jv[sfPublicKey.jsonName] = strHex(alice.pk().slice());
                return env.jt(withTime(jv, at), seq(0));
            };
            auto const c1 = chan(t), c2 = chan(t + 1);
            BEAST_EXPECT(engine(submitBlob(env, stx(c1))) == "tesSUCCESS");
            BEAST_EXPECT(engine(submitBlob(env, stx(c2))) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(env.le(keylet::payChan(
                alice.id(), bob.id(), stx(c1).getTransactionID())));
            BEAST_EXPECT(env.le(keylet::payChan(
                alice.id(), bob.id(), stx(c2).getTransactionID())));
            BEAST_EXPECT(!env.le(keylet::payChan(alice.id(), bob.id(), 0u)));

            // NFToken offers
            auto const nft = token::getNextID(env, alice, 0, tfTransferable);
            env(token::mint(alice, 0), txflags(tfTransferable));
            env.close();
            auto const o1 = env.jt(
                withTime(token::createOffer(alice, nft, XRP(1)), t + 2),
                seq(0),
                txflags(tfSellNFToken));
            auto const o2 = env.jt(
                withTime(token::createOffer(alice, nft, XRP(2)), t + 3),
                seq(0),
                txflags(tfSellNFToken));
            BEAST_EXPECT(engine(submitBlob(env, stx(o1))) == "tesSUCCESS");
            BEAST_EXPECT(engine(submitBlob(env, stx(o2))) == "tesSUCCESS");
            env.close();
            BEAST_EXPECT(env.le(
                keylet::nftoffer(alice.id(), stx(o1).getTransactionID())));
            BEAST_EXPECT(env.le(
                keylet::nftoffer(alice.id(), stx(o2).getTransactionID())));
        }

        // The two raw-sequence creators refuse an emitted transaction under
        // fix20260929. Before it the emitted-transaction path is reached as
        // it always was (and refuses this one for not coming out of the
        // emission directory).
        {
            auto const emitted = [](AccountID const& a, TxType type) {
                return std::make_shared<STTx const>(type, [&](STObject& o) {
                    o.setAccountID(sfAccount, a);
                    o.setFieldU32(sfSequence, 0);
                    o.setFieldAmount(sfFee, XRP(1));
                    o.setFieldVL(sfSigningPubKey, Blob{});
                    o.makeFieldPresent(sfEmitDetails);
                });
            };
            for (auto const type :
                 {ttMPTOKEN_ISSUANCE_CREATE, ttPERMISSIONED_DOMAIN_SET})
            {
                {
                    Env env{*this, all};
                    Account const alice{"alice"};
                    env.fund(XRP(10000), alice);
                    env.close();
                    BEAST_EXPECT(
                        applyDirect(env, emitted(alice.id(), type)) ==
                        temBAD_SEQUENCE);
                }
                {
                    Env env{*this, all - fix20260929};
                    Account const alice{"alice"};
                    env.fund(XRP(10000), alice);
                    env.close();
                    BEAST_EXPECT(
                        applyDirect(env, emitted(alice.id(), type)) ==
                        telNON_LOCAL_EMITTED_TXN);
                }
            }
        }

        // Tickets. MPTokenIssuanceCreate always took the Ticket number;
        // PermissionedDomainSet took the raw Sequence - 0 for every ticketed
        // transaction - until fix20260929. (The unfixed path is not run
        // here: its second domain collides with the first, which is a
        // LogicError when the ledger is built.)
        {
            Env env{*this, all};
            Account const alice{"alice"};
            Account const issuer{"issuer"};
            env.fund(XRP(100000), alice, issuer);
            env.close();

            std::uint32_t const first = env.seq(alice) + 1;
            env(ticket::create(alice, 4));
            env.close();

            Json::Value mpt;
            mpt[jss::TransactionType] = "MPTokenIssuanceCreate";
            mpt[jss::Account] = alice.human();
            env(mpt, ticket::use(first));
            env(mpt, ticket::use(first + 1));
            env.close();
            BEAST_EXPECT(env.le(keylet::mptIssuance(first, alice.id())));
            BEAST_EXPECT(env.le(keylet::mptIssuance(first + 1, alice.id())));

            auto const pd = pdomain::setTx(
                alice.id(), pdomain::Credentials{{issuer, "abcd"}});
            env(pd, ticket::use(first + 2));
            env(pd, ticket::use(first + 3));
            env.close();
            for (auto const tkt : {first + 2, first + 3})
            {
                auto const sle =
                    env.le(keylet::permissionedDomain(alice.id(), tkt));
                if (BEAST_EXPECT(sle))
                    BEAST_EXPECT(sle->getFieldU32(sfSequence) == tkt);
            }
            BEAST_EXPECT(!env.le(keylet::permissionedDomain(alice.id(), 0)));
        }
    }

    void
    testAccountDelete(FeatureBitset features)
    {
        testcase("AccountDelete and sfLastTxnTime");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        auto const reserve = drops(env.current()->fees().increment);
        auto const closeUntilSeqOk = [&] {
            while (env.seq(alice) + 255 + 2 > env.current()->seq())
                env.close();
        };
        closeUntilSeqOk();

        // A time-sequenced AccountDelete is always refused: it is still
        // inside its own window when it applies, so once the account is
        // re-created it could be applied again.
        env(withTime(acctdelete(alice, bob), nowMs(env)),
            seq(0),
            fee(reserve),
            ter(temBAD_SEQUENCE));

        // A recent Time on the account holds deletion off until every
        // transaction that used one is expired for good.
        auto const noopAt = env.jt(withTime(noop(alice), nowMs(env)), seq(0));
        BEAST_EXPECT(engine(submitBlob(env, stx(noopAt))) == "tesSUCCESS");
        env.close();
        auto const last = *lastTxnTime(env, alice);
        env(acctdelete(alice, bob), fee(reserve), ter(tecTOO_SOON));
        env.close();

        while (nowMs(env) <= last + txTimeMaxAgeMs)
            env.close();
        closeUntilSeqOk();
        env(acctdelete(alice, bob), fee(reserve));
        env.close();
        BEAST_EXPECT(!env.le(alice));

        // Re-create the account. It has no sfLastTxnTime, and the old
        // transaction still cannot apply: it expired before deletion could.
        env(pay(bob, alice, XRP(1000)));
        env.close();
        BEAST_EXPECT(env.le(alice));
        BEAST_EXPECT(!lastTxnTime(env, alice));
        BEAST_EXPECT(engine(submitBlob(env, stx(noopAt))) == "tefMAX_LEDGER");
    }

    void
    testQueue(FeatureBitset features)
    {
        testcase("time-sequenced transactions and the TxQ");
        using namespace jtx;

        auto cfg = envconfig();
        auto& q = cfg->section("transaction_queue");
        q.set("minimum_txn_in_ledger_standalone", "1");
        q.set("ledgers_in_queue", "2");
        q.set("minimum_queue_size", "2");
        Env env{*this, std::move(cfg), features};

        // One account per ledger: each fund is a Payment and an AccountSet,
        // and a third transaction in the open ledger would already escalate.
        Account const alice{"alice", KeyType::ed25519};
        Account const carol{"carol"};
        env.fund(XRP(10000), alice);
        env.close();
        env.fund(XRP(10000), carol);
        env.close();

        // push the open ledger into fee escalation
        bool escalated = false;
        for (int i = 0; i < 10 && !escalated; ++i)
            escalated = engine(submitBlob(env, stx(env.jt(noop(carol))))) ==
                "terQUEUED";
        if (!BEAST_EXPECT(escalated))
            return;

        // At the base fee a time-sequenced transaction cannot be queued:
        // the queue is keyed by SeqProxy and every one of them is sequence(0).
        auto const t = nowMs(env);
        BEAST_EXPECT(
            engine(submitBlob(
                env, stx(env.jt(withTime(noop(alice), t), seq(0))))) ==
            "telCAN_NOT_QUEUE");
        BEAST_EXPECT(!lastTxnTime(env, alice));

        // Paying the escalated fee, it applies directly. It is not exempt
        // from escalation the way a manifest is.
        BEAST_EXPECT(
            engine(submitBlob(
                env,
                stx(env.jt(
                    withTime(noop(alice), t + 1), seq(0), fee(XRP(1)))))) ==
            "tesSUCCESS");
        BEAST_EXPECT(lastTxnTime(env, alice) == t + 1);
    }

    void
    testCanonicalOrder()
    {
        testcase("CanonicalTXSet orders time-sequenced transactions by Time");

        auto const make = [](std::uint32_t seq,
                             std::optional<std::uint64_t> time) {
            return std::make_shared<STTx const>(
                ttACCOUNT_SET, [&](STObject& o) {
                    o.setAccountID(sfAccount, AccountID(7));
                    o.setFieldU32(sfSequence, seq);
                    o.setFieldAmount(sfFee, XRPAmount(10));
                    o.setFieldVL(sfSigningPubKey, Blob{});
                    if (time)
                        o.setFieldU64(sfTime, *time);
                });
        };

        CanonicalTXSet set{uint256{}};
        auto const late = make(0, 300), early = make(0, 100),
                   mid = make(0, 200);
        auto const bySeq = make(5, std::nullopt);
        for (auto const& tx : {late, bySeq, early, mid})
            set.insert(tx);

        std::vector<std::shared_ptr<STTx const>> order;
        for (auto const& [k, tx] : set)
            order.push_back(tx);
        BEAST_EXPECT(order.size() == 4);
        if (order.size() == 4)
        {
            // after everything sequenced by a Sequence or a Ticket; then
            // oldest Time first, the only order in which all can apply
            BEAST_EXPECT(order[0] == bySeq);
            BEAST_EXPECT(order[1] == early);
            BEAST_EXPECT(order[2] == mid);
            BEAST_EXPECT(order[3] == late);
        }

        // and the next one after an applied transaction is the next Time
        CanonicalTXSet held{uint256{}};
        held.insert(mid);
        held.insert(late);
        BEAST_EXPECT(held.popAcctTransaction(early) == mid);
        BEAST_EXPECT(held.popAcctTransaction(mid) == late);

        // Ordering of transactions without a Time is untouched.
        CanonicalTXSet plain{uint256{}};
        auto const s1 = make(1, std::nullopt), s2 = make(2, std::nullopt);
        plain.insert(s2);
        plain.insert(s1);
        BEAST_EXPECT(plain.begin()->second == s1);

        // Sequences, then Tickets, then time-sequenced transactions oldest
        // first. A Time on one with a Sequence or a Ticket changes nothing.
        auto const makeTicket = [](std::uint32_t ticket,
                                   std::optional<std::uint64_t> time) {
            return std::make_shared<STTx const>(
                ttACCOUNT_SET, [&](STObject& o) {
                    o.setAccountID(sfAccount, AccountID(7));
                    o.setFieldU32(sfSequence, 0);
                    o.setFieldU32(sfTicketSequence, ticket);
                    o.setFieldAmount(sfFee, XRPAmount(10));
                    o.setFieldVL(sfSigningPubKey, Blob{});
                    if (time)
                        o.setFieldU64(sfTime, *time);
                });
        };
        auto const seq6Timed = make(6, 150);
        auto const tkt8Timed = makeTicket(8, 250);
        auto const tkt9 = makeTicket(9, std::nullopt);

        CanonicalTXSet mixed{uint256{}};
        for (auto const& tx :
             {late, tkt9, seq6Timed, early, tkt8Timed, bySeq, mid, s1})
            mixed.insert(tx);
        std::vector<std::shared_ptr<STTx const>> got;
        for (auto const& [k, tx] : mixed)
            got.push_back(tx);
        std::vector<std::shared_ptr<STTx const>> const want{
            s1, bySeq, seq6Timed, tkt8Timed, tkt9, early, mid, late};
        BEAST_EXPECT(got == want);

        // After a sequenced transaction, Time or not, the next Sequence is
        // offered first: that is what it can have unblocked.
        {
            auto const seq7 = make(7, std::nullopt);
            CanonicalTXSet heldSeq{uint256{}};
            heldSeq.insert(late);
            heldSeq.insert(seq7);
            BEAST_EXPECT(heldSeq.popAcctTransaction(seq6Timed) == seq7);
            BEAST_EXPECT(heldSeq.popAcctTransaction(seq6Timed) == late);
        }
    }

    // The open ledger applies transactions in arrival order, consensus in
    // CanonicalTXSet order, so whatever the open ledger accepted together
    // must apply together in that order too. Z01 to Z03 are the
    // reproductions from the 2026-10-07 review; the reversed pairs hold the
    // fix to both arrival orders.
    void
    testConsensusOrder(FeatureBitset features)
    {
        testcase("consensus applies what the open ledger accepted");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice, bob);
        env.close();

        // strictly increasing, and never behind the open ledger
        std::uint64_t t = nowMs(env);
        auto const nextTime = [&]() {
            auto const now = nowMs(env);
            t = now > t + 10 ? now : t + 10;
            return t;
        };

        // submits jt, which the open ledger must accept, and returns its id
        auto const openAccept = [&](JTx const& jt) {
            BEAST_EXPECT(engine(submitBlob(env, stx(jt))) == "tesSUCCESS");
            return jt.stx->getTransactionID();
        };

        // closes the ledger, which must hold both
        auto const closeWith = [&](uint256 const& a, uint256 const& b) {
            env.close();
            BEAST_EXPECT(env.closed()->txExists(a));
            BEAST_EXPECT(env.closed()->txExists(b));
        };

        // Z01: a sequenced transaction with a Time, then a time-sequenced
        // one with a later Time
        {
            auto const s = env.seq(alice);
            auto const a =
                openAccept(env.jt(withTime(noop(alice), nextTime())));
            auto const tb = nextTime();
            auto const b =
                openAccept(env.jt(withTime(noop(alice), tb), seq(0)));
            closeWith(a, b);
            BEAST_EXPECT(env.seq(alice) == s + 1);
            BEAST_EXPECT(lastTxnTime(env, alice) == tb);
        }

        // reversed: a sequenced transaction with a later Time must not
        // shut out the time-sequenced one before it
        {
            auto const s = env.seq(alice);
            auto const ta = nextTime();
            auto const a =
                openAccept(env.jt(withTime(noop(alice), ta), seq(0)));
            auto const b =
                openAccept(env.jt(withTime(noop(alice), nextTime())));
            closeWith(a, b);
            BEAST_EXPECT(env.seq(alice) == s + 1);
            BEAST_EXPECT(lastTxnTime(env, alice) == ta);
        }

        // A sequenced transaction's Time is only a window, so one older than
        // sfLastTxnTime still applies, in either order.
        {
            auto const s = env.seq(alice);
            auto const tOld = nextTime();
            auto const tb = nextTime();
            auto const a =
                openAccept(env.jt(withTime(noop(alice), tb), seq(0)));
            auto const b = openAccept(env.jt(withTime(noop(alice), tOld)));
            closeWith(a, b);
            BEAST_EXPECT(env.seq(alice) == s + 1);
            BEAST_EXPECT(lastTxnTime(env, alice) == tb);
        }

        // Z02: as Z01, with a Ticket in place of the Sequence
        {
            std::uint32_t const tkt = env.seq(alice) + 1;
            env(ticket::create(alice, 1));
            env.close();
            auto const a = openAccept(
                env.jt(withTime(noop(alice), nextTime()), ticket::use(tkt)));
            auto const tb = nextTime();
            auto const b =
                openAccept(env.jt(withTime(noop(alice), tb), seq(0)));
            closeWith(a, b);
            BEAST_EXPECT(!env.le(keylet::ticket(alice, tkt)));
            BEAST_EXPECT(lastTxnTime(env, alice) == tb);
        }

        // Z03: a plain Payment, then a time-sequenced TicketCreate, whose
        // Tickets must get the numbers the open ledger gave them
        {
            auto const s = env.seq(alice);
            auto const a = openAccept(env.jt(pay(alice, bob, XRP(1))));
            auto const b = openAccept(
                env.jt(withTime(ticket::create(alice, 2), nextTime()), seq(0)));
            closeWith(a, b);
            BEAST_EXPECT(env.le(keylet::ticket(alice, s + 1)));
            BEAST_EXPECT(env.le(keylet::ticket(alice, s + 2)));
            BEAST_EXPECT(env.seq(alice) == s + 3);
        }

        // reversed: the Payment, sequenced after the Tickets, fails
        // terPRE_SEQ ahead of the TicketCreate and applies on the retry
        {
            auto const s = env.seq(alice);
            auto const a = openAccept(
                env.jt(withTime(ticket::create(alice, 2), nextTime()), seq(0)));
            auto const b = openAccept(env.jt(pay(alice, bob, XRP(1))));
            closeWith(a, b);
            BEAST_EXPECT(env.le(keylet::ticket(alice, s)));
            BEAST_EXPECT(env.le(keylet::ticket(alice, s + 1)));
            BEAST_EXPECT(env.seq(alice) == s + 3);
        }
    }

    void
    testLocalTxs(FeatureBitset features)
    {
        testcase("LocalTxs keeps a time-sequenced transaction until spent");
        using namespace jtx;

        Env env{*this, features};
        Account const alice{"alice", KeyType::ed25519};
        env.fund(XRP(10000), alice);
        env.close();

        auto const t = nowMs(env);
        auto const jt = env.jt(withTime(noop(alice), t), seq(0));
        auto const tx = std::make_shared<STTx const>(stx(jt));

        auto local = make_LocalTxs();
        local->push_back(env.closed()->seq(), tx);

        // SeqProxy sequence(0) is below the account sequence; before this
        // change the sweep dropped it at once.
        local->sweep(*env.closed());
        BEAST_EXPECT(local->size() == 1);

        BEAST_EXPECT(engine(submitBlob(env, *tx)) == "tesSUCCESS");
        env.close();
        local->sweep(*env.closed());
        BEAST_EXPECT(local->size() == 0);
    }

    void
    run() override
    {
        using namespace jtx;
        auto const all = supported_amendments();

        testDisabled(all);
        testActivationBoundary(all);
        testSubmit(all);
        testSubmitErrors(all);
        testReplay(all);
        testWindow(all);
        testSequencing(all);
        testObjectIds(all);
        testDenied(all);
        testSequenceKeyedObjects(all);
        testAccountDelete(all);
        testQueue(all);
        testCanonicalOrder();
        testConsensusOrder(all);
        testLocalTxs(all);
    }
};

BEAST_DEFINE_TESTSUITE(JsonTx, app, ripple);

}  // namespace test
}  // namespace ripple
