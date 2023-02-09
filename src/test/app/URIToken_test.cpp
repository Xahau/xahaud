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
    debugXrp(
        jtx::Env const& env,
        std::string const& name,
        jtx::Account const& account)
    {
        std::cout << name << " BALANCE XRP: " << env.balance(account) << "\n";
    }

    void
    debugBalance(
        jtx::Env const& env,
        std::string const& name,
        jtx::Account const& account,
        jtx::IOU const& iou)
    {
        std::cout << name << " BALANCE XRP: " << env.balance(account) << "\n";
        std::cout << name
                  << " BALANCE USD: " << env.balance(account, iou.issue())
                  << "\n";
        std::cout << name << " BALANCE LINE: " << lineBalance(env, account, iou)
                  << "\n";
    }

    void
    debugToken(
        ReadView const& view,
        uint256 const& id)
    {
        // debugToken(*env.current(), tid);
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return;

        std::cout << " ---- DEBUG TOKEN ----" << "\n";
        std::cout << "BURNABLE: " << (slep->getFlags() & tfBurnable) << "\n";
        std::cout <<  "OWNER: " << slep->getAccountID(sfOwner)
                  << "\n";
        std::cout <<  "ISSUER: " << slep->getAccountID(sfIssuer)
                  << "\n";
        // std::cout <<  " sfURI: " << slep->getFieldVL(sfURI) << "\n";
        // std::cout <<  " sfDigest: " << (*slep)[sfDigest] << "\n";
        if (slep->getFieldAmount(sfAmount))
            std::cout <<  "AMOUNT: " << slep->getFieldAmount(sfAmount)
                      << "\n";

        if (slep->getFieldAmount(sfAmount))
            std::cout << "DESTINATION: " << slep->getAccountID(sfDestination)
                      << "\n";
    }

    void
    testToken(
        ReadView const& view,
        bool const& debug,
        uint256 const& id,
        AccountID const& owner,
        AccountID const& issuer,
        std::string const& uri,
        AccountID const& dest,
        STAmount const& amount,
        bool const& burnable)
    {
        // get the ledger object
        auto const slep = view.read({ltURI_TOKEN, id});
        if (!slep)
            return;

        // test the ledger object required
        BEAST_EXPECT(slep->getAccountID(sfOwner) == owner);
        BEAST_EXPECT(slep->getAccountID(sfIssuer) == issuer);
        // BEAST_EXPECT(slep->getFieldVL(sfURI).size() > uri.size());
        
        // test the ledger object optionals
        if(burnable)
            BEAST_EXPECT(slep->getFlags() & tfBurnable);
        
        if (amount != STAmount{0})
            BEAST_EXPECT(slep->getFieldAmount(sfAmount) == amount);

        if (dest != AccountID{0})
            BEAST_EXPECT(slep->getAccountID(sfDestination) == dest);

        // debug the ledger object
        if (debug)
            debugToken(view, id);
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

    static STAmount
    lineBalance(
        jtx::Env const& env,
        jtx::Account const& account,
        jtx::IOU const& iou)
    {
        auto const sle =
            env.le(keylet::line(account, iou.account, iou.currency));
        if (sle && sle->isFieldPresent(sfBalance))
            return (*sle)[sfBalance];
        return STAmount(iou, 0);
    }

    static Json::Value
    mint(jtx::Account const& account, std::string const& uri)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URITokenMint;
        jv[jss::Account] = account.human();
        jv[sfURI.jsonName] = strHex(uri);
        return jv;
    }

    static Json::Value
    burn(jtx::Account const& account, std::string const& id)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::URITokenBurn;
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
        jv[jss::TransactionType] = jss::URITokenBuy;
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
        jv[jss::TransactionType] = jss::URITokenCreateSellOffer;
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
        jv[jss::TransactionType] = jss::URITokenCancelSellOffer;
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

        for (bool const withURIToken : {false, true})
        {
            // If the URIToken amendment is not enabled, you should not be able
            // to mint, burn, buy, sell or clear uri tokens.
            auto const amend =
                withURIToken ? features : features - featureURIToken;
            Env env{*this, amend};

            env.fund(XRP(1000), alice, bob);

            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(alice, uri))};

            auto const txResult =
                withURIToken ? ter(tesSUCCESS) : ter(temDISABLED);
            auto const ownerDir = withURIToken ? 1 : 0;

            // MINT
            env(mint(alice, uri), txResult);
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == ownerDir);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // SELL
            env(sell(alice, id, XRP(10)), txResult);
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == ownerDir);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == 0);

            // BUY
            env(buy(bob, id, XRP(10)), txResult);
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == ownerDir);

            // SELL
            env(sell(bob, id, XRP(10)), txResult);
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == ownerDir);

            // CLEAR
            env(clear(bob, id), txResult);
            env.close();
            BEAST_EXPECT(ownerDirCount(*env.current(), alice) == 0);
            BEAST_EXPECT(ownerDirCount(*env.current(), bob) == ownerDir);

            // BURN
            env(burn(bob, id), txResult);
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
        auto const bob = Account("bob");

        env.fund(XRP(200), alice);
        env.close();

        std::string const uri(2, '?');
        auto const tid = tokenid(alice, uri);
        std::string const hexid{strHex(tid)};

        //----------------------------------------------------------------------
        // preflight

        {
            // temINVALID_FLAG - invalid flags
            env(mint(alice, uri), txflags(tfAllowXRP), ter(temINVALID_FLAG));
            env.close();

            // temMALFORMED - no uri & no flags
            std::string const nouri(0, '?');
            env(mint(alice, nouri), ter(temMALFORMED));
            env.close();

            // temMALFORMED - bad uri 257 len
            std::string const longuri(maxTokenURILength + 1, '?');
            env(mint(alice, longuri), ter(temMALFORMED));
            env.close();
        }

        //----------------------------------------------------------------------
        // preclaim

        {
            env.fund(XRP(251), bob);
            env.close();
            auto const btid = tokenid(bob, uri);
            std::string const bhexid{strHex(btid)};
            // tecDUPLICATE - duplicate uri token
            env(mint(bob, uri), txflags(tfBurnable));
            env(mint(bob, uri), ter(tecDUPLICATE));
            env(burn(bob, bhexid));
            env.close();
        }

        //----------------------------------------------------------------------
        // doApply

        {
            // tecINSUFFICIENT_RESERVE - out of xrp
            env(mint(alice, uri), ter(tecINSUFFICIENT_RESERVE));
            env.close();

            // tecDIR_FULL - directory full
            // pay alice xrp - 251 * required reserve (50) + 1
            // env(pay(env.master, alice, XRP(1) + XRP(251 * 50)));
            // env.close();

            // env(mint(alice, uri), ter(tecDIR_FULL));
            // env.close();
            // auto const reserveFee = 
            //     env.current()->fees().accountReserve(ownerDirCount(*env.current(), alice));
            // std::cout << "RESERVE FEE: " << reserveFee << "\n";
            // debugXrp(env, "alice", alice);
        }
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
        env.fund(XRP(1000), alice, bob, carol, dave);
        env.close();

        //----------------------------------------------------------------------
        // preflight
        // temDISABLED - ignore

        // mint non burnable token
        std::string const uri(2, '?');
        std::string const id{strHex(tokenid(alice, uri))};
        env(mint(alice, uri));
        env.close();

        // temINVALID_FLAG - invalid flags
        env(burn(alice, id), txflags(tfAllowXRP), ter(temINVALID_FLAG));
        env.close();

        //----------------------------------------------------------------------
        // preclaim

        // tecNO_ENTRY - no item
        std::string const baduri(3, '?');
        std::string const badid{strHex(tokenid(alice, baduri))};
        env(burn(alice, badid), ter(tecNO_ENTRY));
        env.close();

        // todo: delete account for this
        // tecNO_ENTRY - no owner exists
        // env(burn(dave, id), ter(tecNO_ENTRY));
        // env.close();

        // tecNO_PERMISSION - not owner and not (issuer/burnable)
        env(burn(bob, id), ter(tecNO_PERMISSION));
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // tecNO_PERMISSION - no permission
        env(burn(carol, id), ter(tecNO_PERMISSION));
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
        // preflight

        // temBAD_AMOUNT - bad xrp/amount
        env(sell(alice, id, XRP(-1)), ter(temBAD_AMOUNT));
        env.close();

        // temBAD_AMOUNT - bad ft/amount
        env(sell(alice, id, USD(-1)), ter(temBAD_AMOUNT));
        env.close();

        // temBAD_CURRENCY - bad currency
        IOU const BAD{gw, badCurrency()};
        env(sell(alice, id, BAD(10)), ter(temBAD_CURRENCY));
        
        // temMALFORMED - no destination and 0 value
        env(sell(alice, id, USD(0)), ter(temMALFORMED));
        env.close();

        //----------------------------------------------------------------------
        // preclaim
        // tecNO_PERMISSION - invalid account
        env(sell(bob, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // todo: delete account for this
        // tecNO_ISSUER - invalid issuer
        // env(sell(alice, id, NUSD(10)), ter(tecNO_ISSUER));
        env.close();

        //----------------------------------------------------------------------
        // doApply

        // tecNO_PERMISSION - invalid account
        env(sell(bob, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();
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
        // preclaim

        // tecNO_PERMISSION - not for sale
        env(buy(bob, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // set sell
        env(sell(alice, id, USD(10)), jtx::token::destination(bob));
        env.close();

        // tecNO_PERMISSION - for sale to dest, you are not dest
        env(buy(carol, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // temBAD_CURRENCY - invalid buy sell amounts
        env(buy(bob, id, EUR(10)), ter(temBAD_CURRENCY));
        env.close();

        // tecINSUFFICIENT_PAYMENT - insuficient buy offer amount
        env(buy(bob, id, USD(9)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();

        env(clear(alice, id));
        env(sell(alice, id, XRP(10000)));
        env.close();

        // tecINSUFFICIENT_FUNDS - insuficient xrp - fees
        env(buy(bob, id, XRP(1000)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();

        // clear sell and reset new sell
        env(clear(alice, id));
        env(sell(alice, id, USD(10000)));
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
        env(sell(alice, id, USD(10)), jtx::token::destination(bob));
        env.close();

        // tecNO_PERMISSION - for sale to dest, you are not dest
        env(buy(carol, id, USD(10)), ter(tecNO_PERMISSION));
        env.close();

        // temBAD_CURRENCY - invalid buy sell amounts
        env(buy(bob, id, EUR(10)), ter(temBAD_CURRENCY));
        env.close();

        // clear sell and set xrp sell
        env(clear(alice, id));
        env(sell(alice, id, XRP(1000)));
        env.close();

        // tecINSUFFICIENT_PAYMENT - insuficient xrp sent
        env(buy(bob, id, XRP(900)), ter(tecINSUFFICIENT_PAYMENT));
        env.close();
        // tecINSUFFICIENT_FUNDS - insuficient xrp - fees
        env(buy(bob, id, XRP(1000)), ter(tecINSUFFICIENT_FUNDS));
        env.close();

        // clear sell and set usd sell
        env(clear(alice, id));
        env(sell(alice, id, USD(1000)));
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

        // temINVALID_FLAG - invalid flag
        env(clear(alice, id), txflags(tfAllowXRP), ter(temINVALID_FLAG));
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
        auto const tid = tokenid(alice, uri);
        std::string const hexid{strHex(tid)};

        auto const digestval =
            "C16E7263F07AA41261DCC955660AF4646ADBA414E37B6F5A5BA50F75153F5CCC";

        // has digest - has uri - no flags
        {
            // mint
            env(mint(alice, uri), json(sfDigest.fieldName, digestval));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // cleanup
            env(burn(alice, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // has digest - has uri - burnable flag
        {
            // mint
            env(mint(alice, uri),
                txflags(tfBurnable),
                json(sfDigest.fieldName, digestval));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // cleanup
            env(burn(alice, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // has uri - no flags
        {
            // mint
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // cleanup
            env(burn(alice, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // has uri - burnable flag
        {
            // mint
            env(mint(alice, uri), txflags(tfBurnable));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // cleanup
            env(burn(alice, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
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
        auto const tid = tokenid(alice, uri);
        std::string const hexid{strHex(tid)};

        // issuer can burn
        {
            // alice mints
            env(mint(alice, uri), txflags(tfBurnable));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, XRP(1)));
            env.close();
            // bob buys
            env(buy(bob, hexid, XRP(1)));
            env.close();
            // alice burns
            env(burn(alice, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // issuer cannot burn
        {
            // alice mints
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, XRP(1)));
            env.close();
            // bob buys
            env(buy(bob, hexid, XRP(1)));
            env.close();
            // alice tries to burn
            env(burn(alice, hexid), ter(tecNO_PERMISSION));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // burn for test reset
            env(burn(bob, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
        }
        // owner can burn
        {
            // alice mints
            env(mint(alice, uri));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            // alice sells
            env(sell(alice, hexid, XRP(1)));
            env.close();
            // bob buys
            env(buy(bob, hexid, XRP(1)));
            env.close();
            // bob burns
            env(burn(bob, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
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
            env(sell(alice, hexid, delta));
            BEAST_EXPECT(env.balance(alice) == preAlice - (2 * feeDrops));
            env.close();
            // bob buys
            env(buy(bob, hexid, delta));
            env.close();

            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            BEAST_EXPECT(
                env.balance(alice) == preAlice + delta - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob) == preBob - delta - feeDrops);
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
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
            env(sell(alice, hexid, delta));
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (2 * feeDrops));
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice);
            env.close();
            // bob buys
            env(buy(bob, hexid, delta));
            env.close();
            BEAST_EXPECT(uritokenExists(*env.current(), tid));
            BEAST_EXPECT(env.balance(alice, USD.issue()) == preAlice + delta);
            BEAST_EXPECT(env.balance(alice) == preAliceXrp - (2 * feeDrops));
            BEAST_EXPECT(env.balance(bob, USD.issue()) == preBob - delta);
            BEAST_EXPECT(env.balance(bob) == preBobXrp - (1 * feeDrops));
            BEAST_EXPECT(bob.id() == tokenOwner(*env.current(), tid));

            // bob burns to reset tests
            env(burn(bob, hexid));
            env.close();
            BEAST_EXPECT(!uritokenExists(*env.current(), tid));
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
            env(sell(alice, hexid, delta));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears and sells again at a higher price
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            env(sell(alice, hexid, XRP(11)));
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

            // bob burns to reset tests
            env(burn(bob, hexid));
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
            env(sell(alice, hexid, delta), jtx::token::destination(bob));
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
            env(burn(bob, hexid));
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
            env(sell(alice, hexid, delta));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears and sells again at a higher price
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            env(sell(alice, hexid, USD(11)));
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
            env(burn(bob, hexid));
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
            env(sell(alice, hexid, delta), jtx::token::destination(bob));
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
            env(burn(bob, hexid));
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

        // auto const feeDrops = env.current()->fees().base;
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
            env(sell(alice, hexid, delta));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            // alice sets sell for higher amount
            env(sell(alice, hexid, XRP(11)));
            env.close();
            BEAST_EXPECT(XRP(11) == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));

            // alice burns to reset tests
            env(burn(alice, hexid));
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
            env(sell(alice, hexid, delta));
            env.close();
            BEAST_EXPECT(delta == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));
            // alice sets sell for higher amount
            env(sell(alice, hexid, USD(11)));
            env.close();
            BEAST_EXPECT(USD(11) == tokenAmount(*env.current(), tid));
            // alice clears the sell amount
            env(clear(alice, hexid));
            BEAST_EXPECT(XRPAmount{-1} == tokenAmount(*env.current(), tid));

            // alice burns to reset tests
            env(burn(alice, hexid));
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
            env(sell(alice, id, USD(10)));
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
            env(sell(alice, id, USD(10)));
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
            env(sell(alice, id, USD(10)));
            env.close();

            // alice has trustline + mint + sell
            rmAccount(env, alice, bob, tecHAS_OBLIGATIONS);

            env(clear(alice, id));
            env(burn(alice, id));
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
        env(sell(alice, id, USD(10)), ticket::use(aliceTicketSeq++));

        env.require(tickets(alice, env.seq(alice) - (2 * aliceTicketSeq)));
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        BEAST_EXPECT(uritokenExists(*env.current(), tokenid(alice, uri)));

        // A transaction that generates a tec still consumes its ticket.
        env(buy(bob, id, USD(10)),
            ticket::use(bobTicketSeq++),
            ter(tecINSUFFICIENT_FUNDS));
        env.require(tickets(alice, env.seq(alice) - (2 * aliceTicketSeq)));

        env(buy(bob, id, USD(10)), ticket::use(bobTicketSeq++));

        env.require(tickets(bob, env.seq(bob) - (2 * bobTicketSeq)));
        BEAST_EXPECT(env.seq(bob) == bobSeq);
        BEAST_EXPECT(uritokenExists(*env.current(), tokenid(bob, uri)));
    }

    void
    testRippleState(FeatureBitset features)
    {
        testcase("RippleState");
        using namespace test::jtx;
        using namespace std::literals;

        //
        // USE lineBalance(env, ...) over env.balance(...)
        // I did this to check the exact sign "-/+"
        //

        // src > dst
        // src > issuer
        // dst no trustline
        // negative tl balance
        {
            auto const src = Account("alice2");
            auto const dst = Account("bob0");
            auto const gw = Account{"gw0"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(5000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src);
            env.close();
            env(pay(gw, src, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc + delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst - delta);
        }
        // src < dst
        // src < issuer
        // dest no trustline
        // positive tl balance
        {
            auto const src = Account("carol0");
            auto const dst = Account("dan1");
            auto const gw = Account{"gw1"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(5000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src);
            env.close();
            env(pay(gw, src, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc - delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst + delta);
        }
        // dst > src
        // dst > issuer
        // dest no trustline
        // negative locked/tl balance
        {
            auto const src = Account("dan1");
            auto const dst = Account("alice2");
            auto const gw = Account{"gw0"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(5000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src);
            env.close();
            env(pay(gw, src, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc + delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst - delta);
        }
        // dst < src
        // dst < issuer
        // dest no trustline
        // positive tl balance
        {
            auto const src = Account("bob0");
            auto const dst = Account("carol0");
            auto const gw = Account{"gw1"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(5000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src);
            env.close();
            env(pay(gw, src, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc - delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst + delta);
        }
        // src > dst
        // src > issuer
        // dst has trustline
        // negative tl balance
        {
            auto const src = Account("alice2");
            auto const dst = Account("bob0");
            auto const gw = Account{"gw0"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(5000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src, dst);
            env.close();
            env(pay(gw, src, USD(10000)));
            env(pay(gw, dst, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc + delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst - delta);
        }
        // src < dst
        // src < issuer
        // dest has trustline
        // positive tl balance
        {
            auto const src = Account("carol0");
            auto const dst = Account("dan1");
            auto const gw = Account{"gw1"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(10000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src, dst);
            env.close();
            env(pay(gw, src, USD(10000)));
            env(pay(gw, dst, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc - delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst + delta);
        }
        // dst > src
        // dst > issuer
        // dest has trustline
        // negative tl balance
        {
            auto const src = Account("dan1");
            auto const dst = Account("alice2");
            auto const gw = Account{"gw0"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(10000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src, dst);
            env.close();
            env(pay(gw, src, USD(10000)));
            env(pay(gw, dst, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc + delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst - delta);
        }
        // dst < src
        // dst < issuer
        // dest has trustline
        // positive tl balance
        {
            auto const src = Account("bob0");
            auto const dst = Account("carol0");
            auto const gw = Account{"gw1"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(10000), src, dst, gw);
            env.close();
            env.trust(USD(100000), src, dst);
            env.close();
            env(pay(gw, src, USD(10000)));
            env(pay(gw, dst, USD(10000)));
            env.close();

            // dst can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(dst, uri))};
            env(mint(dst, uri));
            env.close();

            // dst can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, dst, USD);
            env(sell(dst, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(src, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc - delta);
            BEAST_EXPECT(lineBalance(env, dst, USD) == preDst + delta);
        }
    }

    void
    testGateway(FeatureBitset features)
    {
        testcase("Gateway");
        using namespace test::jtx;
        using namespace std::literals;

        // issuer -> src
        // src > issuer
        // dest no trustline
        // negative tl balance
        {
            auto const src = Account("alice2");
            auto const gw = Account{"gw0"};
            auto const USD = gw["USD"];

            Env env{*this, features};
            env.fund(XRP(10000), src, gw);
            env.close();

            // src can create uritoken
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(src, uri))};
            env(mint(src, uri));
            env.close();

            // issuer can create sell w/out token
            auto const delta = USD(1000);
            auto const preSrc = lineBalance(env, src, USD);
            auto const preDst = lineBalance(env, gw, USD);
            env(sell(src, id, delta));
            env.close();
            BEAST_EXPECT(preDst == preDst);

            // src can create buy
            env(buy(gw, id, delta));
            env.close();
            BEAST_EXPECT(lineBalance(env, src, USD) == preSrc - delta);
            BEAST_EXPECT(lineBalance(env, gw, USD) == preDst + delta);
        }
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
            env(trust(gw, bobUSD(10000)), txflags(tfSetfAuth));
            env(trust(bob, USD(10000)));
            env.close();
            env(pay(gw, bob, USD(1000)));
            env.close();

            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(alice, uri))};
            env(mint(alice, uri));
            env(sell(alice, id, USD(10)));
            env.close();

            // bob cannot buy because alice's trustline is not authorized
            // all parties must be authorized
            env(buy(bob, id, USD(10)), ter(tecNO_AUTH));
            env.close();

            env(trust(gw, aliceUSD(10000)), txflags(tfSetfAuth));
            env(trust(alice, USD(10000)));
            env.close();
            env(pay(gw, alice, USD(1000)));
            env.close();

            // bob can now buy because alice's trustline is authorized
            env(buy(bob, id, USD(10)));
            env.close();
        }
    }

    void
    testGlobalFreeze(FeatureBitset features)
    {
        testcase("Global Freeze");
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
            // setup env
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env.trust(USD(10000), alice);
            env.trust(USD(10000), bob);
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();
            env(fset(gw, asfGlobalFreeze));
            env.close();

            // setup mint
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(alice, uri))};
            env(mint(alice, uri));
            env(sell(alice, id, USD(10)));
            env.close();

            // bob cannot buy
            env(buy(bob, id, USD(10)), ter(tecINSUFFICIENT_FUNDS));
            env.close();

            // clear global freeze
            env(fclear(gw, asfGlobalFreeze));
            env.close();

            // bob can buy
            env(buy(bob, id, USD(10)));
            env.close();
        }
        // test Individual Freeze
        {
            // Env Setup
            Env env{*this, features};
            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env(trust(alice, USD(10000)));
            env(trust(bob, USD(10000)));
            env.close();
            env(pay(gw, alice, USD(1000)));
            env(pay(gw, bob, USD(1000)));
            env.close();

            // set freeze on alice trustline
            env(trust(gw, USD(10000), bob, tfSetFreeze));
            env.close();

            // setup mint
            std::string const uri(maxTokenURILength, '?');
            std::string const id{strHex(tokenid(alice, uri))};
            env(mint(alice, uri));
            env(sell(alice, id, USD(10)));
            env.close();

            // buy uritoken fails - frozen trustline
            env(buy(bob, id, USD(10)), ter(tecINSUFFICIENT_FUNDS));
            env.close();

            // clear freeze on alice trustline
            env(trust(gw, USD(10000), bob, tfClearFreeze));
            env.close();

            // buy uri success
            env(buy(bob, id, USD(10)));
            env.close();
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testMintInvalid(features);
        testBurnInvalid(features);
        testSellInvalid(features);
        testBuyInvalid(features);
        testClearInvalid(features);
        testMintValid(features);
        testBurnValid(features);
        testBuyValid(features);
        testSellValid(features);
        testClearValid(features);
        testMetaAndOwnership(features);
        // testAccountDelete(features);
        // testUsingTickets(features);
        testRippleState(features);
        // testGateway(features);
        testRequireAuth(features);
        testGlobalFreeze(features);
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
