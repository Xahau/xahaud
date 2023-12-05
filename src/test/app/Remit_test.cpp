//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs

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

// Destination Exists - Trust Line Exists
// otxn | dest | exists | native | token | tl exists | uris xfr | uri mint |
//  A   |  B   |   Y    |    N   |   N   |     N     |     N    |    N     |
//  A   |  B   |   Y    |    Y   |   N   |     N     |     N    |    N     |
//  A   |  B   |   Y    |    N   |   Y   |     Y     |     N    |    N     |
//  A   |  B   |   Y    |    Y   |   Y   |     Y     |     N    |    N     |
//  A   |  B   |   Y    |    N   |   N   |     N     |     Y    |    N     |
//  A   |  B   |   Y    |    N   |   N   |     N     |     N    |    Y     |
//  A   |  B   |   Y    |    Y   |   N   |     N     |     Y    |    N     |
//  A   |  B   |   Y    |    Y   |   Y   |     Y     |     Y    |    N     |
//  A   |  B   |   Y    |    Y   |   N   |     N     |     Y    |    Y     |
//  A   |  B   |   Y    |    Y   |   Y   |     Y     |     Y    |    Y     |

/*
// Destination Exists - Trust Line DNE
// otxn | dest | exists | native | token | tl exists | uris xfr | uri mint |
//  A   |  B   |   Y    |    N   |   Y   |     N     |     N    |    N     |
//  A   |  B   |   Y    |    Y   |   Y   |     N     |     N    |    N     |
//  A   |  B   |   Y    |    Y   |   Y   |     N     |     Y    |    N     |
//  A   |  B   |   Y    |    Y   |   Y   |     N     |     Y    |    Y     |

// Destination Does Not Exist - Trust Line Exists
// otxn | dest | exists | native | token | tl exists | uris xfr | uri mint |
//  A   |  B   |   N    |    N   |   N   |     N     |     N    |    N     |
//  A   |  B   |   N    |    Y   |   N   |     N     |     N    |    N     |
//  A   |  B   |   N    |    N   |   Y   |     Y     |     N    |    N     |
//  A   |  B   |   N    |    Y   |   Y   |     Y     |     N    |    N     |
//  A   |  B   |   N    |    N   |   N   |     N     |     Y    |    N     |
//  A   |  B   |   N    |    N   |   N   |     N     |     N    |    Y     |
//  A   |  B   |   N    |    Y   |   N   |     N     |     Y    |    N     |
//  A   |  B   |   N    |    Y   |   Y   |     Y     |     Y    |    N     |
//  A   |  B   |   N    |    Y   |   N   |     N     |     Y    |    Y     |
//  A   |  B   |   N    |    Y   |   Y   |     Y     |     Y    |    Y     |

// Destination Exists - Trust Line DNE
// otxn | dest | exists | native | token | tl exists | uris xfr | uri mint |
//  A   |  B   |   N    |    N   |   Y   |     N     |     N    |    N     |
//  A   |  B   |   N    |    Y   |   Y   |     N     |     N    |    N     |
//  A   |  B   |   N    |    Y   |   Y   |     N     |     Y    |    N     |
//  A   |  B   |   N    |    Y   |   Y   |     N     |     Y    |    Y     |
*/

namespace ripple {
namespace test {
struct Remit_test : public beast::unit_test::suite
{

    void
    testEnabled(FeatureBitset features)
    {
        // 0D8BF22FF7570D58598D1EF19EBB6E142AD46E59A223FD3816262FBB69345BEA
        
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        for (bool const withRemit : {true, false})
        {
            auto const amend =
                withRemit ? features : features - featureRemit;

            Env env{*this, amend};

            env.fund(XRP(1000), alice, bob);

            auto const txResult =
                withRemit ? ter(tesSUCCESS) : ter(temDISABLED);

            // REMIT
            env(remit::remit(alice, bob), txResult);
            env.close();
        }
    }

    void
    testDestExistsTLExists(FeatureBitset features)
    {
        testcase("dest exists and trustline exists");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        Env env{*this, features};
        // Env env{*this, envconfig(), amend, nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace
        // };

        auto const feeDrops = env.current()->fees().base;

        env.fund(XRP(1000), alice, bob, gw);
        env.close();
        env.trust(USD(100000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env(pay(gw, bob, USD(10000)));
        env.close();

        // REMIT
        {
            env(remit::remit(alice, bob), ter(tesSUCCESS));
            env.close();
            // auto const preAlice = env.balance(alice, USD.issue());
        }

        // REMIT: XAH
        {
            auto const preAlice = env.balance(alice);
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            BEAST_EXPECT(postAlice == preAlice + XRP(1) - feeDrops);
        }

        // REMIT: XAH + USD
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tesSUCCESS));
            env.close();
        }

        // REMIT: URITOKEN XFER
        {
            // mint uri token
            std::string const uri(maxTokenURILength, 'A');
            std::string const tid{strHex(uritoken::tokenid(alice, uri))};
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            // remit with uritoken id
            env(remit::remit(alice, bob),
                remit::token_ids({tid}),
                ter(tesSUCCESS));
            env.close();
        }

        // REMIT: URITOKEN MINT
        {
            std::string const uri(maxTokenURILength, 'B');
            env(remit::remit(alice, bob), remit::uri(uri), ter(tesSUCCESS));
            env.close();
        }

        // REMIT: XAH + URITOKEN XFER
        {
            // mint uri token
            std::string const uri(maxTokenURILength, 'C');
            std::string const tid{strHex(uritoken::tokenid(alice, uri))};
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            // remit xah + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                remit::token_ids({tid}),
                ter(tesSUCCESS));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER
        {
            // mint uri token
            std::string const uri(maxTokenURILength, 'D');
            std::string const tid{strHex(uritoken::tokenid(alice, uri))};
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            // remit xah/usd + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                remit::token_ids({tid}),
                ter(tesSUCCESS));
            env.close();
        }

        // REMIT: XAH + URITOKEN XFER + URITOKEN MINT
        {
            // mint uri token
            std::string const uri1(maxTokenURILength, 'E');
            std::string const tid{strHex(uritoken::tokenid(alice, uri1))};
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, 'E');
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                remit::token_ids({tid}),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER + URITOKEN MINT
        {
            // mint uri token
            std::string const uri1(maxTokenURILength, 'F');
            std::string const tid{strHex(uritoken::tokenid(alice, uri1))};
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, 'F');
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                remit::token_ids({tid}),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testDestDoesNotExists(FeatureBitset features)
    {   
        testcase("dest does not exist");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        
        Env env{*this, features};
        // Env env{*this, envconfig(), amend, nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace
        // };

        env.fund(XRP(1000), alice, bob, gw);
        env.close();
        env.trust(USD(100000), alice);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env.close();

        // REMIT No Amounts No URI Tokens
        env(remit::remit(alice, bob), ter(tesSUCCESS));
        env.close();

        // REMIT XAH
        env(remit::remit(alice, bob), remit::amts({ XRP(1) }), ter(tesSUCCESS));
        env.close();

        // // REMIT XAH + USD
        // env(remit::remit(alice, bob), remit::amts({ XRP(1), USD(1) }), txResult);
        // env.close();

        // // MINT
        // std::string const uri(maxTokenURILength, '?');
        // std::string const tid{strHex(uritoken::tokenid(alice, uri))};
        // env(uritoken::mint(alice, uri), txResult);
        // env.close();
        
        // // REMIT URI XFER
        // env(remit::remit(alice, bob), remit::token_ids({ tid }), txResult);
        // env.close();

        // // REMIT 2 amount XAH
        // env(remit::remit(alice, bob), txResult);
        // env.close();
    }

    void
    testTLDoesNotExists(FeatureBitset features)
    {   
        testcase("trust line does not exist");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        
        Env env{*this, features};
        // Env env{*this, envconfig(), amend, nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace
        // };

        env.fund(XRP(1000), alice, bob, gw);
        env.close();
        env.trust(USD(100000), alice);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env.close();

        // REMIT No Amounts No URI Tokens
        env(remit::remit(alice, bob), ter(tesSUCCESS));
        env.close();

        // REMIT XAH
        env(remit::remit(alice, bob), remit::amts({ XRP(1) }), ter(tesSUCCESS));
        env.close();

        // // REMIT XAH + USD
        // env(remit::remit(alice, bob), remit::amts({ XRP(1), USD(1) }), txResult);
        // env.close();

        // // MINT
        // std::string const uri(maxTokenURILength, '?');
        // std::string const tid{strHex(uritoken::tokenid(alice, uri))};
        // env(uritoken::mint(alice, uri), txResult);
        // env.close();
        
        // // REMIT URI XFER
        // env(remit::remit(alice, bob), remit::token_ids({ tid }), txResult);
        // env.close();

        // // REMIT 2 amount XAH
        // env(remit::remit(alice, bob), txResult);
        // env.close();
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testDestExistsTLExists(features);
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

BEAST_DEFINE_TESTSUITE(Remit, app, ripple);
}  // namespace test
}  // namespace ripple
