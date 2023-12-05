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

// Round Robin Test
// Multiple in one Ledger Test
// Forward and Backward remits
// Compute a uri token id inline with the send (a token that is being minted)

namespace ripple {
namespace test {
struct Remit_test : public beast::unit_test::suite
{

    // testDebug("PRE", env, { alice, bob }, {});
    void
    testDebug(
        std::string const& testNumber,
        jtx::Env const& env,
        std::vector<jtx::Account> const& accounts,
        std::vector<jtx::IOU> const& ious)
    {
        std::cout << "DEBUG: " << testNumber << "\n";
        for (std::size_t a = 0; a < accounts.size(); ++a)
        {
            auto const bal = env.balance(accounts[a]);
            std::cout << "account: " <<  accounts[a].human() << "BAL: " << bal << "\n";
            for (std::size_t i = 0; i < ious.size(); ++i)
            {
                auto const iouBal = env.balance(accounts[a], ious[i]);
                std::cout << "account: " <<  accounts[a].human() << "IOU: " << iouBal << "\n";
            }
        }
    }

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

    static AccountID
    tokenOwner(ReadView const& view, uint256 const& id)
    {
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return AccountID();
        return slep->getAccountID(sfOwner);
    }

    static AccountID
    tokenIsser(ReadView const& view, uint256 const& id)
    {
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return AccountID();
        return slep->getAccountID(sfIssuer);
    }

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

        // REMIT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");

            Env env{*this, features};
            // Env env{*this, envconfig(), amend, nullptr,
            //     // beast::severities::kWarning
            //     beast::severities::kTrace
            // };

            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(1000), alice, bob);
            env.close();

            env(remit::remit(alice, bob), ter(tesSUCCESS));
            env.close();
            // auto const preAlice = env.balance(alice, USD.issue());
        }

        // REMIT: XAH
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(1000), alice, bob);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops);
            BEAST_EXPECT(postBob == preBob + XRP(1));
        }

        // REMIT: USD
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // setup
            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());
            
            // remit
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                ter(tesSUCCESS));
            env.close();
            
            // validate
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - feeDrops);
            BEAST_EXPECT(postBob == preBob);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }

        // REMIT: XAH + USD
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // setup
            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());
            
            // remit
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tesSUCCESS));
            env.close();
            
            // validate
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops);
            BEAST_EXPECT(postBob == preBob + XRP(1));
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }

        // REMIT: URITOKEN XFER
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(1000), alice, bob);
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, tid));

            // remit with uritoken id
            env(remit::remit(alice, bob),
                remit::token_ids({ strHex(tid) }),
                ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: URITOKEN MINT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(1000), alice, bob);
            env.close();

            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(remit::remit(alice, bob), remit::uri(uri), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid) == alice.id());

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: XAH + URITOKEN XFER
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob);
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            // remit xah + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                remit::token_ids({ strHex(tid) }),
                ter(tesSUCCESS));
            env.close();

            // verify uri transfer
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());

            // verify xah
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + feeReserve);

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                remit::token_ids({ strHex(tid) }),
                ter(tesSUCCESS));
            env.close();

            // verify uri transfer
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());
            
            // verify usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + feeReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                remit::token_ids({ strHex(tid) }),
                ter(tesSUCCESS));
            env.close();

            // verify uri transfer
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());
            
            // verify xah & usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + feeReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: XAH + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob);
            env.close();

            // mint uri token
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                remit::token_ids({ strHex(tid1) }),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();

            // verify uri mint
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid2));
            BEAST_EXPECT(tokenOwner(*env.current(), tid2) == bob.id());

            // verify uri transfer
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid1) == alice.id());

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);

            // verify xah
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(postBob == preBob + XRP(1) + (feeReserve * 2));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                remit::token_ids({ strHex(tid1) }),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();

            // verify uri mint
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid2));
            BEAST_EXPECT(tokenOwner(*env.current(), tid2) == bob.id());

            // verify uri transfer
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid1) == alice.id());

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 3);

            // verify usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(postBob == preBob + (feeReserve * 2));
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                remit::token_ids({ strHex(tid1) }),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();

            // verify uri mint
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid2));
            BEAST_EXPECT(tokenOwner(*env.current(), tid2) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid2) == alice.id());

            // verify uri transfer
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid1) == alice.id());

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 3);

            // verify xah & usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(postBob == preBob + XRP(1) + (feeReserve * 2));
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }
    }

    void
    testDestExistsTLDNE(FeatureBitset features)
    {
        testcase("dest exists and trustline does not exist");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // REMIT: USD
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            // setup
            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(preAlice == XRP(1000));
            BEAST_EXPECT(preAliceUSD == USD(10000));
            BEAST_EXPECT(preBob == XRP(1000));
            BEAST_EXPECT(preBobUSD == USD(0));

            // remit
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                ter(tesSUCCESS));
            env.close();

            // validate
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + feeReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }

        // REMIT: XAH + USD
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            // setup
            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(preAlice == XRP(1000));
            BEAST_EXPECT(preAliceUSD == USD(10000));
            BEAST_EXPECT(preBob == XRP(1000));
            BEAST_EXPECT(preBobUSD == USD(0));

            // remit
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tesSUCCESS));
            env.close();

            // validate
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + feeReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }

        // REMIT: USD + URITOKEN XFER
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                remit::token_ids({ strHex(tid) }),
                ter(tesSUCCESS));
            env.close();

            // verify uri transfer
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());

            // verify xah & usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - feeDrops - (2 * feeReserve));
            BEAST_EXPECT(postBob == preBob + (2 * feeReserve));
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                remit::token_ids({ strHex(tid) }),
                ter(tesSUCCESS));
            env.close();

            // verify uri transfer
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());

            // verify xah & usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - (2 * feeReserve));
            BEAST_EXPECT(postBob == preBob + XRP(1) + (2 * feeReserve));
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                remit::token_ids({ strHex(tid1) }),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();

            // verify uri mint
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid2));
            BEAST_EXPECT(tokenOwner(*env.current(), tid2) == bob.id());

            // verify uri transfer
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid1) == alice.id());

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 3);

            // verify xah
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - feeDrops - (feeReserve * 3));
            BEAST_EXPECT(postBob == preBob + (feeReserve * 3));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

            Env env{*this, features};

            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            // mint uri token
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());
            auto const preBob = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());

            // remit xah/usd + uritoken id + uritoken mint
            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                remit::token_ids({ strHex(tid1) }),
                remit::uri(uri2),
                ter(tesSUCCESS));
            env.close();

            // verify uri mint
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid2));
            BEAST_EXPECT(tokenOwner(*env.current(), tid2) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid2) == alice.id());

            // verify uri transfer
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid1) == alice.id());

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 3);

            // verify xah & usd
            auto const postAlice = env.balance(alice);
            auto const postAliceUSD = env.balance(alice, USD.issue());
            auto const postBob = env.balance(bob);
            auto const postBobUSD = env.balance(bob, USD.issue());
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - (feeReserve * 3));
            BEAST_EXPECT(postBob == preBob + XRP(1) + (feeReserve * 3));
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
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
        testDestExistsTLDNE(features);
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
