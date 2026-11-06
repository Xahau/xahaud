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
#include <xrpld/core/Config.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/protocol/CreditNote.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/JSONTxSignatures.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/jss.h>

#include <cctype>
#include <chrono>
#include <optional>
#include <string>

namespace ripple {
namespace test {

struct CreditNote_test : public beast::unit_test::suite
{
    //--------------------------------------------------------------------------
    // helpers

    // The open ledger's parent close time in milliseconds since the ripple
    // epoch: the clock NoteClaim checks the note's window against. A fresh
    // Env starts seconds after the epoch, too early to backdate a note from
    // (the subtraction would wrap), so a young clock is first moved an hour on.
    static std::uint64_t
    nowMs(jtx::Env& env)
    {
        using namespace std::chrono_literals;
        if (env.current()->parentCloseTime().time_since_epoch() < 1h)
            env.close(1h);
        return static_cast<std::uint64_t>(env.current()
                                              ->parentCloseTime()
                                              .time_since_epoch()
                                              .count()) *
            1000;
    }

    static std::optional<std::uint64_t>
    lastNoteTime(jtx::Env& env, jtx::Account const& a)
    {
        auto const sle = env.le(a);
        if (!sle || !sle->isFieldPresent(sfLastNoteTime))
            return std::nullopt;
        return sle->getFieldU64(sfLastNoteTime);
    }

    // A bearer key: a jtx::Account is a convenient ed25519 key pair that is
    // never funded. The issuer makes one up per note; its public half goes
    // in the note and its secret goes to the recipient.
    static jtx::Account
    bearerKey(std::string const& name = "bearer")
    {
        return jtx::Account{name, KeyType::ed25519};
    }

    static std::string
    bearerHex(jtx::Account const& bearer)
    {
        return strHex(bearer.pk().slice());
    }

    // The note laid out the way a browser's JSON.stringify would.
    static std::string
    noteText(
        jtx::Env& env,
        jtx::Account const& issuer,
        std::string const& amount,
        std::uint64_t issueTime,
        std::uint64_t expiration,
        jtx::Account const& bearer)
    {
        return "{\"NetworkID\":" +
            std::to_string(env.app().config().NETWORK_ID) + ",\"Account\":\"" +
            issuer.human() + "\",\"Amount\":" + amount + ",\"IssueTime\":\"" +
            jsontx_iso_str(issueTime) + "\",\"Expiration\":\"" +
            jsontx_iso_str(expiration) + "\",\"Bearer\":\"" +
            bearerHex(bearer) + "\"}";
    }

    static std::string
    iou(jtx::IOU const& currency, std::string const& value)
    {
        return "{\"currency\":\"" + to_string(currency.currency) +
            "\",\"issuer\":\"" + currency.account.human() + "\",\"value\":\"" +
            value + "\"}";
    }

    static Buffer
    noteSign(jtx::Account const& signer, std::string const& text)
    {
        return sign(
            signer.pk(), signer.sk(), makeSlice(jsontx_signing_data(text)));
    }

    // The bearer key's proof for `claimant` redeeming `text`.
    static Buffer
    bearerSign(
        jtx::Account const& bearer,
        std::string const& text,
        jtx::Account const& claimant)
    {
        return sign(
            bearer.pk(),
            bearer.sk(),
            makeSlice(noteClaimSigningData(text, claimant.id())));
    }

    // A NoteClaim from `claimant` carrying `text`, signed by `signer`'s key,
    // with the bearer proof by `bearer`.
    static Json::Value
    claim(
        jtx::Account const& claimant,
        std::string const& text,
        jtx::Account const& signer,
        jtx::Account const& bearer)
    {
        Json::Value jv;
        jv[jss::TransactionType] = "NoteClaim";
        jv[jss::Account] = claimant.human();
        jv[sfNote.fieldName] = strHex(text);
        jv[sfNoteSignature.fieldName] = strHex(noteSign(signer, text));
        jv[sfPublicKey.fieldName] = strHex(signer.pk().slice());
        jv[sfBearerSignature.fieldName] =
            strHex(bearerSign(bearer, text, claimant));
        return jv;
    }

    // A fresh ten-minute note issued a minute ago.
    static std::string
    freshNote(
        jtx::Env& env,
        jtx::Account const& issuer,
        std::string const& amount,
        jtx::Account const& bearer,
        std::int64_t offsetMs = 0)
    {
        auto const issue = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(nowMs(env)) - 60'000 + offsetMs);
        return noteText(env, issuer, amount, issue, issue + 600'000, bearer);
    }

    //--------------------------------------------------------------------------

    void
    testParse()
    {
        testcase("parse");
        using namespace jtx;
        Env env{*this, supported_amendments()};
        Account const alice{"alice", KeyType::ed25519};
        Account const gw{"gw"};
        auto const USD = gw["USD"];
        auto const key = bearerKey();

        auto const ok = [&](std::string const& text) {
            try
            {
                parseCreditNote(text);
                return true;
            }
            catch (std::exception const&)
            {
                return false;
            }
        };

        std::uint64_t const t = 800'000'000'000ULL;
        auto const good = noteText(env, alice, "10000000", t, t + 600'000, key);

        BEAST_EXPECT(ok(good));
        {
            auto const n = parseCreditNote(good);
            BEAST_EXPECT(n.account == alice.id());
            BEAST_EXPECT(n.amount == STAmount{XRPAmount{10'000'000}});
            BEAST_EXPECT(n.issueTime == t);
            BEAST_EXPECT(n.expiration == t + 600'000);
            BEAST_EXPECT(n.bearer == key.pk());
        }

        // whitespace is the issuer's to choose; the bytes are what is signed
        BEAST_EXPECT(
            ok(" {\n \"NetworkID\": 0 ,\n\"Account\":\"" + alice.human() +
               "\",\"Amount\":1,\"IssueTime\":\"" + jsontx_iso_str(t) +
               "\",\"Expiration\":\"" + jsontx_iso_str(t + 1) +
               "\" , \"Bearer\" : \"" + bearerHex(key) + "\" }\n"));

        // issued amounts
        BEAST_EXPECT(
            ok(noteText(env, alice, iou(USD, "10"), t, t + 600'000, key)));
        BEAST_EXPECT(
            ok(noteText(env, alice, iou(USD, "0.5"), t, t + 600'000, key)));
        BEAST_EXPECT(
            !ok(noteText(env, alice, iou(USD, "0"), t, t + 600'000, key)));
        BEAST_EXPECT(
            !ok(noteText(env, alice, iou(USD, "-1"), t, t + 600'000, key)));
        BEAST_EXPECT(!ok(noteText(
            env,
            alice,
            "{\"currency\":\"USD\",\"issuer\":\"" + gw.human() + "\"}",
            t,
            t + 600'000,
            key)));
        BEAST_EXPECT(!ok(noteText(
            env,
            alice,
            "{\"currency\":\"XAH\",\"issuer\":\"" + gw.human() +
                "\",\"value\":\"1\"}",
            t,
            t + 600'000,
            key)));

        // shape
        BEAST_EXPECT(!ok(noteText(env, alice, "0", t, t + 600'000, key)));
        BEAST_EXPECT(!ok(noteText(env, alice, "-5", t, t + 600'000, key)));
        BEAST_EXPECT(!ok(noteText(env, alice, "\"10\"", t, t + 600'000, key)));
        BEAST_EXPECT(!ok(noteText(env, alice, "1.5", t, t + 600'000, key)));
        BEAST_EXPECT(!ok(""));
        BEAST_EXPECT(!ok("[]"));
        BEAST_EXPECT(!ok(good + " x"));
        BEAST_EXPECT(!ok(good.substr(0, good.size() - 1)));

        // exactly the six members, spelled exactly so
        BEAST_EXPECT(
            !ok(good.substr(0, good.size() - 1) + ",\"Memo\":\"hi\"}"));
        BEAST_EXPECT(
            !ok(good.substr(0, good.size() - 1) +
                ",\"TransactionType\":\"Payment\"}"));
        {
            auto bad = good;
            bad.replace(bad.find("\"Account\""), 9, "\"account\"");
            BEAST_EXPECT(!ok(bad));
        }
        {
            auto bad = good;
            bad.replace(bad.find("\"IssueTime\""), 11, "\"Expiration\"");
            BEAST_EXPECT(!ok(bad));  // duplicate Expiration
        }

        // window
        BEAST_EXPECT(!ok(noteText(env, alice, "1", t, t, key)));
        BEAST_EXPECT(!ok(noteText(env, alice, "1", t + 1, t, key)));
        BEAST_EXPECT(
            ok(noteText(env, alice, "1", t, t + creditNoteMaxLifeMs, key)));
        BEAST_EXPECT(!ok(
            noteText(env, alice, "1", t, t + creditNoteMaxLifeMs + 1, key)));

        // a time the way sfTime renders, or nothing
        BEAST_EXPECT(
            !ok("{\"NetworkID\":0,\"Account\":\"" + alice.human() +
                "\",\"Amount\":1,\"IssueTime\":" + std::to_string(t) +
                ",\"Expiration\":\"" + jsontx_iso_str(t + 1) +
                "\",\"Bearer\":\"" + bearerHex(key) + "\"}"));

        // the bearer key: an ed25519 public key in hex, either case
        {
            auto const withBearer = [&](std::string const& b) {
                auto n = good;
                auto const at = n.find("\"Bearer\":\"");
                auto const end = n.find('"', at + 10);
                return n.replace(at + 10, end - (at + 10), b);
            };
            auto lower = bearerHex(key);
            for (auto& c : lower)
                c = static_cast<char>(std::tolower(c));
            BEAST_EXPECT(ok(withBearer(lower)));
            BEAST_EXPECT(parseCreditNote(withBearer(lower)).bearer == key.pk());

            // secp256k1
            BEAST_EXPECT(!ok(withBearer(strHex(Account{"k1"}.pk().slice()))));
            // not hex, odd length, too short, too long, empty, missing
            BEAST_EXPECT(!ok(withBearer("ED" + std::string(64, 'G'))));
            BEAST_EXPECT(!ok(withBearer(bearerHex(key) + "0")));
            BEAST_EXPECT(!ok(withBearer(bearerHex(key).substr(0, 64))));
            BEAST_EXPECT(!ok(withBearer(bearerHex(key) + "00")));
            BEAST_EXPECT(!ok(withBearer("")));
            {
                auto n = good;
                auto const at = n.find(",\"Bearer\"");
                n.erase(at, n.find('"', at + 11) + 1 - at);
                BEAST_EXPECT(!ok(n));
            }
            // a number, not a string
            {
                auto n = good;
                auto const at = n.find("\"Bearer\":\"");
                n.replace(at + 9, n.find('"', at + 10) + 1 - (at + 9), "1");
                BEAST_EXPECT(!ok(n));
            }
        }

        // size
        {
            std::string big = good;
            big.insert(1, std::string(maxCreditNoteBytes, ' '));
            BEAST_EXPECT(!ok(big));
        }
    }

    void
    testDisabled()
    {
        testcase("disabled");
        using namespace jtx;
        Env env{*this, supported_amendments() - featureCreditNote};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        auto const key = bearerKey();
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(claim(bob, freshNote(env, alice, "10000000", key), alice, key),
            ter(temDISABLED));
    }

    void
    testXAH()
    {
        testcase("native");
        using namespace jtx;
        Env env{*this, supported_amendments()};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        Account const carol{"carol"};                  // secp256k1
        Account const dave{"dave", KeyType::ed25519};  // never funded
        auto const key = bearerKey();
        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        auto const text = freshNote(env, alice, "10000000", key);

        // a well-formed ed25519 signature by a key the issuer does not hold
        env(claim(bob, text, dave, key), ter(tecNO_PERMISSION));

        // signature over different bytes
        {
            auto jv = claim(bob, text, alice, key);
            jv[sfNote.fieldName] =
                strHex(freshNote(env, alice, "10000001", key));
            env(jv, ter(temBAD_SIGNATURE));
        }

        // not ed25519
        env(claim(bob, text, carol, key), ter(temMALFORMED));

        // wrong network
        {
            auto bad = text;
            auto const nid = "\"NetworkID\":" +
                std::to_string(env.app().config().NETWORK_ID);
            bad.replace(bad.find(nid), nid.size(), "\"NetworkID\":9");
            env(claim(bob, bad, alice, key), ter(temMALFORMED));
        }

        // redeeming your own note
        env(claim(alice, text, alice, key), ter(temREDUNDANT));

        // too early, expired
        env(claim(
                bob,
                freshNote(env, alice, "10000000", key, 120'000),
                alice,
                key),
            ter(terPRE_SEQ));
        {
            auto const t = nowMs(env) - 700'000;
            env(claim(
                    bob,
                    noteText(env, alice, "10000000", t, t + 600'000, key),
                    alice,
                    key),
                ter(tecEXPIRED));
        }

        // issuer cannot cover it above their reserve
        env(claim(bob, freshNote(env, alice, "10000000000", key), alice, key),
            ter(tecUNFUNDED_PAYMENT));

        BEAST_EXPECT(!lastNoteTime(env, alice));

        // the note redeems, and once
        auto const issued = parseCreditNote(text).issueTime;
        STAmount const before = env.balance(bob);
        STAmount const fee{env.current()->fees().base};
        env(claim(bob, text, alice, key));
        env.close();
        BEAST_EXPECT(env.balance(bob) == before + XRP(10).value() - fee);
        BEAST_EXPECT(lastNoteTime(env, alice) == issued);
        env(claim(bob, text, alice, key), ter(tefPAST_SEQ));
        env(claim(carol, text, alice, key), ter(tefPAST_SEQ));

        // a note issued before the redeemed one is retired with it ...
        env(claim(
                bob,
                noteText(
                    env, alice, "1000000", issued - 1, issued + 600'000, key),
                alice,
                key),
            ter(tefPAST_SEQ));

        // ... and one issued after it still redeems
        auto const later =
            noteText(env, alice, "1000000", issued + 1, issued + 600'000, key);
        env(claim(carol, later, alice, key));
        env.close();
        BEAST_EXPECT(lastNoteTime(env, alice) == issued + 1);
    }

    void
    testBearer()
    {
        testcase("bearer key");
        using namespace jtx;
        Env env{*this, supported_amendments()};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        Account const carol{"carol"};
        auto const key = bearerKey();
        auto const other = bearerKey("other");
        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        auto const text = freshNote(env, alice, "10000000", key);

        // no proof at all
        {
            auto jv = claim(bob, text, alice, key);
            jv.removeMember(sfBearerSignature.fieldName);
            env(jv, ter(temMALFORMED));
        }

        // a proof by a key that is not the note's Bearer
        env(claim(bob, text, alice, other), ter(temBAD_SIGNATURE));

        // bob's proof, lifted into carol's claim: bound to the account
        {
            auto jv = claim(carol, text, alice, key);
            jv[sfBearerSignature.fieldName] =
                strHex(bearerSign(key, text, bob));
            env(jv, ter(temBAD_SIGNATURE));
        }

        // a proof over a different note with the same Bearer: bound to the
        // note's bytes
        {
            auto jv = claim(bob, text, alice, key);
            jv[sfBearerSignature.fieldName] = strHex(
                bearerSign(key, freshNote(env, alice, "10000001", key), bob));
            env(jv, ter(temBAD_SIGNATURE));
        }

        // the issuer's own signature is not a bearer proof, nor vice versa
        {
            auto jv = claim(bob, text, alice, key);
            jv[sfBearerSignature.fieldName] = jv[sfNoteSignature.fieldName];
            env(jv, ter(temBAD_SIGNATURE));
        }
        {
            auto jv = claim(bob, text, alice, key);
            jv[sfNoteSignature.fieldName] = jv[sfBearerSignature.fieldName];
            env(jv, ter(temBAD_SIGNATURE));
        }

        // nothing above cost a fee or moved LastNoteTime
        BEAST_EXPECT(!lastNoteTime(env, alice));

        // the holder of the seed redeems, to whichever account they like
        STAmount const before = env.balance(carol);
        STAmount const fee{env.current()->fees().base};
        env(claim(carol, text, alice, key));
        env.close();
        BEAST_EXPECT(env.balance(carol) == before + XRP(10).value() - fee);

        // each note gets its own key: a later note under the first key is
        // the issuer's choice, and still redeems only with that key
        env(claim(bob, freshNote(env, alice, "1000000", key, 1), alice, other),
            ter(temBAD_SIGNATURE));
        env(claim(
            bob, freshNote(env, alice, "1000000", other, 1), alice, other));
        env.close();
    }

    void
    testRegularKey()
    {
        testcase("regular key");
        using namespace jtx;
        Env env{*this, supported_amendments()};
        Account const alice{"alice"};  // secp256k1 master
        Account const alie{"alie", KeyType::ed25519};
        Account const bob{"bob"};
        auto const key = bearerKey();
        env.fund(XRP(10000), alice, bob);
        env.close();

        // alice's master is secp256k1, which cannot sign a note
        env(claim(bob, freshNote(env, alice, "1000000", key), alice, key),
            ter(temMALFORMED));

        // an ed25519 key she does not hold yet
        env(claim(bob, freshNote(env, alice, "1000000", key), alie, key),
            ter(tecNO_PERMISSION));
        // Close before the regkey: a tec stays in the open ledger and is
        // re-applied at close in canonical order, where it could land after
        // the regkey and redeem.
        env.close();

        // as her regular key it signs for her
        env(regkey(alice, alie));
        env.close();
        env(claim(bob, freshNote(env, alice, "1000000", key), alie, key));
        env.close();

        // and a disabled master no longer does
        Account const carol{"carol", KeyType::ed25519};
        env.fund(XRP(10000), carol);
        env(regkey(carol, alie));
        env(fset(carol, asfDisableMaster), sig(carol));
        env.close();
        env(claim(bob, freshNote(env, carol, "1000000", key), carol, key),
            ter(tecNO_PERMISSION));
        env(claim(bob, freshNote(env, carol, "1000000", key, 1), alie, key));
        env.close();
    }

    void
    testIOU()
    {
        testcase("issued currency");
        using namespace jtx;
        Env env{*this, supported_amendments()};
        Account const alice{"alice", KeyType::ed25519};
        Account const bob{"bob"};
        Account const gw{"gw", KeyType::ed25519};
        auto const USD = gw["USD"];
        auto const key = bearerKey();
        env.fund(XRP(10000), alice, bob, gw);
        env.close();
        env(trust(alice, USD(1000)));
        env(pay(gw, alice, USD(100)));
        env.close();

        // bob has no trust line; none is created
        env(claim(bob, freshNote(env, alice, iou(USD, "10"), key), alice, key),
            ter(tecNO_LINE));

        env(trust(bob, USD(1000)));
        env.close();

        // more than alice holds
        env(claim(bob, freshNote(env, alice, iou(USD, "101"), key), alice, key),
            ter(tecUNFUNDED_PAYMENT));
        env.close();

        // frozen line
        env(trust(gw, bob["USD"](0), tfSetFreeze));
        env.close();
        env(claim(bob, freshNote(env, alice, iou(USD, "10"), key), alice, key),
            ter(tecFROZEN));
        // The rejected claim must close before the unfreeze: otherwise the
        // close re-applies both in canonical order and, with the unfreeze
        // first, the note redeems after all.
        env.close();
        env(trust(gw, bob["USD"](0), tfClearFreeze));
        env.close();

        // deep frozen line: cannot receive at all
        env(trust(gw, bob["USD"](0), tfSetFreeze | tfSetDeepFreeze));
        env.close();
        env(claim(bob, freshNote(env, alice, iou(USD, "10"), key), alice, key),
            ter(tecFROZEN));
        env.close();
        env(trust(gw, bob["USD"](0), tfClearFreeze | tfClearDeepFreeze));
        env.close();

        // transit: bob receives 10, alice pays 10 plus the transfer fee
        env(rate(gw, 1.25));
        env.close();
        env(claim(bob, freshNote(env, alice, iou(USD, "10"), key), alice, key));
        env.close();
        BEAST_EXPECT(env.balance(bob, USD) == USD(10));
        BEAST_EXPECT(env.balance(alice, USD) == USD(87.5));

        // issue: the gateway's own note, no fee
        env(claim(bob, freshNote(env, gw, iou(USD, "5"), key), gw, key));
        env.close();
        BEAST_EXPECT(env.balance(bob, USD) == USD(15));

        // redemption: a note cashed by the gateway, no line needed on its side
        env(claim(
            gw, freshNote(env, alice, iou(USD, "7"), key, 1000), alice, key));
        env.close();
        BEAST_EXPECT(env.balance(alice, USD) == USD(80.5));
    }

    void
    run() override
    {
        testParse();
        testDisabled();
        testXAH();
        testBearer();
        testRegularKey();
        testIOU();
    }
};

BEAST_DEFINE_TESTSUITE(CreditNote, app, ripple);

}  // namespace test
}  // namespace ripple
