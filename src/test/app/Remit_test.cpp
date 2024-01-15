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

// Destination Exists - Trust Line DNE
// otxn | dest | exists | native | token | tl exists |
//  A   |  B   |   Y    |    N   |   Y   |     N     |
//  A   |  B   |   Y    |    Y   |   Y   |     N     |

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

// Destination DNE - Trust Line DNE
// otxn | dest | exists | native | token | tl exists |
//  A   |  B   |   Y    |    N   |   Y   |     N     |
//  A   |  B   |   Y    |    Y   |   Y   |     N     |


// Round Robin Test - A -> B -> C
// - Multiple in one Ledger Test
// - Forward and Backward remits
// Compute a uri token id inline with the send (a token that is being minted)
// URITokenMint: sfDigest
// URITokenMint: sfFlags

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
            std::cout << "account: " << accounts[a].human() << "BAL: " << bal
                      << "\n";
            for (std::size_t i = 0; i < ious.size(); ++i)
            {
                auto const iouBal = env.balance(accounts[a], ious[i]);
                std::cout << "account: " << accounts[a].human()
                          << "IOU: " << iouBal << "\n";
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

    static STAmount
    lineBalance(
        jtx::Env const& env,
        jtx::Account const& account,
        jtx::Account const& gw,
        jtx::IOU const& iou)
    {
        auto const sle = env.le(keylet::line(account, gw, iou.currency));
        if (sle && sle->isFieldPresent(sfBalance))
            return (*sle)[sfBalance];
        return STAmount(iou, 0);
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
            auto const amend = withRemit ? features : features - featureRemit;

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

    testPreflightInvalid(FeatureBitset features)
    {
        testcase("preflight invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, gw);
        env.close();

        //----------------------------------------------------------------------
        // preflight

        // temINVALID_FLAG - Invalid flags set.
        {
            env(remit::remit(alice, bob),
                txflags(tfBurnable),
                ter(temINVALID_FLAG));
            env.close();
        }

        // temREDUNDANT - Remit to self.
        {
            env(remit::remit(alice, alice), ter(temREDUNDANT));
            env.close();
        }

        // temMALFORMED - Expected AmountEntry.
        {
            auto tx = remit::remit(alice, bob);
            std::vector<STAmount> const amts_ = {XRP(1)};
            auto& ja = tx[sfAmounts.getJsonName()];
            for (std::size_t i = 0; i < amts_.size(); ++i)
            {
                ja[i][sfGenesisMint.jsonName] = Json::Value{};
                ja[i][sfGenesisMint.jsonName][jss::Amount] =
                    amts_[i].getJson(JsonOptions::none);
                ja[i][sfGenesisMint.jsonName][jss::Destination] = bob.human();
            }
            tx[sfAmounts.jsonName] = ja;
            env(tx, ter(temMALFORMED));
            env.close();
        }

        // temBAD_AMOUNT - Bad amount
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(-1)}),
                ter(temBAD_AMOUNT));
            env.close();
        }

        // temBAD_CURRENCY - Bad currency.
        {
            auto const bXAH = gw["XaH"];
            env(remit::remit(alice, bob),
                remit::amts({bXAH(100)}),
                ter(temBAD_CURRENCY));
            env.close();
        }

        // temMALFORMED - Native Currency appears more than once
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), XRP(1)}),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Issued Currency appears more than once.
        {
            env(remit::remit(alice, bob),
                remit::amts({USD(1), USD(1)}),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - URI was too short/long. (short)
        {
            std::string const uri(0, '?');
            env(remit::remit(alice, bob), remit::uri(uri), ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - URI was too short/long. (long)
        {
            std::string const uri(maxTokenURILength + 1, '?');
            env(remit::remit(alice, bob), remit::uri(uri), ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Invalid UTF8 inside MintURIToken.

        // temINVALID_FLAG - Invalid URI Mint Flag
        {
            std::string const uri(maxTokenURILength, '?');
            env(remit::remit(alice, bob),
                remit::uri(uri, tfAllowXRP),
                ter(temINVALID_FLAG));
            env.close();
        }

        // temMALFORMED - Duplicate URITokenID.
        {
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid1), strHex(tid2)}),
                ter(temMALFORMED));
            env.close();
        }
    }

    void
    testDoApplyInvalid(FeatureBitset features)
    {
        testcase("doApply invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        //----------------------------------------------------------------------
        // doApply

        // terNO_ACCOUNT - account doesnt exist
        // {
        //     auto const carol = Account("carol");
        //     env.memoize(carol);
        //     env(remit::remit(carol, bob), ter(terNO_ACCOUNT));
        //     env.close();
        // }

        // tecNO_PERMISSION - lsfDisallowIncomingRemit
        // see testDisallowIncoming
        
        // tecDST_TAG_NEEDED
        // see testDstTag

        // tecDUPLICATE
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            std::string const uri(maxTokenURILength, '?');
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env(remit::remit(alice, bob),
                remit::uri(uri),
                ter(tecDUPLICATE));
            env.close();
        }

        // tecDIR_FULL - account directory full
        // DA: impossible test

        // tecNO_ENTRY
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid)}),
                ter(tecNO_ENTRY));
            env.close();
        }

        // tecINTERNAL - not actually a uritoken.
        // DA: impossible test

        // tecNO_PERMISSION - not owned by sender.
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(bob, uri);
            env(uritoken::mint(bob, uri), ter(tesSUCCESS));
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid)}),
                ter(tecNO_PERMISSION));
            env.close();
        }

        // tefBAD_LEDGER - invalid owner directory
        {

        }

        // tecDIR_FULL - destination directory full
        // DA: impossible test

        // tecUNFUNDED_PAYMENT
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            env(remit::remit(alice, bob),
                remit::amts({USD(1001)}),
                ter(tecUNFUNDED_PAYMENT));
            env.close();
        }
    
        // tecINTERNAL - negative XRP
        // DA: impossible test

        // tecUNFUNDED_PAYMENT
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            env(remit::remit(alice, bob),
                remit::amts({XRP(1001)}),
                ter(tecUNFUNDED_PAYMENT));
            env.close();
        }

        // tecINTERNAL
        // DA: impossible test

        // tecINTERNAL
        // DA: impossible test

        // tecINSUFFICIENT_RESERVE
        {
            Env env{*this, features};
            env.fund(XRP(250), alice, bob);
            env.close();

            std::string const uri(maxTokenURILength, '?');
            env(remit::remit(alice, bob),
                remit::uri(uri),
                ter(tecINSUFFICIENT_RESERVE));
            env.close();
        }
    }

    void
    testDisallowXRP(FeatureBitset features)
    {
        // auth amount defaults to balance if not present
        testcase("Disallow XRP");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        {
            // Make a remit where dst disallows XRP
            Env env(*this, features - featureDepositAuth);
            env.fund(XRP(10000), alice, bob);
            env(fset(bob, asfDisallowXRP));

            env(remit::remit(alice, bob),
                remit::amts({ XRP(1) }),
                ter(tesSUCCESS));

        }
        {
            // Make a remit where dst disallows XRP.  Ignore that flag,
            // since it's just advisory.
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob);
            env(fset(bob, asfDisallowXRP));
            env(remit::remit(alice, bob),
                remit::amts({ XRP(1) }),
                ter(tesSUCCESS));
        }
    }

    void
    testDstTag(FeatureBitset features)
    {
        // auth amount defaults to balance if not present
        testcase("Dst Tag");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(10000), alice, bob);
        env(fset(bob, asfRequireDest));
        {
            env(remit::remit(alice, bob),
                remit::amts({ XRP(1) }),
                ter(tecDST_TAG_NEEDED));
        }
        {
            env(remit::remit(alice, bob, 1),
                remit::amts({ XRP(1) }),
                ter(tesSUCCESS));
        }
    }

    void
    testDisallowIncoming(FeatureBitset features)
    {
        testcase("disallow incoming");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        {
            Env env{*this, features - featureRemit};
            Account const alice{"alice"};
            env.fund(XRP(10000), alice);
            env(fset(alice, asfDisallowIncomingRemit));
            env.close();
            auto const sle = env.le(alice);
            uint32_t flags = sle->getFlags();
            BEAST_EXPECT(!(flags & lsfDisallowIncomingRemit));
        }

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        // set flag on bob only
        env(fset(bob, asfDisallowIncomingPayChan));
        env.close();

        // remit from alice to bob is disallowed
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tecNO_PERMISSION));
        }

        // set flag on alice also
        env(fset(alice, asfDisallowIncomingPayChan));
        env.close();

        // remit from bob to alice is now disallowed
        {
            env(remit::remit(bob, alice),
                remit::amts({XRP(1)}),
                ter(tecNO_PERMISSION));
        }

        // remove flag from bob
        env(fclear(bob, asfDisallowIncomingPayChan));
        env.close();

        // now remit between alice and bob allowed
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
        }

        // a remit from carol to alice isn't allowed
        {
            env(remit::remit(carol, alice),
                remit::amts({XRP(1)}),
                ter(tecNO_PERMISSION));
        }

        // remove flag from alice
        env(fclear(alice, asfDisallowIncomingPayChan));
        env.close();

        // now a remit from carol to alice is allowed
        {
            env(remit::remit(carol, alice),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(1000), alice, bob);
            env.close();

            env(remit::remit(alice, bob), ter(tesSUCCESS));
            env.close();
            // auto const preAlice = env.balance(alice, USD.issue());
        }

        // REMIT: XAH
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

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
                remit::token_ids({strHex(tid)}),
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

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
                remit::token_ids({strHex(tid)}),
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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + feeReserve);

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
                remit::token_ids({strHex(tid)}),
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
                remit::token_ids({strHex(tid)}),
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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - feeReserve);
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

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
                remit::token_ids({strHex(tid1)}),
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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(postBob == preBob + XRP(1) + (feeReserve * 2));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
                remit::token_ids({strHex(tid1)}),
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
                remit::token_ids({strHex(tid1)}),
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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - (feeReserve * 2));
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];

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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + feeReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }
    }

    void
    testDestDNETLDNE(FeatureBitset features)
    {
        testcase("dest does not exist and trustline does not exist");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // REMIT
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            env(remit::remit(alice, bob), ter(tesSUCCESS));
            env.close();

            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - feeDrops - accountReserve);
            BEAST_EXPECT(postBob == preBob + accountReserve);
        }

        // REMIT: XAH
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
            env.close();
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - accountReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + accountReserve);
        }

        // REMIT: USD
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.memoize(bob);

            env.fund(XRP(1000), alice, gw);
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

            BEAST_EXPECT(postAlice == preAlice - feeDrops - accountReserve - increment);
            BEAST_EXPECT(postBob == preBob + accountReserve + increment);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }

        // REMIT: XAH + USD
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.memoize(bob);

            env.fund(XRP(1000), alice, gw);
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
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - accountReserve - increment);
            BEAST_EXPECT(postBob == preBob + XRP(1) + accountReserve + increment);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));
        }

        // REMIT: URITOKEN XFER
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
            env.close();

            // mint uri token
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, tid));

            // setup
            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            // remit with uritoken id
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid)}),
                ter(tesSUCCESS));
            env.close();

            // validate
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - feeDrops - accountReserve - increment);
            BEAST_EXPECT(postBob == preBob + accountReserve + increment);

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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
            env.close();

            // setup
            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);
            env(remit::remit(alice, bob), remit::uri(uri), ter(tesSUCCESS));
            env.close();

            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(postAlice == preAlice - feeDrops - accountReserve - increment);
            BEAST_EXPECT(postBob == preBob + accountReserve + increment);
            
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
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
                remit::token_ids({strHex(tid)}),
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
            BEAST_EXPECT(postAlice == preAlice - XRP(1) - feeDrops - accountReserve - increment);
            BEAST_EXPECT(postBob == preBob + XRP(1) + accountReserve + increment);

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.memoize(bob);

            env.fund(XRP(1000), alice, gw);
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
                remit::token_ids({strHex(tid)}),
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
            BEAST_EXPECT(postAlice == preAlice - feeDrops - (increment * 2) - accountReserve);
            BEAST_EXPECT(postBob == preBob + (increment * 2) + accountReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: XAH/USD + URITOKEN XFER
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.memoize(bob);

            env.fund(XRP(1000), alice, gw);
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
                remit::token_ids({strHex(tid)}),
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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - (increment * 2) - accountReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + (increment * 2) + accountReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: XAH + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
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
                remit::token_ids({strHex(tid1)}),
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

            std::cout << "alice owner: " << ownerDirCount(*env.current(), alice) << "\n";
            std::cout << "bob owner: " << ownerDirCount(*env.current(), bob) << "\n";
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);

            // verify xah
            testDebug("POST", env, { alice, bob }, {});
            auto const postAlice = env.balance(alice);
            auto const postBob = env.balance(bob);
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - (increment * 2) - accountReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + (increment * 2) + accountReserve);

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }

        // REMIT: USD + URITOKEN XFER + URITOKEN MINT
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.memoize(bob);

            env.fund(XRP(1000), alice, gw);
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
                remit::amts({USD(1)}),
                remit::token_ids({strHex(tid1)}),
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
            BEAST_EXPECT(postAlice == preAlice - feeDrops - (increment * 3) - accountReserve);
            BEAST_EXPECT(postBob == preBob + (increment * 3) + accountReserve);
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
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const increment = env.current()->fees().increment;
            auto const accountReserve = env.current()->fees().accountReserve(0);

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const gw = Account("gw");
            auto const USD = gw["USD"];
            env.memoize(bob);

            env.fund(XRP(1000), alice, gw);
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
                remit::token_ids({strHex(tid1)}),
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
            BEAST_EXPECT(
                postAlice == preAlice - XRP(1) - feeDrops - (increment * 3) - accountReserve);
            BEAST_EXPECT(postBob == preBob + XRP(1) + (increment * 3) + accountReserve);
            BEAST_EXPECT(postAliceUSD == preAliceUSD - USD(1));
            BEAST_EXPECT(postBobUSD == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }
    }

    void
    testGateway(FeatureBitset features)
    {
        testcase("Gateway");
        using namespace test::jtx;
        using namespace std::literals;

        struct TestAccountData
        {
            Account src;
            Account dst;
            bool hasTrustline;
            bool negative;
        };

        std::array<TestAccountData, 8> gwSrcTests = {{
            // src > dst && src > issuer && dst no trustline
            {Account("gw0"), Account{"alice2"}, false, true},
            // // src < dst && src < issuer && dst no trustline
            {Account("gw1"), Account{"carol0"}, false, false},
            // // // // dst > src && dst > issuer && dst no trustline
            {Account("gw0"), Account{"dan1"}, false, true},
            // // // // dst < src && dst < issuer && dst no trustline
            {Account("gw1"), Account{"bob0"}, false, false},
            // // // src > dst && src > issuer && dst has trustline
            {Account("gw0"), Account{"alice2"}, true, true},
            // // // src < dst && src < issuer && dst has trustline
            {Account("gw1"), Account{"carol0"}, true, false},
            // // // dst > src && dst > issuer && dst has trustline
            {Account("gw0"), Account{"dan1"}, true, true},
            // // // dst < src && dst < issuer && dst has trustline
            {Account("gw1"), Account{"bob0"}, true, false},
        }};

        for (auto const& t : gwSrcTests)
        {
            Env env{*this, features};
            auto const USD = t.src["USD"];
            env.fund(XRP(5000), t.dst, t.src);
            env.close();

            if (t.hasTrustline)
                env.trust(USD(100000), t.dst);

            env.close();

            if (t.hasTrustline)
                env(pay(t.src, t.dst, USD(10000)));

            env.close();

            auto const preAmount = t.hasTrustline ? 10000 : 0;
            auto const preDst = lineBalance(env, t.dst, t.src, USD);

            // issuer can remit
            env(remit::remit(t.src, t.dst),
                remit::amts({ USD(100) }),
                ter(tesSUCCESS));
            env.close();

            auto const postAmount = t.hasTrustline ? 10100 : 100;
            BEAST_EXPECT(preDst == (t.negative ? -USD(preAmount) : USD(preAmount)));
            BEAST_EXPECT(
                lineBalance(env, t.dst, t.src, USD) ==
                (t.negative ? -USD(postAmount) : USD(postAmount)));
            BEAST_EXPECT(lineBalance(env, t.src, t.src, USD) == USD(0));
        }

        // std::array<TestAccountData, 4> gwDstTests = {{
        //     // // // src > dst && src > issuer && dst has trustline
        //     {Account("alice2"), Account{"gw0"}, true, true},
        //     // // // src < dst && src < issuer && dst has trustline
        //     {Account("carol0"), Account{"gw1"}, true, false},
        //     // // // // dst > src && dst > issuer && dst has trustline
        //     {Account("dan1"), Account{"gw0"}, true, true},
        //     // // // // dst < src && dst < issuer && dst has trustline
        //     {Account("bob0"), Account{"gw1"}, true, false},
        // }};

        // for (auto const& t : gwDstTests)
        // {
        //     Env env{*this, features};
        //     auto const USD = t.dst["USD"];
        //     env.fund(XRP(5000), t.src, t.dst);
        //     env.close();

        //     env.trust(USD(100000), t.src);
        //     env.close();

        //     env(pay(t.dst, t.src, USD(10000)));
        //     env.close();

        //     // src can create paychan to dst/issuer
        //     auto const pk = t.src.pk();
        //     auto const settleDelay = 100s;
        //     auto const chan = channel(t.src, t.dst, env.seq(t.src));
        //     env(paychan::create(t.src, t.dst, USD(1000), settleDelay, pk));
        //     env.close();

        //     // dst/gw can claim paychan
        //     auto const preSrc = lineBalance(env, t.src, t.dst, USD);
        //     auto chanBal = channelBalance(*env.current(), chan);
        //     auto chanAmt = channelAmount(*env.current(), chan);
        //     auto const delta = USD(500);
        //     auto const reqBal = chanBal + delta;
        //     auto const authAmt = reqBal + USD(100);
        //     auto const sig =
        //         signClaimIOUAuth(t.src.pk(), t.src.sk(), chan, authAmt);
        //     env(paychan::claim(
        //         t.dst, chan, reqBal, authAmt, Slice(sig), t.src.pk()));
        //     env.close();
        //     auto const preAmount = 10000;
        //     BEAST_EXPECT(
        //         preSrc == (t.negative ? -USD(preAmount) : USD(preAmount)));
        //     auto const postAmount = 9500;
        //     BEAST_EXPECT(
        //         lineBalance(env, t.src, t.dst, USD) ==
        //         (t.negative ? -USD(postAmount) : USD(postAmount)));
        //     BEAST_EXPECT(lineBalance(env, t.dst, t.dst, USD) == USD(0));
        // }
    }

    void
    testRequireAuth(FeatureBitset features)
    {
        testcase("Require Auth");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];

        auto const aliceUSD = alice["USD"];
        auto const bobUSD = bob["USD"];

        // test asfRequireAuth
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, gw);
            env(fset(gw, asfRequireAuth));
            env.close();
            env(trust(gw, aliceUSD(10000)), txflags(tfSetfAuth));
            env(trust(alice, USD(10000)));
            env.close();
            env(pay(gw, alice, USD(1000)));
            env.close();

            // alice cannot remit because bob's trustline is not authorized
            // all parties must be authorized
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tecNO_AUTH));
            env.close();

            env(trust(gw, bobUSD(10000)), txflags(tfSetfAuth));
            env(trust(bob, USD(10000)));
            env.close();
            env(pay(gw, bob, USD(1000)));
            env.close();

            // alice can now remit because bob's trustline is authorized
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testTLFreeze(FeatureBitset features)
    {
        testcase("Trustline Freeze");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];

        auto const aliceUSD = alice["USD"];
        auto const bobUSD = bob["USD"];
        // test Global Freeze
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice);
            env.trust(USD(100000), bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();
            
            env(fset(gw, asfGlobalFreeze));
            env.close();

            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tecFROZEN));
            env.close();
            
            env(fclear(gw, asfGlobalFreeze));
            env.close();
            
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tesSUCCESS));
            env.close();
        }

        // test Individual Freeze
        {
            // Env Setup
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob, gw);
            env.close();
            env(trust(alice, USD(100000)));
            env(trust(bob, USD(100000)));
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // set freeze on alice trustline
            env(trust(gw, USD(100000), alice, tfSetFreeze));
            env.close();

            // remit fails - frozen trustline
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tecFROZEN));
            env.close();

            // clear freeze on alice trustline
            env(trust(gw, USD(100000), alice, tfClearFreeze));
            env.close();

            // remit success
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1)}),
                ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testEnabled(features);
        // testPreflightInvalid(features);
        // testDoApplyInvalid(features);
        // testDisallowXRP(features);
        // testDstTag(features);
        // testDisallowIncoming(features);
        // testDestExistsTLExists(features);
        // testDestExistsTLDNE(features);
        // testDestDNETLDNE(features);
        testGateway(features);
        // testRequireAuth(features);
        // testTLFreeze(features);
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
