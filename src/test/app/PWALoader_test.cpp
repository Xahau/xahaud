//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/protocol/AppLoader.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/digest.h>
#include <xrpl/protocol/jss.h>

#include <string>

namespace ripple {
namespace test {

class PWALoader_test : public beast::unit_test::suite
{
    static appLoader::Result
    check(std::string const& s)
    {
        return appLoader::validate(
            reinterpret_cast<std::uint8_t const*>(s.data()), s.size());
    }

    static std::string
    docOfSize(std::size_t len)
    {
        std::string const head = "<!DOCTYPE html><html><body>";
        std::string const tail = "</body></html>";
        if (len < head.size() + tail.size())
            return head + tail;
        return head + std::string(len - head.size() - tail.size(), 'x') + tail;
    }

    static std::string
    doc()
    {
        return "<!DOCTYPE html><html><body>hi</body></html>";
    }

    // AccountSet carrying sfAppLoader.
    static Json::Value
    set(jtx::Account const& a, std::string const& d)
    {
        auto jt = jtx::noop(a);
        jt[sfAppLoader.fieldName] = strHex(d);
        return jt;
    }

    // validate() is consensus critical: these pin its edges, so a refactor
    // that moves any of them fails here rather than forking the network.
    void
    testValidator()
    {
        testcase("validator");
        using R = appLoader::Result;
        using namespace std::string_literals;

        for (auto const& s :
             {"<!DOCTYPE html><html></html>"s,
              "<!doctype html><html></html>"s,
              "<html></html>"s,
              "<HTML></HTML>"s,
              "<html lang=\"en\" class=\"root\"></html>"s,
              "  \n\t<html></html>\n\n  "s,
              "<html></html >"s,
              "<html></html\n>"s,
              "\xEF\xBB\xBF<html></html>"s,
              "\xEF\xBB\xBF<!DOCTYPE html><html></html>"s,
              "<html><body>\xE6\xBC\xA2\xE5\xAD\x97</body></html>"s,
              "<html><script>var s=\"</html>\";</script></html>"s,
              "<html>\t\n\r\f</html>"s,
              "<html></html>  \n"s,
              "<!DOCTYPE html>\n<!-- c --><html></html>"s,
              "<html>\xC2\x85</html>"s,  // C1 controls are not rejected
              docOfSize(maxAppLoaderLength)})
            BEAST_EXPECTS(check(s) == R::ok, s);

        std::vector<std::pair<std::string, R>> const bad{
            {""s, R::empty},
            {"   \n"s, R::empty},
            {"\xEF\xBB\xBF"s, R::empty},
            {docOfSize(maxAppLoaderLength + 1), R::tooLarge},
            {"<html>\xC0\xAF</html>"s, R::badUTF8},
            {"<html>\xED\xA0\x80</html>"s, R::badUTF8},
            {"<html>\xEF\xBF\xBE</html>"s, R::badUTF8},
            {"<html>\xE6\xBC</html>"s, R::badUTF8},
            {"<html></html>\xE6"s, R::badUTF8},
            {"<html>\0</html>"s, R::badControlChar},
            {"<html>\x1B</html>"s, R::badControlChar},
            {"<html>\x7F</html>"s, R::badControlChar},
            {"hello"s, R::noDoctype},
            {"<div>frag</div>"s, R::noDoctype},
            {"<htmlish></html>"s, R::noDoctype},
            {"<!DOCTYPEhtml><html></html>"s, R::noDoctype},
            {"<!DOCTYPE"s, R::noDoctype},
            {"<!DOCTYPE html>"s, R::noHtmlElement},
            {"<!DOCTYPE html><htmlish></html>"s, R::noHtmlElement},
            {"<html>"s, R::unclosed},
            {"<html></html"s, R::unclosed},
            {"<html></htmlx>"s, R::unclosed},
            {"<html></html>trailing"s, R::trailingGarbage},
            {"<html></html><p>"s, R::trailingGarbage},
            {"<html></html>\xC2\xA0"s, R::trailingGarbage},  // NBSP
        };
        for (auto const& [s, r] : bad)
            BEAST_EXPECTS(
                check(s) == r, s + ": " + appLoader::to_string(check(s)));
    }

    void
    testEnabled()
    {
        testcase("amendment gate");
        using namespace jtx;
        Env env{*this, supported_amendments() - featurePWALoader};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();
        env(set(alice, doc()), fee(XRP(1)), ter(temDISABLED));
        env(set(alice, ""), fee(XRP(1)), ter(temDISABLED));
        env.close();
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfAppLoaderSequence));
    }

    void
    testSetAndRemove()
    {
        testcase("set, overwrite, remove, sequence");
        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();
        auto const kl = keylet::appLoader(alice.id());
        auto seq = [&] {
            return (*env.le(alice))[~sfAppLoaderSequence].value_or(0);
        };

        BEAST_EXPECT(!env.le(kl));
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfAppLoaderSequence));

        // Create: the object, with the owner's new sequence; nothing inline
        // on the AccountRoot, no reserve, not in the owner directory.
        env(set(alice, doc()), fee(XRP(1)));
        env.close();
        auto sle = env.le(kl);
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT((*sle)[sfAppLoader] == makeSlice(doc()));
            BEAST_EXPECT((*sle)[sfOwner] == alice.id());
            BEAST_EXPECT((*sle)[sfAppLoaderSequence] == 1);
        }
        BEAST_EXPECT(seq() == 1);
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfAppLoader));
        BEAST_EXPECT((*env.le(alice))[sfOwnerCount] == 0);
        BEAST_EXPECT(
            env.rpc(
                   "json",
                   "account_objects",
                   R"({"account":")" + alice.human() +
                       R"("})")[jss::result][jss::account_objects]
                .size() == 0);
        auto const txid = (*sle)[sfPreviousTxnID];

        // Overwrite: new content, next sequence, rethreaded.
        env(set(alice, "<html><body>v2</body></html>"), fee(XRP(1)));
        env.close();
        sle = env.le(kl);
        if (BEAST_EXPECT(sle))
        {
            BEAST_EXPECT(
                (*sle)[sfAppLoader] ==
                makeSlice(std::string("<html><body>v2</body></html>")));
            BEAST_EXPECT((*sle)[sfAppLoaderSequence] == 2);
            BEAST_EXPECT((*sle)[sfPreviousTxnID] != txid);
        }
        BEAST_EXPECT(seq() == 2);

        // An AccountSet without the field leaves everything alone.
        env(noop(alice));
        env.close();
        BEAST_EXPECT((*env.le(kl))[sfAppLoaderSequence] == 2 && seq() == 2);

        // Remove with an empty blob: the object goes, the sequence still
        // advances (so a copy of sequence 2 is known to be stale), and
        // removing again is a no-op that advances it again.
        env(set(alice, ""), fee(XRP(1)));
        env.close();
        BEAST_EXPECT(!env.le(kl) && seq() == 3);
        env(set(alice, ""), fee(XRP(1)));
        env.close();
        BEAST_EXPECT(!env.le(kl) && seq() == 4);

        // Republish: the sequence carries on.
        env(set(alice, doc()), fee(XRP(1)));
        env.close();
        BEAST_EXPECT((*env.le(kl))[sfAppLoaderSequence] == 5 && seq() == 5);

        // ledger_entry reaches it by account or by index.
        for (auto const& req :
             {R"({"app_loader":")" + alice.human() + R"("})",
              R"({"index":")" + to_string(kl.key) + R"("})"})
        {
            auto const r = env.rpc("json", "ledger_entry", req)[jss::result];
            BEAST_EXPECTS(
                r[jss::node][sfLedgerEntryType.fieldName] == jss::AppLoader &&
                    r[jss::node][sfAppLoaderSequence.fieldName] == 5,
                req);
        }
        BEAST_EXPECT(
            env.rpc(
                "json",
                "ledger_entry",
                R"({"app_loader":"nope"})")[jss::result][jss::error] ==
            "malformedAddress");
    }

    void
    testFee()
    {
        testcase("fee: base plus one drop per byte");
        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();
        auto const base = env.current()->fees().base;
        auto const kl = keylet::appLoader(alice.id());

        auto const expected = base + XRPAmount{1024};
        env(set(alice, docOfSize(1024)),
            fee(expected - XRPAmount{1}),
            ter(telINSUF_FEE_P));
        env.close();
        BEAST_EXPECT(!env.le(kl));
        auto const before = env.balance(alice);
        env(set(alice, docOfSize(1024)), fee(expected));
        env.close();
        BEAST_EXPECT(
            env.le(kl) && before - env.balance(alice) == drops(expected));

        // Overwrites pay again; removal is base fee only.
        env(set(alice, docOfSize(100)),
            fee(base + XRPAmount{99}),
            ter(telINSUF_FEE_P));
        env(set(alice, docOfSize(100)), fee(base + XRPAmount{100}));
        env(set(alice, ""), fee(base));
        env.close();
        BEAST_EXPECT(!env.le(kl));

        // Publishing takes no reserve: an account at its reserve floor can.
        Account const poor{"poor"};
        auto const reserve = env.current()->fees().accountReserve(0);
        env.fund(drops(reserve + base + XRPAmount{100}), noripple(poor));
        env.close();
        env(set(poor, docOfSize(100)), fee(base + XRPAmount{100}));
        env.close();
        BEAST_EXPECT(env.le(keylet::appLoader(poor.id())));
        BEAST_EXPECT(env.balance(poor) == drops(reserve));
        env.require(owners(poor, 0));
    }

    void
    testMalformed()
    {
        testcase("malformed documents are temMALFORMED");
        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();
        for (auto const& d :
             {std::string("hello"),
              std::string("<html>"),
              std::string("<html></html>x"),
              std::string("<html>\0</html>", 14),
              std::string("<html>\xC0\xAF</html>"),
              docOfSize(maxAppLoaderLength + 1)})
        {
            env(set(alice, d), fee(XRP(1)), ter(temMALFORMED));
            env.close();
        }
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfAppLoaderSequence));
    }

    void
    testAccountDelete()
    {
        testcase("account delete erases the loader");
        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"}, bob{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();
        env(set(alice, doc()), fee(XRP(1)));
        env.close();
        for (int i = 0; i < 256; ++i)
            env.close();
        // Not in the owner directory, so it cannot block deletion; erased
        // explicitly, or AccountRootsDeletedClean would fire on the orphan.
        env(acctdelete(alice, bob),
            fee(drops(env.current()->fees().increment)));
        env.close();
        BEAST_EXPECT(!env.le(alice));
        BEAST_EXPECT(!env.le(keylet::appLoader(alice.id())));
    }

    void
    testValidateRPC()
    {
        testcase("pwa_validate");
        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);
        env.close();
        auto validate = [&](std::string const& d, std::string const& domain) {
            Json::Value p(Json::objectValue);
            p[jss::account] = alice.human();
            p[jss::hash] = to_string(sha512Half(makeSlice(d)));
            if (!domain.empty())
                p[jss::domain] = domain;
            return env.rpc("json", "pwa_validate", to_string(p))[jss::result];
        };

        BEAST_EXPECT(validate(doc(), "")["reason"] == "no loader");
        env(set(alice, doc()), fee(XRP(1)));
        env.close();
        auto r = validate(doc(), "");
        BEAST_EXPECT(r["valid"] == true && r["app_loader_sequence"] == 1);
        BEAST_EXPECT(r[jss::ledger_index] == env.closed()->seq());
        BEAST_EXPECT(
            validate("<html></html>", "")["reason"] == "hash mismatch");

        // domain: the account's Domain, or a host beneath it, ignoring case.
        BEAST_EXPECT(
            validate(doc(), "pwa.test")["reason"] ==
            "domain not endorsed by account");
        auto jt = noop(alice);
        jt[sfDomain.fieldName] = strHex(std::string("Pwa.Test"));
        env(jt);
        env.close();
        for (auto const d :
             {"pwa.test", "PWA.TEST.", "pwaabcd.pwa.test", "a.b.pwa.test"})
            BEAST_EXPECTS(validate(doc(), d)["valid"] == true, d);
        for (auto const d : {"other.test", "xpwa.test", "pwa.test.evil"})
            BEAST_EXPECTS(validate(doc(), d)["valid"] == false, d);

        // Malformed input.
        BEAST_EXPECT(
            env.rpc(
                "json",
                "pwa_validate",
                R"({"account":"x","hash":"00"})")[jss::result][jss::error] ==
            "actMalformed");
        BEAST_EXPECT(
            env.rpc(
                "pwa_validate", alice.human(), "zz")[jss::result][jss::error] ==
            "invalidParams");
    }

public:
    void
    run() override
    {
        testValidator();
        testEnabled();
        testSetAndRemove();
        testFee();
        testMalformed();
        testAccountDelete();
        testValidateRPC();
    }
};

BEAST_DEFINE_TESTSUITE(PWALoader, app, ripple);

}  // namespace test
}  // namespace ripple
