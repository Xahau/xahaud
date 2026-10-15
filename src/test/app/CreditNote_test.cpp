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
    // epoch: the clock NoteClaim checks the note's window against.
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
    lastNoteTime(jtx::Env& env, jtx::Account const& a)
    {
        auto const sle = env.le(a);
        if (!sle || !sle->isFieldPresent(sfLastNoteTime))
            return std::nullopt;
        return sle->getFieldU64(sfLastNoteTime);
    }

    // The note laid out the way a browser's JSON.stringify would.
    static std::string
    noteText(
        jtx::Env& env,
        jtx::Account const& issuer,
        std::string const& amount,
        std::uint64_t issueTime,
        std::uint64_t expiration)
    {
        return "{\"NetworkID\":" +
            std::to_string(env.app().config().NETWORK_ID) + ",\"Account\":\"" +
            issuer.human() + "\",\"Amount\":" + amount + ",\"IssueTime\":\"" +
            jsontx_iso_str(issueTime) + "\",\"Expiration\":\"" +
            jsontx_iso_str(expiration) + "\"}";
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

    // A NoteClaim from `bearer` carrying `text`, signed by `signer`'s key.
    static Json::Value
    claim(
        jtx::Account const& bearer,
        std::string const& text,
        jtx::Account const& signer)
    {
        Json::Value jv;
        jv[jss::TransactionType] = "NoteClaim";
        jv[jss::Account] = bearer.human();
        jv[sfNote.fieldName] = strHex(text);
        jv[sfNoteSignature.fieldName] = strHex(noteSign(signer, text));
        jv[sfPublicKey.fieldName] = strHex(signer.pk().slice());
        return jv;
    }

    // A fresh ten-minute note issued a minute ago.
    static std::string
    freshNote(
        jtx::Env& env,
        jtx::Account const& issuer,
        std::string const& amount,
        std::int64_t offsetMs = 0)
    {
        auto const issue = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(nowMs(env)) - 60'000 + offsetMs);
        return noteText(env, issuer, amount, issue, issue + 600'000);
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
        auto const good = noteText(env, alice, "10000000", t, t + 600'000);

        BEAST_EXPECT(ok(good));
        {
            auto const n = parseCreditNote(good);
            BEAST_EXPECT(n.account == alice.id());
            BEAST_EXPECT(n.amount == STAmount{XRPAmount{10'000'000}});
            BEAST_EXPECT(n.issueTime == t);
            BEAST_EXPECT(n.expiration == t + 600'000);
        }

        // whitespace is the issuer's to choose; the bytes are what is signed
        BEAST_EXPECT(
            ok(" {\n \"NetworkID\": 0 ,\n\"Account\":\"" + alice.human() +
               "\",\"Amount\":1,\"IssueTime\":\"" + jsontx_iso_str(t) +
               "\",\"Expiration\":\"" + jsontx_iso_str(t + 1) + "\" }\n"));

        // issued amounts
        BEAST_EXPECT(ok(noteText(env, alice, iou(USD, "10"), t, t + 600'000)));
        BEAST_EXPECT(ok(noteText(env, alice, iou(USD, "0.5"), t, t + 600'000)));
        BEAST_EXPECT(!ok(noteText(env, alice, iou(USD, "0"), t, t + 600'000)));
        BEAST_EXPECT(!ok(noteText(env, alice, iou(USD, "-1"), t, t + 600'000)));
        BEAST_EXPECT(!ok(noteText(
            env,
            alice,
            "{\"currency\":\"USD\",\"issuer\":\"" + gw.human() + "\"}",
            t,
            t + 600'000)));
        BEAST_EXPECT(!ok(noteText(
            env,
            alice,
            "{\"currency\":\"XAH\",\"issuer\":\"" + gw.human() +
                "\",\"value\":\"1\"}",
            t,
            t + 600'000)));

        // shape
        BEAST_EXPECT(!ok(noteText(env, alice, "0", t, t + 600'000)));
        BEAST_EXPECT(!ok(noteText(env, alice, "-5", t, t + 600'000)));
        BEAST_EXPECT(!ok(noteText(env, alice, "\"10\"", t, t + 600'000)));
        BEAST_EXPECT(!ok(noteText(env, alice, "1.5", t, t + 600'000)));
        BEAST_EXPECT(!ok(""));
        BEAST_EXPECT(!ok("[]"));
        BEAST_EXPECT(!ok(good + " x"));
        BEAST_EXPECT(!ok(good.substr(0, good.size() - 1)));

        // exactly the five members, spelled exactly so
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
        BEAST_EXPECT(!ok(noteText(env, alice, "1", t, t)));
        BEAST_EXPECT(!ok(noteText(env, alice, "1", t + 1, t)));
        BEAST_EXPECT(ok(noteText(env, alice, "1", t, t + creditNoteMaxLifeMs)));
        BEAST_EXPECT(
            !ok(noteText(env, alice, "1", t, t + creditNoteMaxLifeMs + 1)));

        // a time the way sfTime renders, or nothing
        BEAST_EXPECT(
            !ok("{\"NetworkID\":0,\"Account\":\"" + alice.human() +
                "\",\"Amount\":1,\"IssueTime\":" + std::to_string(t) +
                ",\"Expiration\":\"" + jsontx_iso_str(t + 1) + "\"}"));

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
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(claim(bob, freshNote(env, alice, "10000000"), alice),
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
        Account const carol{"carol"};
        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        auto const text = freshNote(env, alice, "10000000");

        // signature by a key the issuer does not hold
        env(claim(bob, text, carol), ter(tecNO_PERMISSION));

        // signature over different bytes
        {
            auto jv = claim(bob, text, alice);
            jv[sfNote.fieldName] = strHex(freshNote(env, alice, "10000001"));
            env(jv, ter(temBAD_SIGNATURE));
        }

        // not ed25519
        {
            auto jv = claim(bob, text, carol);
            jv[sfNoteSignature.fieldName] = strHex(noteSign(carol, text));
            env(jv, ter(temMALFORMED));
        }

        // wrong network
        {
            auto bad = text;
            auto const nid = "\"NetworkID\":" +
                std::to_string(env.app().config().NETWORK_ID);
            bad.replace(bad.find(nid), nid.size(), "\"NetworkID\":9");
            env(claim(bob, bad, alice), ter(temMALFORMED));
        }

        // redeeming your own note
        env(claim(alice, text, alice), ter(temREDUNDANT));

        // too early, expired
        env(claim(bob, freshNote(env, alice, "10000000", 120'000), alice),
            ter(terPRE_SEQ));
        {
            auto const t = nowMs(env) - 700'000;
            env(claim(
                    bob,
                    noteText(env, alice, "10000000", t, t + 600'000),
                    alice),
                ter(tecEXPIRED));
        }

        // issuer cannot cover it above their reserve
        env(claim(bob, freshNote(env, alice, "10000000000"), alice),
            ter(tecUNFUNDED_PAYMENT));

        BEAST_EXPECT(!lastNoteTime(env, alice));

        // the note redeems, and once
        auto const issued = parseCreditNote(text).issueTime;
        STAmount const before = env.balance(bob);
        STAmount const fee{env.current()->fees().base};
        env(claim(bob, text, alice));
        env.close();
        BEAST_EXPECT(env.balance(bob) == before + XRP(10).value() - fee);
        BEAST_EXPECT(lastNoteTime(env, alice) == issued);
        env(claim(bob, text, alice), ter(tefPAST_SEQ));
        env(claim(carol, text, alice), ter(tefPAST_SEQ));

        // a note issued before the redeemed one is retired with it ...
        env(claim(
                bob,
                noteText(env, alice, "1000000", issued - 1, issued + 600'000),
                alice),
            ter(tefPAST_SEQ));

        // ... and one issued after it still redeems
        auto const later =
            noteText(env, alice, "1000000", issued + 1, issued + 600'000);
        env(claim(carol, later, alice));
        env.close();
        BEAST_EXPECT(lastNoteTime(env, alice) == issued + 1);
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
        env.fund(XRP(10000), alice, bob);
        env.close();

        // alice's master is secp256k1, which cannot sign a note
        env(claim(bob, freshNote(env, alice, "1000000"), alice),
            ter(temMALFORMED));

        // an ed25519 key she does not hold yet
        env(claim(bob, freshNote(env, alice, "1000000"), alie),
            ter(tecNO_PERMISSION));

        // as her regular key it signs for her
        env(regkey(alice, alie));
        env.close();
        env(claim(bob, freshNote(env, alice, "1000000"), alie));
        env.close();

        // and a disabled master no longer does
        Account const carol{"carol", KeyType::ed25519};
        env.fund(XRP(10000), carol);
        env(regkey(carol, alie));
        env(fset(carol, asfDisableMaster), sig(carol));
        env.close();
        env(claim(bob, freshNote(env, carol, "1000000"), carol),
            ter(tecNO_PERMISSION));
        env(claim(bob, freshNote(env, carol, "1000000", 1), alie));
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
        env.fund(XRP(10000), alice, bob, gw);
        env.close();
        env(trust(alice, USD(1000)));
        env(pay(gw, alice, USD(100)));
        env.close();

        // bob has no trust line; none is created
        env(claim(bob, freshNote(env, alice, iou(USD, "10")), alice),
            ter(tecNO_LINE));

        env(trust(bob, USD(1000)));
        env.close();

        // more than alice holds
        env(claim(bob, freshNote(env, alice, iou(USD, "101")), alice),
            ter(tecUNFUNDED_PAYMENT));

        // frozen line
        env(trust(gw, bob["USD"](0), tfSetFreeze));
        env.close();
        env(claim(bob, freshNote(env, alice, iou(USD, "10")), alice),
            ter(tecFROZEN));
        env(trust(gw, bob["USD"](0), tfClearFreeze));
        env.close();

        // transit: bob receives 10, alice pays 10 plus the transfer fee
        env(rate(gw, 1.25));
        env.close();
        env(claim(bob, freshNote(env, alice, iou(USD, "10")), alice));
        env.close();
        BEAST_EXPECT(env.balance(bob, USD) == USD(10));
        BEAST_EXPECT(env.balance(alice, USD) == USD(87.5));

        // issue: the gateway's own note, no fee
        env(claim(bob, freshNote(env, gw, iou(USD, "5")), gw));
        env.close();
        BEAST_EXPECT(env.balance(bob, USD) == USD(15));

        // redemption: a note cashed by the gateway, no line needed on its side
        env(claim(gw, freshNote(env, alice, iou(USD, "7"), 1000), alice));
        env.close();
        BEAST_EXPECT(env.balance(alice, USD) == USD(80.5));
    }

    void
    run() override
    {
        testParse();
        testDisabled();
        testXAH();
        testRegularKey();
        testIOU();
    }
};

BEAST_DEFINE_TESTSUITE(CreditNote, app, ripple);

}  // namespace test
}  // namespace ripple
