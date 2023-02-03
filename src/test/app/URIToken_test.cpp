//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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
#include <ripple/ledger/Directory.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
// #include <ripple/protocol/URIToken.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

#include <chrono>

namespace ripple {
namespace test {
struct URIToken_test : public beast::unit_test::suite
{
    static uint256
    tokenid(jtx::Account const& account, std::string const& uri)
    {
        auto const k = keylet::uritoken(account, Blob(uri.begin(), uri.end()));
        return k.key;
    }

    static bool
    inOwnerDir(
        ReadView const& view,
        jtx::Account const& acct,
        std::shared_ptr<SLE const> const& token)
    {
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::find(ownerDir.begin(), ownerDir.end(), token) !=
            ownerDir.end();
    }

    static std::size_t
    ownerDirCount(ReadView const& view, jtx::Account const& acct)
    {
        ripple::Dir const ownerDir(view, keylet::ownerDir(acct.id()));
        return std::distance(ownerDir.begin(), ownerDir.end());
    };

    static std::pair<uint256, std::shared_ptr<SLE const>>
    uriTokenKeyAndSle(
        ReadView const& view,
        jtx::Account const& account,
        std::string const& uri)
    {
        auto const k = keylet::uritoken(account, Blob(uri.begin(), uri.end()));
        return {k.key, view.read(k)};
    }

    static bool
    uritokenExists(ReadView const& view, uint256 const& id)
    {
        auto const slep = view.read({ltURI_TOKEN, id});
        return bool(slep);
    }

    void
    debugBalance(
        jtx::Env const& env,
        std::string const& name,
        jtx::Account const& account,
        jtx::IOU const& iou)
    {
        // debugBalance(env, "alice", alice, USD);
        std::cout << name << " BALANCE XRP: " << env.balance(account) << "\n";
        std::cout << name
                  << " BALANCE USD: " << env.balance(account, iou.issue())
                  << "\n";
    }

    void
    debugToken(
        ReadView const& view,
        std::string const& name,
        jtx::Account const& account,
        uint256 const& id)
    {
        // debugToken(*env.current(), "token", alice, tid);
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return;

        std::cout << name << " sfOwner: " << slep->getAccountID(sfOwner)
                  << "\n";
        std::cout << name << " sfIssuer: " << slep->getAccountID(sfIssuer)
                  << "\n";
        // std::cout << name << " sfURI: " << slep->getFieldVL(sfURI) << "\n";
        // std::cout << name << " sfDigest: " << (*slep)[sfDigest] << "\n";
        if (slep->getFieldAmount(sfAmount))
            std::cout << name << " sfAmount: " << slep->getFieldAmount(sfAmount)
                      << "\n";

        if (slep->getFieldAmount(sfAmount))
            std::cout << name
                      << " sfDestination: " << slep->getAccountID(sfDestination)
                      << "\n";
    }

    static AccountID
    tokenOwner(ReadView const& view, uint256 const& id)
    {
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return AccountID();
        return slep->getAccountID(sfOwner);
    }

    static STAmount
    tokenAmount(ReadView const& view, uint256 const& id)
    {
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return XRPAmount{-1};
        if (slep->getFieldAmount(sfAmount))
            return (*slep)[sfAmount];
        return XRPAmount{-1};
    }

    // void
    // debugOwnerDir(
    //     jtx::Env const& env,
    //     std::string const& name,
    //     jtx::Account const& account,
    //     std::string const& uri)
    // {
    //     auto const [urit, uritSle] = uriTokenKeyAndSle(env.current(),
    //     account, uri); std::cout << "URIT: " << urit << "\n"; std::cout <<
    //     name << "IN OWNER DIR: " << inOwnerDir(env.current(), account,
    //     uritSle) << "\n"; std::cout << name << "DIR: " <<
    //     ownerDirCount(env.current(), account) << "\n";
    // }

    static Json::Value
    mint(jtx::Account const& account, std::string const& uri)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Account] = account.human();
        jv[sfURI.jsonName] = strHex(uri);
        return jv;
    }

    static Json::Value
    burn(jtx::Account const& account, std::string const& id)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Account] = account.human();
        jv[sfURITokenID.jsonName] = id;
        return jv;
    }

    static Json::Value
    buy(jtx::Account const& account,
        std::string const& id,
        STAmount const& amount)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Account] = account.human();
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        jv[sfURITokenID.jsonName] = id;
        return jv;
    }

    static Json::Value
    sell(
        jtx::Account const& account,
        std::string const& id,
        STAmount const& amount)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Account] = account.human();
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        jv[sfURITokenID.jsonName] = id;
        return jv;
    }

    static Json::Value
    clear(jtx::Account const& account, std::string const& id)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URIToken;
        jv[jss::Account] = account.human();
        jv[sfURITokenID.jsonName] = id;
        return jv;
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

        {
            // If the URIToken amendment is not enabled, you should not be able
            // to mint, burn, buy, sell or clear uri tokens.
            Env env{*this, features - featureURIToken};

            env.fund(XRP(1000), alice, bob);

            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(alice, uri))};

            // MINT
            env(mint(alice, uri), ter(temDISABLED));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // SELL
            env(sell(alice, id, XRP(10)), txflags(tfSell), ter(temDISABLED));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // BUY
            env(buy(bob, id, XRP(10)), ter(temDISABLED));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // SELL
            env(sell(bob, id, XRP(10)), txflags(tfSell), ter(temDISABLED));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // CLEAR
            env(clear(bob, id), ter(temDISABLED));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // BURN
            env(burn(bob, id), ter(temDISABLED));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);
        }
        {
            // If the URIToken amendment is enabled, you should be able
            // to mint, burn, buy, sell and clear uri tokens.
            Env env{*this, features};

            env.fund(XRP(1000), alice, bob);

            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(alice, uri))};

            // MINT
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // SELL
            env(sell(alice, id, XRP(10)), txflags(tfSell));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // BUY
            env(buy(bob, id, XRP(10)));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);

            // SELL
            env(sell(bob, id, XRP(10)), txflags(tfSell));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);

            // CLEAR
            env(clear(bob, id));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);

            // BURN
            env(burn(bob, id));
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);
        }
    }

    void
    testMintInvalid(FeatureBitset features)
    {
        testcase("mint_invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(200), alice);
        env.close();

        //----------------------------------------------------------------------
        // preflight
        // temDISABLED - ignored
        std::string const uri(2, '?');

        // tecINSUFFICIENT_RESERVE - out of xrp
        env(mint(alice, uri), ter(tecINSUFFICIENT_RESERVE));
        env.close();

        // pay alice xrp
        env(pay(env.master, alice, XRP(1000)));
        env.close();

        // temMALFORMED - no uri & no flags
        std::string const nouri(0, '?');
        env(mint(alice, nouri), ter(temMALFORMED));
        env.close();

        // temMALFORMED - bad uri 257 len
        std::string const longuri(maxTokenURILength + 1, '?');
        env(mint(alice, longuri), ter(temMALFORMED));
        env.close();

        //----------------------------------------------------------------------
        // preclaim

        // tecNO_ENTRY - no id & not mint operation
        env(mint(alice, uri), txflags(tfSell), ter(temMALFORMED));
        env.close();
        // tecDUPLICATE - duplicate uri token
        env(mint(alice, uri));
        env(mint(alice, uri), ter(tecDUPLICATE));
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // tecDUPLICATE - duplicate uri token
        env(mint(alice, uri), ter(tecDUPLICATE));
        env.close();
        // tecDIR_FULL - directory full
    }

    void
    testBurnInvalid(FeatureBitset features)
    {
        testcase("burn_invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const dave = Account("dave");
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        //----------------------------------------------------------------------
        // preflight
        // temDISABLED - ignore

        // mint non burnable token
        std::string const uri(2, '?');
        std::string const id{strHex(tokenid(alice, uri))};
        env(mint(alice, uri));
        env.close();

        // temMALFORMED - bad flags
        env(burn(alice, id), txflags(0b001100010U), ter(temMALFORMED));
        env.close();

        // tecNO_PERMISSION - not owner and not (issuer/burnable)
        env(burn(bob, id), txflags(tfBurn), ter(tecNO_PERMISSION));
        env.close();

        //----------------------------------------------------------------------
        // entry preclaim

        // tecNO_ENTRY - no item
        std::string const baduri(3, '?');
        std::string const badid{strHex(tokenid(alice, baduri))};
        env(burn(alice, badid), txflags(tfBurn), ter(tecNO_ENTRY));
        env.close();
        // tecNO_ENTRY - no owner
        env(burn(dave, id), txflags(tfBurn), ter(tecNO_ENTRY));
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // tecNO_PERMISSION - no permission
        env(burn(carol, id), txflags(tfBurn), ter(tecNO_PERMISSION));
        env.close();
        // tefBAD_LEDGER - could not remove object
    }

    void
    testSellInvalid(FeatureBitset features)
    {
        testcase("sell_invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        Env env{*this, features};
        auto const nacct = Account("alice");
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        auto const NUSD = nacct["USD"];
        env.fund(XRP(1000), alice, bob, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env.close();

        // mint token
        std::string const uri(2, '?');
        std::string const id{strHex(tokenid(alice, uri))};
        env(mint(alice, uri));
        env.close();

        //----------------------------------------------------------------------
        // operator preflight
        // temDISABLED

        // temMALFORMED - invalid sell flag
        env(sell(alice, id, USD(10)), txflags(0b000110101U), ter(temMALFORMED));
        env.close();

        //----------------------------------------------------------------------
        // amount preflight
        // temBAD_AMOUNT - bad xrp/amount
        env(sell(alice, id, XRP(-1)), txflags(tfSell), ter(temBAD_AMOUNT));
        // temBAD_AMOUNT - bad ft/amount
        env(sell(alice, id, USD(-1)), txflags(tfSell), ter(temBAD_AMOUNT));
        // temBAD_CURRENCY - bad currency
        IOU const BAD{gw, badCurrency()};
        env(sell(alice, id, BAD(10)), txflags(tfSell), ter(temBAD_CURRENCY));
        env.close();

        //----------------------------------------------------------------------
        // preclaim
        // tecNO_PERMISSION - invalid account
        env(sell(bob, id, USD(10)), txflags(tfSell), ter(tecNO_PERMISSION));
        // tecNO_ISSUER - invalid issuer
        // env(sell(alice, id, NUSD(10)), txflags(tfSell), ter(tecNO_ISSUER));

        //----------------------------------------------------------------------
        // doApply

        // tecNO_PERMISSION - invalid account
        env(sell(bob, id, USD(10)), txflags(tfSell), ter(tecNO_PERMISSION));
    }

    void
    testBuyInvalid(FeatureBitset features)
    {
        testcase("buy_invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const dave = Account("dave");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        auto const EUR = gw["EUR"];
        env.fund(XRP(1000), alice, bob, carol, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.trust(USD(10000), carol);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env(pay(gw, carol, USD(1000)));
        env.close();

        // mint token
        std::string const uri(2, '?');
        std::string const id{strHex(tokenid(alice, uri))};
        env(mint(alice, uri));
        env.close();

        //----------------------------------------------------------------------
        // operator preflight
        // temDISABLED

        // temMALFORMED - invalid buy flag
        env(buy(bob, id, USD(10)), txflags(0b000110011U), ter(temMALFORMED));
        env.close();

        //----------------------------------------------------------------------
        // preclaim
        // tecNO_PERMISSION - not for sale
        env(buy(bob, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // set sell
        env(sell(alice, id, USD(10)),
            txflags(tfSell),
            jtx::token::destination(bob));
        env.close();

        // tecNO_PERMISSION - for sale to dest, you are not dest
        env(buy(carol, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // tecNFTOKEN_BUY_SELL_MISMATCH - invalid buy sell amounts
        env(buy(bob, id, EUR(10)), ter(tecNFTOKEN_BUY_SELL_MISMATCH));
        env.close();

        // tecINSUFFICIENT_PAYMENT - insuficient buy offer amount
        env(buy(bob, id, USD(9)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();

        env(clear(alice, id));
        env(sell(alice, id, XRP(10000)), txflags(tfSell));
        env.close();

        // tecINSUFFICIENT_FUNDS - insuficient xrp - fees
        env(buy(bob, id, XRP(1000)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();

        // clear sell and reset new sell
        env(clear(alice, id));
        env(sell(alice, id, USD(10000)), txflags(tfSell));
        env.close();

        // tecINSUFFICIENT_FUNDS - insuficient amount
        env(buy(bob, id, USD(1000)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // clear sell
        env(clear(alice, id));
        env.close();

        // tecNO_PERMISSION - not listed
        env(buy(bob, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // set sell
        env(sell(alice, id, USD(10)),
            txflags(tfSell),
            jtx::token::destination(bob));
        env.close();

        // tecNO_PERMISSION - for sale to dest, you are not dest
        env(buy(carol, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // tecNFTOKEN_BUY_SELL_MISMATCH - invalid buy sell amounts
        env(buy(bob, id, EUR(10)), ter(tecNFTOKEN_BUY_SELL_MISMATCH));
        env.close();

        // clear sell and set xrp sell
        env(clear(alice, id));
        env(sell(alice, id, XRP(1000)), txflags(tfSell));
        env.close();

        // tecINSUFFICIENT_PAYMENT - insuficient xrp sent
        env(buy(bob, id, XRP(900)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();
        // tecINSUFFICIENT_FUNDS - insuficient xrp - fees
        env(buy(bob, id, XRP(1000)), ter(tecINSUFFICIENT_FUNDS));
        env.close();

        // clear sell and set usd sell
        env(clear(alice, id));
        env(sell(alice, id, USD(1000)), txflags(tfSell));
        env.close();

        // tecINSUFFICIENT_PAYMENT - insuficient amount sent
        env(buy(bob, id, USD(900)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();

        // tecINSUFFICIENT_FUNDS - insuficient amount sent
        env(buy(bob, id, USD(10000)), ter(tecINSUFFICIENT_FUNDS));
        env.close();

        // fund dave 200 xrp (not enough for reserve)
        env.fund(XRP(260), dave);
        env.trust(USD(10000), dave);
        env(pay(gw, dave, USD(1000)));
        env.close();

        // auto const reserveFee =
        // env.current()->fees().accountReserve(ownerDirCount(*env.current(),
        // dave)); std::cout << "XRP RESERVE: " << reserveFee << "\n";
        // tecNO_LINE_INSUF_RESERVE - insuficient xrp to create line
        // env(buy(dave, id, USD(1000)), ter(tecNO_LINE_INSUF_RESERVE));
        // env.close();

        // tecDIR_FULL - unknown how to test/handle
        // tecINTERNAL - unknown how to test/handle
        // tecINTERNAL - unknown how to test/handle
        // tecINTERNAL - unknown how to test/handle
        // tecINTERNAL - unknown how to test/handle
        // tefBAD_LEDGER - unknown how to test/handle
        // tefBAD_LEDGER - unknown how to test/handle
        // tecINTERNAL - unknown how to test/handle
        // tecINTERNAL - unknown how to test/handle
    }

    void
    testClearInvalid(FeatureBitset features)
    {
        testcase("clear_invalid");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        auto const EUR = gw["EUR"];
        env.fund(XRP(1000), alice, bob, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env.close();

        // mint token
        std::string const uri(2, '?');
        std::string const id{strHex(tokenid(alice, uri))};
        env(mint(alice, uri));
        env.close();

        //----------------------------------------------------------------------
        // operator preflight
        // temDISABLED

        // temMALFORMED - invalid buy flag
        env(clear(alice, id), txflags(0b000100011U), ter(temMALFORMED));
        env.close();

        //----------------------------------------------------------------------
        // preclaim

        // tecNO_PERMISSION - not your uritoken
        env(clear(bob, id), ter(tecNO_PERMISSION));
        env.close();
    }

    void
    testMintValid(FeatureBitset features)
    {
        testcase("mint");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        // setup env
        Env env{*this, features};
        env.fund(XRP(1000), alice, bob);

        std::string const uri(maxTokenURILength, '?');
        std::string const id{strHex(tokenid(alice, uri))};

        auto const digestval =
            "C16E7263F07AA41261DCC955660AF4646ADBA414E37B6F5A5BA50F75153F5CCC";

        // 0b110000001U: has digest - has uri - no flags
        {
            // mint
            env(mint(alice, uri), json(sfDigest.fieldName, digestval));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // cleanup
            env(burn(alice, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
        }
        // 0b110000010U: has digest - has uri - burnable flag
        {
            // mint
            env(mint(alice, uri),
                txflags(tfBurnable),
                json(sfDigest.fieldName, digestval));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // cleanup
            env(burn(alice, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
        }
        // 0b010000001U: has uri - no flags
        {
            // mint
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // cleanup
            env(burn(alice, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
        }
        // 0b010000010U: has uri - burnable flag
        {
            // mint
            env(mint(alice, uri), txflags(tfBurnable));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // cleanup
            env(burn(alice, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
        }
    }

    void
    testBurnValid(FeatureBitset features)
    {
        testcase("burn");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        // setup env
        Env env{*this, features};
        env.fund(XRP(1000), alice, bob);

        std::string const uri(maxTokenURILength, '?');
        std::string const id{strHex(tokenid(alice, uri))};

        // issuer can burn
        {
            // alice mints
            env(mint(alice, uri), txflags(tfBurnable));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // alice sells
            env(sell(alice, id, XRP(1)), txflags(tfSell));
            env.close();
            // bob buys
            env(buy(bob, id, XRP(1)));
            env.close();
            // alice burns
            env(burn(alice, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(bob, uri)));
        }
        // issuer cannot burn
        {
            // alice mints
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // alice sells
            env(sell(alice, id, XRP(1)), txflags(tfSell));
            env.close();
            // bob buys
            env(buy(bob, id, XRP(1)));
            env.close();
            // alice tries to burn
            env(burn(alice, id), txflags(tfBurn), ter(tecNO_PERMISSION));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(bob, uri)));
            // burn for test reset
            env(burn(bob, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(bob, uri)));
        }
        // owner can burn
        {
            // alice mints
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            // alice sells
            env(sell(alice, id, XRP(1)), txflags(tfSell));
            env.close();
            // bob buys
            env(buy(bob, id, XRP(1)));
            env.close();
            // bob burns
            env(burn(bob, id), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(alice, uri)));
            BEAST_EXPECT(!uritokenExists(*env.current(), tokenid(bob, uri)));
        }
    }

    void
    testBuyValid(FeatureBitset features)
    {
        testcase("buy");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        // setup env
        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env.close();

        auto const feeDrops = env.current()->fees().base;
        std::string const uri(maxTokenURILength, '?');
        auto const tid = tokenid(alice, uri);
        std::string const hexid{strHex(tid)};

        // bob can buy with XRP
        {
            // alice mints
            const auto delta = XRP(10);
            auto preAlice = env.balance(alice);
            auto preBob = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            BEAST_EXPECT(env.balance(alice) == preAlice - (1 * feeDrops));
            // alice sells
            env(sell(alice, hexid, delta), txflags(tfSell));
            BEAST_EXPECT(env.balance(alice) == preAlice - (2 * feeDrops));
            env.close();
            // bob buys
            env(buy(bob, hexid, delta));
            env.close();

            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(
                env.balance(alice) == preAlice + delta - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob) == preBob - delta - feeDrops);
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), urit));
        }
        // bob can buy with USD
        {
            // alice mints
            const auto delta = USD(10);
            auto preAlice = env.balance(alice, USD.issue());
            auto preAliceXrp = env.balance(alice);
            auto preBob = env.balance(bob, USD.issue());
            auto preBobXrp = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (1 * feeDrops));
            // alice sells
            env(sell(alice, hexid, delta), txflags(tfSell));
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (2 * feeDrops));
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice);
            env.close();
            // bob buys
            env(buy(bob, hexid, delta));
            env.close();
            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice + delta);
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob - delta);
            BEAST_EXPECT(env.balance(bob) == preBobXrp - (1 * feeDrops));
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), urit));
        }
    }

    void
    testSellValid(FeatureBitset features)
    {
        testcase("sell");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        // setup env
        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.trust(USD(10000), carol);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env(pay(gw, carol, USD(1000)));
        env.close();

        auto const feeDrops = env.current()->fees().base;
        std::string const uri(maxTokenURILength, '?');
        auto const tid = tokenid(alice, uri);
        std::string const hexid{strHex(tid)};

        // alice can sell with XRP
        {
            // alice mints
            const auto delta = XRP(10);
            auto preAlice = env.balance(alice);
            auto preBob = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, delta), txflags(tfSell));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears and sells again at a higher price
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            env(sell(alice, hexid, XRP(11)), txflags(tfSell));
            env.close();
            BEAST_EXPECT(XRP(11) == tokenAmount(*env.current(), tid));
            // bob tries to buy at original price and fails
            env(buy(bob, hexid, delta), ter(tecINSUFFICIENT_PAYMENT));
            // bob buys at higher price
            env(buy(bob, hexid, XRP(11)));
            env.close();
            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(
                env.balance(alice) == preAlice + XRP(11) - (4 * feeDrops));
            BEAST_EXPECT(env.balance(bob) == preBob - XRP(11) - (2 * feeDrops));
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            debugToken(*env.current(), "token", alice, tid);
            debugBalance(env, "alice", alice, USD);
            debugBalance(env, "bob", bob, USD);

            // bob burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // alice can sell with XRP and dest
        {
            // alice mints
            const auto delta = XRP(10);
            auto preAlice = env.balance(alice);
            auto preBob = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, delta),
                txflags(tfSell),
                jtx::token::destination(bob));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // carol tries to buy but cannot
            env(buy(carol, hexid, delta), ter(tecNO_PERMISSION));
            env.close();
            // bob buys
            env(buy(bob, hexid, delta));
            env.close();
            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(
                env.balance(alice) == preAlice + delta - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob) == preBob - delta - (1 * feeDrops));
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }

        // alice can sell with USD
        {
            // alice mints
            const auto delta = USD(10);
            auto preAlice = env.balance(alice, USD.issue());
            auto preAliceXrp = env.balance(alice);
            auto preBob = env.balance(bob, USD.issue());
            auto preBobXrp = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, delta), txflags(tfSell));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears and sells again at a higher price
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            env(sell(alice, hexid, USD(11)), txflags(tfSell));
            env.close();
            BEAST_EXPECT(USD(11) == tokenAmount(*env.current(), tid));
            // bob tries to buy at original price and fails
            env(buy(bob, hexid, delta), ter(tecINSUFFICIENT_PAYMENT));
            // bob buys at higher price
            env(buy(bob, hexid, USD(11)));
            env.close();

            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice + USD(11));
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (4 * feeDrops));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob - USD(11));
            BEAST_EXPECT(env.balance(bob) == preBobXrp - (2 * feeDrops));
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // alice can sell with USD and dest
        {
            // alice mints
            const auto delta = USD(10);
            auto preAlice = env.balance(alice, USD.issue());
            auto preAliceXrp = env.balance(alice);
            auto preBob = env.balance(bob, USD.issue());
            auto preBobXrp = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, delta),
                txflags(tfSell),
                jtx::token::destination(bob));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // carol tries to buy but cannot
            env(buy(carol, hexid, delta), ter(tecNO_PERMISSION));
            env.close();
            // bob buys
            env(buy(bob, hexid, delta));
            env.close();

            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice + delta);
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob - delta);
            BEAST_EXPECT(env.balance(bob) == preBobXrp - (1 * feeDrops));
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
    }

    void
    testClearValid(FeatureBitset features)
    {
        testcase("clear");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        // setup env
        Env env{*this, features};
        env.fund(XRP(1000), alice, bob, carol, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.trust(USD(10000), carol);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env(pay(gw, carol, USD(1000)));
        env.close();

        auto const feeDrops = env.current()->fees().base;
        std::string const uri(maxTokenURILength, '?');
        auto const tid = tokenid(alice, uri);
        std::string const hexid{strHex(tid)};

        // alice can clear / reset XRP amount
        {
            // alice mints
            const auto delta = XRP(10);
            auto preAlice = env.balance(alice);
            auto preBob = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, delta), txflags(tfSell));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            // alice sets sell for higher amount
            env(sell(alice, hexid, XRP(11)), txflags(tfSell));
            env.close();
            BEAST_EXPECT(XRP(11) == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));

            // alice burns to reset tests
            env(burn(bob, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // alice can clear / reset USD amount
        {
            // alice mints
            const auto delta = USD(10);
            auto preAlice = env.balance(alice, USD.issue());
            auto preAliceXrp = env.balance(alice);
            auto preBob = env.balance(bob, USD.issue());
            auto preBobXrp = env.balance(bob);
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, delta), txflags(tfSell));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            // alice sets sell for higher amount
            env(sell(alice, hexid, USD(11)), txflags(tfSell));
            env.close();
            BEAST_EXPECT(USD(11) == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));

            // alice burns to reset tests
            env(burn(alice, hexid), txflags(tfBurn));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
    }

    void
    testMetaAndOwnership(FeatureBitset features)
    {
        testcase("Metadata & Ownership");

        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        std::string const uri(maxTokenURILength, '?');
        std::string const id{strHex(tokenid(alice, uri))};

        {
            // Test without adding the uritoken to the recipient's owner
            // directory
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, gw);
            env.trust(USD(10000), alice);
            env.trust(USD(10000), bob);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            env(mint(alice, uri));
            env(sell(alice, id, USD(10)), txflags(tfSell));
            env.close();
            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 2);
            BEAST_EXPECT(!inOwnerDir(*env.current(), bob, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            // // alice sets the sell offer
            // // bob sets the buy offer
            env(buy(bob, id, USD(10)));
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(!inOwnerDir(*env.current(), alice, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(!inOwnerDir(*env.current(), bob, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
        }
        {
            // Test with adding the uritoken to the recipient's owner
            // directory
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, gw);
            env.trust(USD(10000), alice);
            env.trust(USD(10000), bob);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            env(mint(alice, uri));
            env(sell(alice, id, USD(10)), txflags(tfSell));
            env.close();
            auto const [urit, uritSle] =
                uriTokenKeyAndSle(*env.current(), alice, uri);
            BEAST_EXPECT(inOwnerDir(*env.current(), alice, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 2);
            BEAST_EXPECT(!inOwnerDir(*env.current(), bob, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 1);
            // // alice sets the sell offer
            // // bob sets the buy offer
            env(buy(bob, id, USD(10)));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), urit));
            BEAST_EXPECT(!inOwnerDir(*env.current(), alice, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 1);
            BEAST_EXPECT(!inOwnerDir(*env.current(), bob, uritSle));
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 2);
        }
    }

    // TODO: THIS TEST IS NOT COMPLETE
    void
    testAccountDelete(FeatureBitset features)
    {
        testcase("Account Delete");
        using namespace test::jtx;
        using namespace std::literals::chrono_literals;
        auto rmAccount = [this](
                             Env& env,
                             Account const& toRm,
                             Account const& dst,
                             TER expectedTer = tesSUCCESS) {
            // only allow an account to be deleted if the account's sequence
            // number is at least 256 less than the current ledger sequence
            for (auto minRmSeq = env.seq(toRm) + 257;
                 env.current()->seq() < minRmSeq;
                 env.close())
            {
            }

            env(acctdelete(toRm, dst),
                fee(drops(env.current()->fees().increment)),
                ter(expectedTer));
            env.close();
            this->BEAST_EXPECT(
                isTesSuccess(expectedTer) ==
                !env.closed()->exists(keylet::account(toRm.id())));
        };

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];

        std::string const uri(maxTokenURILength, '?');
        std::string const id{strHex(tokenid(alice, uri))};

        {
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, carol, gw);
            env.trust(USD(10000), alice);
            env.trust(USD(10000), bob);
            env.trust(USD(10000), carol);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env(pay(gw, carol, USD(1000)));
            env.close();

            // mint a uritoken from alice
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
            env(sell(alice, id, USD(10)), txflags(tfSell));
            env.close();

            // alice has trustline + mint + sell
            rmAccount(env, alice, bob, tecHAS_OBLIGATIONS);

            env(clear(alice, id));
            env(burn(alice, id), txflags(tfBurn));
            env.close();

            // alice still has a trustline
            rmAccount(env, alice, bob, tecHAS_OBLIGATIONS);

            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));

            // buy should fail if the uri token was removed
            auto preBob = env.balance(bob, USD.issue());
            env(buy(bob, id, USD(10)), ter(tecNO_ENTRY));
            env.close();
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob);

            env(mint(bob, uri));
            BEAST_EXPECT(uritokenExists(*env.current(), tokenid(bob, uri)));
        }
    }

    // TODO: THIS TEST IS NOT COMPLETE
    void
    testUsingTickets(FeatureBitset features)
    {
        testcase("using tickets");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        auto USD = gw["USD"];
        env.fund(XRP(1000), alice, bob, gw);
        env.trust(USD(10000), alice);
        env.trust(USD(10000), bob);
        env.close();
        env(pay(gw, alice, USD(1000)));
        env(pay(gw, bob, USD(1000)));
        env.close();

        // alice and bob grab enough tickets for all of the following
        // transactions.  Note that once the tickets are acquired alice's
        // and bob's account sequence numbers should not advance.
        std::uint32_t aliceTicketSeq{env.seq(alice) + 1};
        env(ticket::create(alice, 10));
        std::uint32_t const aliceSeq{env.seq(alice)};

        std::uint32_t bobTicketSeq{env.seq(bob) + 1};
        env(ticket::create(bob, 10));
        std::uint32_t const bobSeq{env.seq(bob)};

        std::string const uri(maxTokenURILength, '?');
        std::string const id{strHex(tokenid(alice, uri))};

        env(mint(alice, uri), ticket::use(aliceTicketSeq++));

        env.require(tickets(alice, env.seq(alice) - aliceTicketSeq));
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testEnabled(features);
        // testMintInvalid(features);
        // testBurnInvalid(features);
        // testSellInvalid(features);
        // testBuyInvalid(features);
        // testClearInvalid(features);
        // testMintValid(features);
        // testBurnValid(features);
        // testBuyValid(features);
        testSellValid(features);
        testClearValid(features);
        // testMetaAndOwnership(features);
        // testAccountDelete(features);
        // testUsingTickets(features);
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

BEAST_DEFINE_TESTSUITE(URIToken, app, ripple);
}  // namespace test
}  // namespace ripple
