//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2016 Ripple Labs Inc.

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

#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

struct Option_test : public beast::unit_test::suite
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

    static STAmount
    lockedValue(
        jtx::Env const& env,
        jtx::Account const& account,
        std::uint32_t const& seq)
    {
        auto const sle = env.le(keylet::optionOffer(account, seq));
        if (sle->isFieldPresent(sfAmount))
            return (*sle)[sfAmount];
        return STAmount(0);
    }

    Json::Value
    optionlist(
        jtx::Account const& account,
        std::uint32_t expiration,
        STAmount const& amount)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionList;
        jv[jss::Account] = account.human();
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        jv[sfExpiration.jsonName] = expiration;
        return jv;
    }

    Json::Value
    optioncreate(
        jtx::Account const& account,
        uint256 const& optionId,
        uint32_t const& quantity,
        STAmount const& amount)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionCreate;
        jv[jss::Account] = account.human();
        jv[sfOfferID.jsonName] = to_string(optionId);
        jv[jss::Amount] = amount.getJson(JsonOptions::none);
        jv[sfQualityIn.jsonName] = quantity;
        return jv;
    }

    Json::Value
    optionexecute(
        jtx::Account const& account,
        uint256 const& optionId,
        uint256 const& offerId)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::OptionExecute;
        jv[jss::Account] = account.human();
        jv[sfOfferID.jsonName] = to_string(optionId);
        jv[sfInvoiceID.jsonName] = to_string(offerId);
        ;
        return jv;
    }

    static uint256
    getOptionIndex(
        AccountID const& issuer,
        std::uint32_t expiration)
    {
        return keylet::option(issuer, expiration).key;
    }

    static uint256
    getOfferIndex(AccountID const& account, std::uint32_t sequence)
    {
        return keylet::optionOffer(account, sequence).key;
    }

    void
    testBookBuy(FeatureBitset features)
    {
        testcase("book buy");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        IOU const USD(gw["USD"]);

        env.fund(XRP(1000), gw, alice, bob);
        env.close();
        env.trust(USD(100000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env(pay(gw, bob, USD(10000)));
        env.close();

        // Alice offers to sell 100 XRP for 110 USD.
        // Create an sell: TakerPays 100, TakerGets 110/USD
        env(offer(alice, XRP(100), USD(110)), txflags(tfSell));  // <- Best
        // Alice offers to sell 100 XRP for 100 USD.
        // Create an sell: TakerPays 100, TakerGets 100/USD
        env(offer(alice, XRP(100), USD(100)), txflags(tfSell));
        // Alice offers to sell 100 XRP for 90 USD.
        // Create an sell: TakerPays 100, TakerGets 90/USD
        env(offer(alice, XRP(100), USD(90)), txflags(tfSell));
        // Alice offers to sell 100 XRP for 80 USD.
        // Create an sell: TakerPays 100, TakerGets 80/USD
        env(offer(alice, XRP(100), USD(80)), txflags(tfSell));
        // Alice offers to sell 100 XRP for 70 USD.
        // Create an sell: TakerPays 100, TakerGets 70/USD
        env(offer(alice, XRP(100), USD(70)), txflags(tfSell));  // <- Worst
        env.close();

        // Bob offers to buy 110 USD for 100 XRP.
        // env(offer(bob, USD(110), XRP(100))); // <- Best

        Book const book1{
            xrpIssue(),
            USD.issue(),
        };

        const uint256 uBookBase1 = getBookBase(book1);
        const uint256 uBookEnd1 = getQualityNext(uBookBase1);
        auto view1 = env.closed();
        auto key1 = view1->succ(uBookBase1, uBookEnd1);
        if (key1)
        {
            auto sleOfferDir1 = view1->read(keylet::page(key1.value()));
            uint256 offerIndex1;
            unsigned int bookEntry1;
            cdirFirst(
                *view1,
                sleOfferDir1->key(),
                sleOfferDir1,
                bookEntry1,
                offerIndex1);
            auto sleOffer1 = view1->read(keylet::offer(offerIndex1));
            auto const dir1 =
                to_string(sleOffer1->getFieldH256(sfBookDirectory));
            auto const uTipIndex1 = sleOfferDir1->key();
            STAmount dirRate1 = amountFromQuality(getQuality(uTipIndex1));
            auto const rate1 = dirRate1 / 1'000'000;
            std::cout << "dirRate1: " << dirRate1 << "\n";
            std::cout << "rate1: " << rate1 << "\n";
            std::cout << "rate1=: " << (100 / rate1) << "\n";
            BEAST_EXPECT(100 / rate1 == 110);
        }
    }

    void
    testBookSell(FeatureBitset features)
    {
        testcase("book sell");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const gw = Account("gw");
        IOU const USD(gw["USD"]);

        env.fund(XRP(1000), gw, alice, bob);
        env.close();
        env.trust(USD(100000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env(pay(gw, bob, USD(10000)));
        env.close();

        // Bob offers to buy 70 XRP for 100 USD.
        // Create an buy: TakerPays 100/USD, TakerGets 70
        env(offer(bob, USD(100), XRP(70)));  // <- Worst
        // Bob offers to buy 80 XRP for 100 USD.
        // Create an buy: TakerPays 100/USD, TakerGets 80
        env(offer(bob, USD(100), XRP(80)));
        // Bob offers to buy 90 XRP for 100 USD.
        // Create an buy: TakerPays 100/USD, TakerGets 90
        env(offer(bob, USD(100), XRP(90)));
        // Bob offers to buy 100 XRP for 100 USD.
        // Create an buy: TakerPays 100/USD, TakerGets 100
        env(offer(bob, USD(100), XRP(100)));
        // Bob offers to buy 110 XRP for 100 USD.
        // Create an buy: TakerPays 100/USD, TakerGets 110
        env(offer(bob, USD(100), XRP(110)));  // <- Best
        env.close();

        // Alice offers to sell 110 XRP for 100 USD.
        // env(offer(alice, XRP(110), USD(100))); // <- Best

        Book const bookOpp{
            USD.issue(),
            xrpIssue(),
        };

        const uint256 uBookBaseOpp = getBookBase(bookOpp);
        const uint256 uBookEndOpp = getQualityNext(uBookBaseOpp);
        auto view = env.closed();
        auto key = view->succ(uBookBaseOpp, uBookEndOpp);
        if (key)
        {
            auto sleOfferDir = view->read(keylet::page(key.value()));
            uint256 offerIndex;
            unsigned int bookEntry;
            cdirFirst(
                *view, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex);
            auto sleOffer = view->read(keylet::offer(offerIndex));
            auto const dir = to_string(sleOffer->getFieldH256(sfBookDirectory));
            auto const uTipIndex = sleOfferDir->key();
            STAmount dirRate = amountFromQuality(getQuality(uTipIndex));
            auto const rate = dirRate * 1'000'000;
            std::cout << "dirRate: " << dirRate << "\n";
            std::cout << "rate: " << rate << "\n";
            std::cout << "rate=: " << (100 / rate) << "\n";
            BEAST_EXPECT(100 / rate == 110);
        }
    }

    void
    testOptionBookBuy(FeatureBitset features)
    {
        testcase("option book buy");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const broker = Account("broker");
        auto const gw = Account("gw");
        IOU const USD(gw["USD"]);

        env.fund(XRP(1000), gw, alice, bob, broker);
        env.close();
        env.trust(USD(100000), alice, bob);
        env.close();
        env(pay(gw, alice, USD(10000)));
        env(pay(gw, bob, USD(10000)));
        env.close();

        AccountID const zeroAcct{AccountID{}};
        auto const _exp = env.now() + 1s;
        auto const expiration = _exp.time_since_epoch().count();
        uint256 const optionId{getOptionIndex(zeroAcct, expiration)};
        env(optionlist(alice, expiration, XRP(10)), ter(tesSUCCESS));
        env.close();

        env(optioncreate(alice, optionId, 100, XRP(70)),
            txflags(tfAction),
            ter(tesSUCCESS));
        env(optioncreate(alice, optionId, 100, XRP(80)),
            txflags(tfAction),
            ter(tesSUCCESS));
        env(optioncreate(alice, optionId, 100, XRP(90)),
            txflags(tfAction),
            ter(tesSUCCESS));
        env(optioncreate(alice, optionId, 100, XRP(100)),
            txflags(tfAction),
            ter(tesSUCCESS));
        env(optioncreate(alice, optionId, 100, XRP(110)),
            txflags(tfAction),
            ter(tesSUCCESS));
        env.close();

        STAmount strikePrice = XRP(10);
        const uint256 uBookBaseOpp = getOptionBookBase(
            strikePrice.getIssuer(),
            strikePrice.getCurrency(),
            strikePrice.mantissa(),
            expiration);
        std::cout << "BOOK BASE: " << uBookBaseOpp << "\n";
        const uint256 uBookEndOpp = getOptionQualityNext(uBookBaseOpp);
        std::cout << "BOOK BASE END: " << uBookEndOpp << "\n";
        auto view = env.closed();
        auto key = view->succ(uBookBaseOpp, uBookEndOpp);
        if (key)
        {
            auto sleOfferDir = view->read(keylet::page(key.value()));
            uint256 offerIndex;
            unsigned int bookEntry;
            cdirFirst(
                *view, sleOfferDir->key(), sleOfferDir, bookEntry, offerIndex);
            auto sleOffer = view->read(keylet::unchecked(offerIndex));
            STAmount premium = sleOffer->getFieldAmount(sfAmount);
            auto const dir = to_string(sleOffer->getFieldH256(sfBookDirectory));
            std::cout << "dir: " << dir << "\n";
            auto const uTipIndex = sleOfferDir->key();
            auto const optionQuality = getOptionQuality(uTipIndex);
            std::cout << "optionQuality: " << optionQuality << "\n";
            STAmount dirRate = STAmount(premium.issue(), optionQuality);
            std::cout << "dirRate: " << dirRate << "\n";
            // BEAST_EXPECT(100 / rate == 110);
        }
    }

    void
    testEnabled(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("enabled");

        test::jtx::Env env{*this, network::makeNetworkConfig(21337), features};
        // test::jtx::Env env{
        //     *this,
        //     network::makeNetworkConfig(21337),
        //     features,
        //     nullptr,
        //     beast::severities::kWarning};
        
        auto const feeDrops = env.current()->fees().base;

        auto const writer = Account("alice");
        auto const buyer = Account("bob");
        auto const gw = Account("gateway");
        auto const USD = gw["USD"];

        env.fund(XRP(100000), writer, buyer, gw);
        env.close();

        BEAST_EXPECT(0 == 0);
        AccountID const zeroAcct{AccountID{}};

        auto const _exp = env.now() + 1s;
        auto const expiration = _exp.time_since_epoch().count();
        uint256 const optionId{getOptionIndex(zeroAcct, expiration)};

        auto preWriter = env.balance(writer);
        auto preBuyer = env.balance(buyer);

        auto const strikePrice = 10;
        env(optionlist(writer, expiration, XRP(strikePrice)),
            ter(tesSUCCESS));
        env.close();

        BEAST_EXPECT(env.balance(writer) == preWriter - feeDrops);

        // Call - Sell - Open
        auto const premium = 1;
        auto const quantity = 100;
        auto const seq = env.seq(writer);
        env(optioncreate(writer, optionId, quantity, XRP(premium)),
            txflags(tfAction),
            ter(tesSUCCESS));
        env.close();

        BEAST_EXPECT(
            env.balance(writer) ==
            preWriter - (feeDrops * 2) - XRP(quantity));
        BEAST_EXPECT(
            lockedValue(env, writer, seq) == XRP(quantity));

        preWriter = env.balance(writer);
        preBuyer = env.balance(buyer);

        // Call - Buy - Open
        auto const seq1 = env.seq(buyer);
        env(optioncreate(buyer, optionId, quantity, XRP(premium)),
            ter(tesSUCCESS));
        env.close();

        uint256 const offerId{getOfferIndex(buyer, seq1)};
        BEAST_EXPECT(
            env.balance(buyer) ==
            preBuyer - feeDrops - XRP(quantity * premium));
        BEAST_EXPECT(
            env.balance(writer) ==
            preWriter + XRP(quantity * premium));

        preWriter = env.balance(writer);
        preBuyer = env.balance(buyer);

        // Execute Option
        env(optionexecute(buyer, optionId, offerId), ter(tesSUCCESS));
        env.close();

        BEAST_EXPECT(
            env.balance(buyer) ==
            preBuyer - feeDrops - XRP(quantity * strikePrice) + XRP(quantity));
        BEAST_EXPECT(
            env.balance(writer) ==
            preWriter + XRP(quantity * strikePrice));
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        // testBookBuy(sa);
        // testBookSell(sa);
        // testOptionBookBuy(sa);
        testEnabled(sa);
    }
};

BEAST_DEFINE_TESTSUITE(Option, app, ripple);

}  // namespace test
}  // namespace ripple
