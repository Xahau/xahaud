//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <ripple/basics/chrono.h>
#include <ripple/core/ConfigSections.h>
#include <ripple/ledger/Directory.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

#include <chrono>

namespace ripple {
namespace test {
struct SetRemarks_test : public beast::unit_test::suite
{
    static bool
    inOwnerDir(
        ReadView const& view,
        jtx::Account const& acct,
        uint256 const& tid)
    {
        auto const uritSle = view.read({ltURI_TOKEN, tid});
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::find(ownerDir.begin(), ownerDir.end(), uritSle) !=
            ownerDir.end();
    }

    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    };

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        for (bool const withRemarks : {false, true})
        {
            std::cout << "RESULT" << "\n";
            // // If the URIToken amendment is not enabled, you should not be able
            // // to mint, burn, buy, sell or clear uri tokens.
            // auto const amend =
            //     withURIToken ? features : features - featureURIToken;
            // Env env{*this, amend};

            // env.fund(XRP(1000), alice, bob);
            // env.close();

            // std::string const uri(maxTokenURILength, '?');
            // std::string const id{strHex(uritoken::tokenid(alice, uri))};

            // auto const txResult =
            //     withURIToken ? ter(tesSUCCESS) : ter(temDISABLED);
            // auto const ownerDir = withURIToken ? 1 : 0;

            // // MINT
            // env(uritoken::mint(alice, uri), txResult);
            // env.close();
            // BEAST_EXPECT(ownerDirCount(*env.current(), alice) == ownerDir);
            // BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // // SELL
            // env(uritoken::sell(alice, id), uritoken::amt(XRP(10)), txResult);
            // env.close();
            // BEAST_EXPECT(ownerDirCount(*env.current(), alice) == ownerDir);
            // BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // // BUY
            // env(uritoken::buy(bob, id), uritoken::amt(XRP(10)), txResult);
            // env.close();
            // BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            // BEAST_EXPECT(ownerDirCount(*env.current(), bob) == ownerDir);

            // // SELL
            // env(uritoken::sell(bob, id), uritoken::amt(XRP(10)), txResult);
            // env.close();
            // BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            // BEAST_EXPECT(ownerDirCount(*env.current(), bob) == ownerDir);

            // // CLEAR
            // env(uritoken::cancel(bob, id), txResult);
            // env.close();
            // BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            // BEAST_EXPECT(ownerDirCount(*env.current(), bob) == ownerDir);

            // // BURN
            // env(uritoken::burn(bob, id), txResult);
            // env.close();
            // BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            // BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetRemarks, app, ripple);
}  // namespace test
}  // namespace ripple
