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

    static bool
    validateSequence(
        jtx::Env const& env,
        jtx::Account const& account,
        std::uint32_t const& sequence)
    {
        auto const sle = env.le(keylet::account(account));
        if (sle && sle->isFieldPresent(sfSequence))
            return (*sle)[sfSequence] == sequence;
        return false;
    }

    static std::pair<uint256, std::shared_ptr<SLE const>>
    uriTokenKeyAndSle(
        ReadView const& view,
        jtx::Account const& account,
        std::string const& uri)
    {
        auto const k = keylet::uritoken(account, Blob(uri.begin(), uri.end()));
        return {k.key, view.read(k)};
    }

    void
    testEnabled(FeatureBitset features)
    {
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

        // temMALFORMED - blob was more than 128kib
        {
            ripple::Blob blob;
            blob.resize(129 * 1024);
            env(remit::remit(alice, bob),
                remit::blob(strHex(blob)),
                ter(temMALFORMED));
        }

        // temMALFORMED - AmountEntrys Exceeds Limit
        {
            std::vector<STAmount> amts_;  // Remove the const qualifier
            for (size_t i = 0; i < 33; i++)
            {
                auto const USD = gw["USD"];
                amts_.emplace_back(USD(1));
            }

            env(remit::remit(alice, bob),
                remit::amts(amts_),
                ter(temMALFORMED));
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

            // not adjacent
            env(remit::remit(alice, bob),
                remit::amts({XRP(1), USD(1), XRP(1)}),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Issued Currency appears more than once.
        {
            env(remit::remit(alice, bob),
                remit::amts({USD(1), USD(1)}),
                ter(temMALFORMED));
            env.close();

            // not adjacent
            env(remit::remit(alice, bob),
                remit::amts({USD(1), XRP(1), USD(1)}),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - URI field was not provided.
        // DA: Template/Submit Failure
        // {
        //     auto tx = remit::remit(alice, bob);
        //     tx[sfMintURIToken.jsonName] = Json::Value{};
        //     tx[sfMintURIToken.jsonName][sfFlags.fieldName] = 1;
        //     env(tx, ter(temMALFORMED));
        //     env.close();
        // }

        // temMALFORMED - Field found in disallowed location.
        // DA: Template/Submit Failure
        // {
        //     std::string const uri(0, '?');
        //     auto tx = remit::remit(alice, bob);
        //     tx[sfMintURIToken.jsonName] = Json::Value{};
        //     std::string const digestval =
        //     "C16E7263F07AA41261DCC955660AF4646ADBA414E37B6F5A5BA50F75153F5CCC";
        //     tx[sfMintURIToken.jsonName][sfURI.fieldName] = strHex(uri);
        //     tx[sfMintURIToken.jsonName][sfHookOn.fieldName] = digestval;
        //     env(tx, ter(temMALFORMED));
        //     env.close();
        // }

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

        // temMALFORMED - URITokenIDs < 1
        {
            std::vector<std::string> token_ids;
            env(remit::remit(alice, bob),
                remit::token_ids(token_ids),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - URITokenIDs Exceeds Limit
        {
            std::vector<std::string> token_ids;
            for (size_t i = 0; i < 33; i++)
            {
                std::string const uri(i, '?');
                auto const tid = uritoken::tokenid(alice, uri);
                token_ids.emplace_back(strHex(tid));
            }

            env(remit::remit(alice, bob),
                remit::token_ids(token_ids),
                ter(temMALFORMED));
            env.close();
        }

        // temMALFORMED - Duplicate URITokenID.
        {
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid1), strHex(tid1)}),
                ter(temMALFORMED));
            env.close();

            // not adj
            std::string const uri2(maxTokenURILength, 'a');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid1), strHex(tid2), strHex(tid1)}),
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
        {
            auto const carol = Account("carol");
            Env env{*this, features};
            env.memoize(carol);
            auto tx = remit::remit(carol, bob);
            tx[jss::Sequence] = 0;
            env(tx, carol, ter(terNO_ACCOUNT));
            env.close();
        }

        // tecNO_TARGET - inform acct doesnt exist
        {
            auto const carol = Account("carol");
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            env.memoize(carol);
            auto tx = remit::remit(alice, bob);
            tx[sfInform.jsonName] = carol.human();
            env(tx, alice, ter(tecNO_TARGET));
            env.close();
        }

        // tecNO_PERMISSION - lsfDisallowIncomingRemit
        // DA: see testAllowIncoming

        // tecDST_TAG_NEEDED
        // DA: see testDstTag

        // tecDUPLICATE
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob);
            env.close();

            std::string const uri(maxTokenURILength, '?');
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));
            env(remit::remit(alice, bob), remit::uri(uri), ter(tecDUPLICATE));
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
        testcase("disallow xrp");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        {
            // Make a remit where dst disallows XRP
            Env env(*this, features - featureDepositAuth);
            env.fund(XRP(10000), alice, bob);
            env(fset(bob, asfDisallowXRP));
            env.close();

            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
        }
        {
            // Make a remit where dst disallows XRP.  Ignore that flag,
            // since it's just advisory.
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob);
            env(fset(bob, asfDisallowXRP));
            env.close();
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
        }
    }

    void
    testDstTag(FeatureBitset features)
    {
        // auth amount defaults to balance if not present
        testcase("dest tag");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(10000), alice, bob);
        env(fset(bob, asfRequireDest));
        env.close();
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tecDST_TAG_NEEDED));
        }
        {
            env(remit::remit(alice, bob, 1),
                remit::amts({XRP(1)}),
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
        env(fset(bob, asfDisallowIncomingRemit));
        env.close();

        // remit from alice to bob is not allowed
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tecNO_PERMISSION));
        }

        // set flag on alice also
        env(fset(alice, asfDisallowIncomingRemit));
        env.close();

        // remit from bob to alice is not allowed
        {
            env(remit::remit(bob, alice),
                remit::amts({XRP(1)}),
                ter(tecNO_PERMISSION));
        }

        // remove flag from bob
        env(fclear(bob, asfDisallowIncomingRemit));
        env.close();

        // now remit between alice and bob allowed
        {
            env(remit::remit(alice, bob),
                remit::amts({XRP(1)}),
                ter(tesSUCCESS));
        }

        // a remit from carol to alice is not allowed
        {
            env(remit::remit(carol, alice),
                remit::amts({XRP(1)}),
                ter(tecNO_PERMISSION));
        }

        // remove flag from alice
        env(fclear(alice, asfDisallowIncomingRemit));
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

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            env(remit::remit(alice, bob), ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(env.balance(alice) == preAlice - feeDrops);
            BEAST_EXPECT(env.balance(bob) == preBob);
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
            BEAST_EXPECT(env.balance(alice) == preAlice - XRP(1) - feeDrops);
            BEAST_EXPECT(env.balance(bob) == preBob + XRP(1));
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
            BEAST_EXPECT(env.balance(alice) == preAlice - feeDrops);
            BEAST_EXPECT(env.balance(bob) == preBob);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));
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
            BEAST_EXPECT(env.balance(alice) == preAlice - XRP(1) - feeDrops);
            BEAST_EXPECT(env.balance(bob) == preBob + XRP(1));
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));
        }

        // REMIT: URITOKEN XFER
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
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, tid));

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            // remit with uritoken id
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid)}),
                ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());

            BEAST_EXPECT(
                env.balance(alice) == preAlice - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + feeReserve);

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // REMIT: URITOKEN MINT
        {
            // setup env
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;
            auto const feeReserve = env.current()->fees().increment;

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(1000), alice, bob);
            env.close();

            auto const preAlice = env.balance(alice);
            auto const preBob = env.balance(bob);

            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);

            env(remit::remit(alice, bob), remit::uri(uri), ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid));
            BEAST_EXPECT(tokenOwner(*env.current(), tid) == bob.id());
            BEAST_EXPECT(tokenIsser(*env.current(), tid) == alice.id());

            BEAST_EXPECT(
                env.balance(alice) == preAlice - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + feeReserve);

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + XRP(1) + feeReserve);

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
            BEAST_EXPECT(
                env.balance(alice) == preAlice - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + feeReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + XRP(1) + feeReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(
                env.balance(bob) == preBob + XRP(1) + (feeReserve * 2));

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
            BEAST_EXPECT(
                env.balance(alice) == preAlice - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(env.balance(bob) == preBob + (feeReserve * 2));
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - (feeReserve * 2));
            BEAST_EXPECT(
                env.balance(bob) == preBob + XRP(1) + (feeReserve * 2));
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }
    }

    void
    testDestExistsTLNotExist(FeatureBitset features)
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
            BEAST_EXPECT(
                env.balance(alice) == preAlice - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + feeReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));
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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - feeReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + XRP(1) + feeReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));
        }
    }

    void
    testDestNotExistTLNotExist(FeatureBitset features)
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

            bool const withXahau =
                env.current()->rules().enabled(featureXahauGenesis);
            BEAST_EXPECT(validateSequence(
                env, bob, withXahau ? 10 : env.closed()->info().seq));

            BEAST_EXPECT(
                env.balance(alice) == preAlice - feeDrops - accountReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + accountReserve);
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

            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - accountReserve);
            BEAST_EXPECT(env.balance(bob) == preBob + XRP(1) + accountReserve);
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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - feeDrops - accountReserve - increment);
            BEAST_EXPECT(
                env.balance(bob) == preBob + accountReserve + increment);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));
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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - accountReserve - increment);
            BEAST_EXPECT(
                env.balance(bob) ==
                preBob + XRP(1) + accountReserve + increment);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));
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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - feeDrops - accountReserve - increment);
            BEAST_EXPECT(
                env.balance(bob) == preBob + accountReserve + increment);

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

            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - feeDrops - accountReserve - increment);
            BEAST_EXPECT(
                env.balance(bob) == preBob + accountReserve + increment);

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - accountReserve - increment);
            BEAST_EXPECT(
                env.balance(bob) ==
                preBob + XRP(1) + accountReserve + increment);

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - feeDrops - (increment * 2) - accountReserve);
            BEAST_EXPECT(
                env.balance(bob) == preBob + (increment * 2) + accountReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - (increment * 2) -
                    accountReserve);
            BEAST_EXPECT(
                env.balance(bob) ==
                preBob + XRP(1) + (increment * 2) + accountReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

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

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);

            // verify xah
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - (increment * 2) -
                    accountReserve);
            BEAST_EXPECT(
                env.balance(bob) ==
                preBob + XRP(1) + (increment * 2) + accountReserve);

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - feeDrops - (increment * 3) - accountReserve);
            BEAST_EXPECT(
                env.balance(bob) == preBob + (increment * 3) + accountReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

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
            BEAST_EXPECT(
                env.balance(alice) ==
                preAlice - XRP(1) - feeDrops - (increment * 3) -
                    accountReserve);
            BEAST_EXPECT(
                env.balance(bob) ==
                preBob + XRP(1) + (increment * 3) + accountReserve);
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(1));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(1));

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }
    }

    void
    testGateway(FeatureBitset features)
    {
        testcase("gateway");
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
            {
                env.trust(USD(100000), t.dst);
                env.close();
            }

            if (t.hasTrustline)
            {
                env(pay(t.src, t.dst, USD(10000)));
                env.close();
            }

            auto const preAmount = t.hasTrustline ? 10000 : 0;
            auto const preDst = lineBalance(env, t.dst, t.src, USD);

            // issuer can remit
            env(remit::remit(t.src, t.dst),
                remit::amts({USD(100)}),
                ter(tesSUCCESS));
            env.close();

            auto const postAmount = t.hasTrustline ? 10100 : 100;
            BEAST_EXPECT(
                preDst == (t.negative ? -USD(preAmount) : USD(preAmount)));
            BEAST_EXPECT(
                lineBalance(env, t.dst, t.src, USD) ==
                (t.negative ? -USD(postAmount) : USD(postAmount)));
            BEAST_EXPECT(lineBalance(env, t.src, t.src, USD) == USD(0));
        }

        std::array<TestAccountData, 8> gwDstTests = {{
            // // // src > dst && src > issuer && dst has trustline
            {Account("alice2"), Account{"gw0"}, true, true},
            // // // src > dst && src > issuer && dst has trustline
            {Account("alice2"), Account{"gw0"}, false, true},
            // // // src < dst && src < issuer && dst has trustline
            {Account("carol0"), Account{"gw1"}, true, false},
            // // // src < dst && src < issuer && dst has trustline
            {Account("carol0"), Account{"gw1"}, false, false},
            // // // // dst > src && dst > issuer && dst has trustline
            {Account("dan1"), Account{"gw0"}, true, true},
            // // // // dst > src && dst > issuer && dst has trustline
            {Account("dan1"), Account{"gw0"}, false, true},
            // // // // dst < src && dst < issuer && dst has trustline
            {Account("bob0"), Account{"gw1"}, true, false},
            // // // // dst < src && dst < issuer && dst has trustline
            {Account("bob0"), Account{"gw1"}, false, false},
        }};

        for (auto const& t : gwDstTests)
        {
            Env env{*this, features};
            auto const USD = t.dst["USD"];
            env.fund(XRP(5000), t.src, t.dst);
            env.close();

            if (t.hasTrustline)
            {
                env.trust(USD(100000), t.src);
                env.close();
            }

            if (t.hasTrustline)
            {
                env(pay(t.dst, t.src, USD(10000)));
                env.close();
            }

            auto const preSrc = lineBalance(env, t.src, t.dst, USD);

            // src can remit to issuer
            env(remit::remit(t.src, t.dst),
                remit::amts({USD(100)}),
                t.hasTrustline ? ter(tesSUCCESS) : ter(tecUNFUNDED_PAYMENT));
            env.close();

            if (t.hasTrustline)
            {
                auto const preAmount = 10000;
                BEAST_EXPECT(
                    preSrc == (t.negative ? -USD(preAmount) : USD(preAmount)));
                auto const postAmount = 9900;
                BEAST_EXPECT(
                    lineBalance(env, t.src, t.dst, USD) ==
                    (t.negative ? -USD(postAmount) : USD(postAmount)));
                BEAST_EXPECT(lineBalance(env, t.dst, t.dst, USD) == USD(0));
            }
            else
            {
                BEAST_EXPECT(preSrc == USD(0));
                BEAST_EXPECT(lineBalance(env, t.src, t.dst, USD) == USD(0));
                BEAST_EXPECT(lineBalance(env, t.dst, t.dst, USD) == USD(0));
            }
        }
    }

    void
    testTransferRate(FeatureBitset features)
    {
        testcase("transfer rate");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];

        struct TestRateData
        {
            double rate;
            STAmount delta;
            std::string result;
            TER code;
        };
        std::array<TestRateData, 10> testCases = {{
            {0.0, USD(100), "900", tesSUCCESS},
            {-1.0, USD(100), "900", temBAD_TRANSFER_RATE},
            {0.9, USD(100), "900", temBAD_TRANSFER_RATE},
            {1.0, USD(100), "900", tesSUCCESS},
            {1.1, USD(100), "890", tesSUCCESS},
            {1.0005, USD(100), "899.95", tesSUCCESS},
            {1.005, USD(100), "899.5000001", tesSUCCESS},
            {1.25, USD(100), "875", tesSUCCESS},
            {2.0, USD(100), "800", tesSUCCESS},
            {2.1, USD(100), "900", temBAD_TRANSFER_RATE},
        }};

        for (auto const& tc : testCases)
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob, gw);
            env(rate(gw, tc.rate), ter(tc.code));
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            auto const preBob = env.balance(bob, USD.issue());

            auto const delta = USD(100);
            env(remit::remit(alice, bob), remit::amts({delta}));
            env.close();
            auto const postAlice = env.balance(alice, USD.issue());
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob + delta);
            BEAST_EXPECT(to_string(postAlice.value()) == tc.result);
        }

        // test rate change
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice, bob, gw);
            env(rate(gw, 1.25));
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            // setup
            auto const delta = USD(100);
            auto preAlice = env.balance(alice, USD.issue());
            auto preBob = env.balance(bob, USD.issue());

            // remit
            env(remit::remit(alice, bob), remit::amts({delta}));
            env.close();
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAlice - delta - USD(25));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob + delta);

            // issuer changes rate lower
            env(rate(gw, 1.00));
            env.close();

            preAlice = env.balance(alice, USD.issue());
            preBob = env.balance(bob, USD.issue());

            // remit
            env(remit::remit(alice, bob), remit::amts({delta}));
            env.close();
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice - delta);
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob + delta);
        }

        // test issuer doesnt pay own rate
        {
            Env env{*this, features};
            env.fund(XRP(10000), alice, gw);
            env(rate(gw, 1.25));
            env.close();
            env.trust(USD(100000), alice);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env.close();

            auto const delta = USD(100);
            auto const preAlice = env.balance(alice, USD.issue());

            // remit
            env(remit::remit(gw, alice), remit::amts({delta}));
            env.close();

            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice + delta);
        }
    }

    void
    testRequireAuth(FeatureBitset features)
    {
        testcase("require auth");
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
    testDepositAuth(FeatureBitset features)
    {
        testcase("deposit authorization");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];

        {
            Env env{*this, features};
            auto const feeDrops = env.current()->fees().base;

            env.fund(XRP(10000), alice, bob, gw);
            env.close();
            env.trust(USD(100000), alice, bob);
            env.close();
            env(pay(gw, alice, USD(10000)));
            env(pay(gw, bob, USD(10000)));
            env.close();

            auto const preBobXrp = env.balance(bob);
            auto const preBobUSD = env.balance(bob, USD.issue());
            auto const preAliceXrp = env.balance(alice);
            auto const preAliceUSD = env.balance(alice, USD.issue());

            env(fset(bob, asfDepositAuth));
            env.close();

            // Since alice is not preauthorized, remit fails.
            env(remit::remit(alice, bob),
                remit::amts({USD(100)}),
                ter(tecNO_PERMISSION));
            env.close();

            // Bob preauthorizes alice for deposit, remit success.
            env(deposit::auth(bob, alice));
            env.close();

            env(remit::remit(alice, bob),
                remit::amts({USD(100)}),
                ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob) == preBobXrp - (2 * feeDrops));
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(100));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(100));

            // bob removes preauthorization of alice.
            env(deposit::unauth(bob, alice));
            env.close();

            // alice remits and fails since she is no longer preauthorized.
            env(remit::remit(alice, bob),
                remit::amts({USD(100)}),
                ter(tecNO_PERMISSION));
            env.close();

            // bob clears lsfDepositAuth.
            env(fclear(bob, asfDepositAuth));
            env.close();

            // alice remits successfully.
            env(remit::remit(alice, bob),
                remit::amts({USD(100)}),
                ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (4 * feeDrops));
            BEAST_EXPECT(env.balance(bob) == preBobXrp - (4 * feeDrops));
            BEAST_EXPECT(
                env.balance(alice, USD.issue()) == preAliceUSD - USD(200));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBobUSD + USD(200));
        }
    }

    void
    testTLFreeze(FeatureBitset features)
    {
        testcase("trustline freeze");
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
    validateNoRipple(
        jtx::Env& env,
        jtx::Account const& acct,
        jtx::Account const& peer,
        bool const& result)
    {
        Json::Value params;
        params[jss::account] = acct.human();
        params[jss::peer] = peer.human();

        auto lines = env.rpc("json", "account_lines", to_string(params));
        auto const& line = lines[jss::result][jss::lines][0u];
        BEAST_EXPECT(line[jss::no_ripple_peer].asBool() == result);
    }

    void
    testRippling(FeatureBitset features)
    {
        testcase("rippling");
        using namespace test::jtx;
        using namespace std::literals;

        // rippling enabled
        {
            Env env{*this, features};
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const USDA = alice["USD"];
            auto const USDB = bob["USD"];
            auto const USDC = carol["USD"];
            env.fund(XRP(10000), alice, bob, carol);
            env.close();

            // alice trusts USD bob & carol
            env(trust(alice, USDB(100)));
            env(trust(alice, USDC(100)));
            // bob trusts USD alice & carol
            env(trust(bob, USDA(100)));
            env(trust(bob, USDC(100)));
            // carol trusts USD alice & bob
            env(trust(carol, USDA(100)));
            env(trust(carol, USDB(100)));
            env.close();
            // alice pays bob USDA
            env(pay(alice, bob, USDA(10)));
            // carol pays alice USDC
            env(pay(carol, alice, USDC(10)));
            env.close();

            BEAST_EXPECT(env.balance(alice, USDA) == USDA(0));
            BEAST_EXPECT(env.balance(alice, USDB) == USDB(-10));
            BEAST_EXPECT(env.balance(alice, USDC) == USDC(10));
            BEAST_EXPECT(env.balance(bob, USDA) == USDA(10));
            BEAST_EXPECT(env.balance(bob, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(bob, USDC) == USDC(0));
            BEAST_EXPECT(env.balance(carol, USDA) == USDA(-10));
            BEAST_EXPECT(env.balance(carol, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(carol, USDC) == USDC(0));

            validateNoRipple(env, alice, bob, false);
            validateNoRipple(env, alice, carol, false);
            validateNoRipple(env, bob, alice, false);
            validateNoRipple(env, bob, carol, false);
            validateNoRipple(env, carol, alice, false);
            validateNoRipple(env, carol, bob, false);

            // alice cannot create to carol with USDB
            env(remit::remit(alice, carol),
                remit::amts({USDB(10)}),
                ter(tecUNFUNDED_PAYMENT));
            env.close();

            // negative direction destination
            // bob can remit to carol with USDA
            env(remit::remit(bob, carol),
                remit::amts({USDA(10)}),
                ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(env.balance(alice, USDA) == USDA(0));
            BEAST_EXPECT(env.balance(alice, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(alice, USDC) == USDC(0));
            BEAST_EXPECT(env.balance(bob, USDA) == USDA(0));
            BEAST_EXPECT(env.balance(bob, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(bob, USDC) == USDC(0));
            BEAST_EXPECT(env.balance(carol, USDA) == USDA(0));
            BEAST_EXPECT(env.balance(carol, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(carol, USDC) == USDC(0));
        }

        // rippling not enabled
        {
            Env env{*this, features};
            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            auto const USDA = alice["USD"];
            auto const USDB = bob["USD"];
            auto const USDC = carol["USD"];
            env.fund(XRP(10000), alice, bob, carol);
            env.close();

            // alice trusts USD bob & carol
            env(trust(alice, USDB(100), bob, tfSetNoRipple));
            env(trust(alice, USDC(100), carol, tfSetNoRipple));
            // bob trusts USD alice & carol
            env(trust(bob, USDA(100), alice, tfSetNoRipple));
            env(trust(bob, USDC(100), carol, tfSetNoRipple));
            // carol trusts USD alice & bob
            env(trust(carol, USDA(100), alice, tfSetNoRipple));
            env(trust(carol, USDB(100), bob, tfSetNoRipple));
            env.close();
            // alice pays bob USDA
            env(pay(alice, bob, USDA(10)));
            // carol pays alice USDC
            env(pay(carol, alice, USDC(10)));
            env.close();

            BEAST_EXPECT(env.balance(alice, USDA) == USDA(0));
            BEAST_EXPECT(env.balance(alice, USDB) == USDB(-10));
            BEAST_EXPECT(env.balance(alice, USDC) == USDC(10));
            BEAST_EXPECT(env.balance(bob, USDA) == USDA(10));
            BEAST_EXPECT(env.balance(bob, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(bob, USDC) == USDC(0));
            BEAST_EXPECT(env.balance(carol, USDA) == USDA(-10));
            BEAST_EXPECT(env.balance(carol, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(carol, USDC) == USDC(0));

            validateNoRipple(env, alice, bob, true);
            validateNoRipple(env, alice, carol, true);
            validateNoRipple(env, bob, alice, true);
            validateNoRipple(env, bob, carol, true);
            validateNoRipple(env, carol, alice, true);
            validateNoRipple(env, carol, bob, true);

            // alice cannot create to carol with USDB
            env(remit::remit(alice, carol),
                remit::amts({USDB(10)}),
                ter(tecPATH_DRY));
            env.close();

            // negative direction destination
            // bob can not remit to carol with USDA
            env(remit::remit(bob, carol),
                remit::amts({USDA(10)}),
                ter(tecPATH_DRY));
            env.close();

            BEAST_EXPECT(env.balance(alice, USDA) == USDA(0));
            BEAST_EXPECT(env.balance(alice, USDB) == USDB(-10));
            BEAST_EXPECT(env.balance(alice, USDC) == USDC(10));
            BEAST_EXPECT(env.balance(bob, USDA) == USDA(10));
            BEAST_EXPECT(env.balance(bob, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(bob, USDC) == USDC(0));
            BEAST_EXPECT(env.balance(carol, USDA) == USDA(-10));
            BEAST_EXPECT(env.balance(carol, USDB) == USDB(0));
            BEAST_EXPECT(env.balance(carol, USDC) == USDC(0));
        }
    }

    void
    testURIToken(FeatureBitset features)
    {
        testcase("uritoken");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");

        Env env{*this, features};
        env.fund(XRP(1000), alice, bob);
        env.close();

        // cannot mint and transfer same token
        {
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);

            env(remit::remit(alice, bob),
                remit::uri(uri),
                remit::token_ids({strHex(tid)}),
                ter(tecNO_PERMISSION));
            env.close();
        }

        // mint and xfer in same ledger
        {
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);

            // mint uritoken
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));

            // remit
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

        // confirm offer (amount/dest) is removed on xfer
        {
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);

            // mint uritoken
            env(uritoken::mint(alice, uri), ter(tesSUCCESS));

            // sell offer
            env(uritoken::sell(alice, strHex(tid)),
                uritoken::amt(XRP(1)),
                uritoken::dest(bob),
                ter(tesSUCCESS));
            env.close();

            // verify amount and destination
            auto const [urikey1, uriSle1] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uriSle1->getAccountID(sfDestination) == bob.id());
            BEAST_EXPECT((*uriSle1)[sfAmount] == XRP(1));

            // xfer the uritoken
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid)}),
                ter(tesSUCCESS));
            env.close();

            // verify amount and destination was removed
            auto const [urikey2, uriSle2] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uriSle2->isFieldPresent(sfDestination) == false);
            BEAST_EXPECT(uriSle2->isFieldPresent(sfAmount) == false);

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // test digest
        {
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(alice, uri);

            std::string const digestval =
                "C16E7263F07AA41261DCC955660AF4646ADBA414E37B6F5A5BA50F75153F5C"
                "CC";

            // mint the uritoken w/ digest
            env(remit::remit(alice, bob),
                remit::uri(uri, 0, digestval),
                ter(tesSUCCESS));
            env.close();

            auto const [urikey, uriSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(
                to_string(uriSle->getFieldH256(sfDigest)) == digestval);

            // clean up test
            env(uritoken::burn(bob, strHex(tid)));
            env.close();
        }

        // test xfer multiple
        {
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            std::string const uri2(maxTokenURILength - 1, '?');
            auto const tid2 = uritoken::tokenid(alice, uri2);
            env(uritoken::mint(alice, uri2), ter(tesSUCCESS));
            env.close();

            // xfer multiple tokens
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid1), strHex(tid2)}),
                ter(tesSUCCESS));
            env.close();

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid2));
            BEAST_EXPECT(tokenOwner(*env.current(), tid2) == bob.id());

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env(uritoken::burn(bob, strHex(tid2)));
            env.close();
        }

        // test sell/xfer in same ledger
        {
            std::string const uri1(maxTokenURILength, '?');
            auto const tid1 = uritoken::tokenid(alice, uri1);

            // mint uritoken
            env(uritoken::mint(alice, uri1), ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(alice, strHex(tid1)),
                uritoken::amt(XRP(1)),
                uritoken::dest(bob),
                ter(tesSUCCESS));

            // buy uritoken
            env(uritoken::buy(bob, strHex(tid1)),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // xfer uritoken
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid1)}),
                ter(tecNO_PERMISSION));
            env.close();

            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            BEAST_EXPECT(inOwnerDir(*env.current(), bob, tid1));
            BEAST_EXPECT(tokenOwner(*env.current(), tid1) == bob.id());

            // clean up test
            env(uritoken::burn(bob, strHex(tid1)));
            env.close();
        }
    }

    void
    testOptionals(FeatureBitset features)
    {
        testcase("optionals");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");

        Env env{*this, features};
        auto const feeDrops = env.current()->fees().base;

        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        // inform
        {
            env(remit::remit(alice, bob),
                remit::inform(carol),
                ter(tesSUCCESS));
            env.close();
        }

        // blob
        {
            ripple::Blob blob;
            blob.resize(128 * 1024);
            XRPAmount const extraFee =
                XRPAmount{static_cast<XRPAmount>(blob.size())};
            env(remit::remit(alice, bob),
                remit::blob(strHex(blob)),
                fee(feeDrops + extraFee),
                ter(tesSUCCESS));
            env.close();
        }

        // invoice
        {
            env(remit::remit(alice, bob),
                invoice_id(uint256{4}),
                ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testPreflightInvalid(features);
        testDoApplyInvalid(features);
        testDisallowXRP(features);
        testDstTag(features);
        testDisallowIncoming(features);
        testDestExistsTLExists(features);
        testDestExistsTLNotExist(features);
        testDestNotExistTLNotExist(features);
        testGateway(features);
        testTransferRate(features);
        testRequireAuth(features);
        testDepositAuth(features);
        testTLFreeze(features);
        testRippling(features);
        testURIToken(features);
        testOptionals(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa - featureXahauGenesis);
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(Remit, app, ripple);
}  // namespace test
}  // namespace ripple
