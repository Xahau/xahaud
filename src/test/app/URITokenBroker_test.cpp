//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
    SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <test/jtx/AMM.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/TxFlags.h>

namespace ripple {
namespace test {

struct URITokenBroker_test : public beast::unit_test::suite
{
    static Json::Value
    brokeredBuy(
        jtx::Account const& buyer,
        std::string const& tokenID,
        STAmount const& amount,
        jtx::Account const& broker)
    {
        auto tx = jtx::uritoken::buy(buyer, tokenID);
        tx[sfAmount.jsonName] = amount.getJson(JsonOptions::none);
        tx[sfBrokerAccount.jsonName] = broker.human();
        return tx;
    }

    static std::string
    mintAndSell(
        jtx::Env& env,
        jtx::Account const& seller,
        std::string const& uri,
        STAmount const& amount,
        std::optional<jtx::Account> const& destination = std::nullopt)
    {
        auto const id = strHex(jtx::uritoken::tokenid(seller, uri));
        env(jtx::uritoken::mint(seller, uri));
        if (destination)
            env(jtx::uritoken::sell(seller, id),
                jtx::uritoken::amt(amount),
                jtx::uritoken::dest(*destination));
        else
            env(jtx::uritoken::sell(seller, id), jtx::uritoken::amt(amount));
        env.close();
        return id;
    }

    static std::shared_ptr<SLE const>
    token(jtx::Env const& env, uint256 const& id)
    {
        return env.le(Keylet{ltURI_TOKEN, id});
    }

    static std::shared_ptr<SLE const>
    token(jtx::Env const& env, std::string const& id)
    {
        uint256 tokenID;
        if (!tokenID.parseHex(id))
            return nullptr;
        return token(env, tokenID);
    }

    void
    testAmendment(FeatureBitset const& features)
    {
        testcase("amendment");
        using namespace jtx;

        Account const seller{"seller"};
        Account const buyer{"buyer"};
        Account const broker{"broker"};

        // A broker-less buy remains valid when the broker amendment is off.
        {
            Env env{*this, features - featureURITokenBroker};
            env.fund(XRP(1000), seller, buyer, broker);
            auto const id = mintAndSell(env, seller, "no-broker", XRP(10));
            env(uritoken::buy(buyer, id), uritoken::amt(XRP(10)));
        }

        // BrokerAccount requires both URITokenBroker and fixXahauV1.
        for (auto const disabled : {featureURITokenBroker, fixXahauV1})
        {
            Env env{*this, features - disabled};
            env.fund(XRP(1000), seller, buyer, broker);
            auto const id =
                mintAndSell(env, seller, to_string(disabled), XRP(10));
            env(brokeredBuy(buyer, id, XRP(11), broker), ter(temDISABLED));
        }
    }

    void
    testMalformed(FeatureBitset const& features)
    {
        testcase("preflight");
        using namespace jtx;

        Env env{*this, features};
        Account const seller{"seller"};
        Account const buyer{"buyer"};
        env.fund(XRP(1000), seller, buyer);
        auto const id = mintAndSell(env, seller, "preflight", XRP(10));

        env(brokeredBuy(buyer, id, XRP(11), buyer), ter(temREDUNDANT));

        auto zero = uritoken::buy(buyer, id);
        zero[sfAmount.jsonName] = XRP(11).value().getJson(JsonOptions::none);
        zero[sfBrokerAccount.jsonName] = "rrrrrrrrrrrrrrrrrrrrrhoLvTp";
        env(zero, ter(temMALFORMED));

        // BrokerAccount is only part of the URITokenBuy template.
        auto mint = uritoken::mint(seller, "broker-on-mint");
        mint[sfBrokerAccount.jsonName] = buyer.human();
        env(mint, ter(temMALFORMED));

        // BrokerAccount belongs only to URITokenBuy's serialization template.
        auto payment = pay(buyer, seller, XRP(1));
        payment[sfBrokerAccount.jsonName] = seller.human();
        env(payment, ter(temMALFORMED));
    }

    void
    testPreclaim(FeatureBitset const& features)
    {
        testcase("preclaim");
        using namespace jtx;

        Account const seller{"seller"};
        Account const buyer{"buyer"};
        Account const broker{"broker"};
        Account const missing{"missing"};

        // Missing broker account.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer);
            env.memoize(missing);
            auto const id = mintAndSell(env, seller, "missing", XRP(10));
            env(brokeredBuy(buyer, id, XRP(11), missing), ter(tecNO_TARGET));
        }

        // The seller cannot also receive the broker spread.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer);
            auto const id = mintAndSell(env, seller, "seller", XRP(10));
            env(brokeredBuy(buyer, id, XRP(11), seller), ter(tecNO_PERMISSION));
        }

        // A brokered sale requires both a positive price and positive spread.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer, broker);
            auto const zeroID = mintAndSell(env, seller, "zero", XRP(0), buyer);
            env(brokeredBuy(buyer, zeroID, XRP(1), broker),
                ter(tecNO_PERMISSION));
            // The legacy destination-restricted zero-price buy is unchanged.
            env(uritoken::buy(buyer, zeroID), uritoken::amt(XRP(0)));

            auto const id = mintAndSell(env, seller, "spread", XRP(10));
            env(brokeredBuy(buyer, id, XRP(9), broker),
                ter(tecINSUFFICIENT_PAYMENT));
            env(brokeredBuy(buyer, id, XRP(10), broker), ter(tecNO_PERMISSION));
        }

        // An IOU broker must already have a trust line.
        {
            Env env{*this, features};
            Account const gw{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), seller, buyer, broker, gw);
            env.trust(USD(1000), seller, buyer);
            env.close();
            env(pay(gw, buyer, USD(100)));
            auto const id = mintAndSell(env, seller, "no-line", USD(10));
            env(brokeredBuy(buyer, id, USD(11), broker), ter(tecNO_LINE));
        }

        // AMM pseudo-accounts cannot be named as brokers.
        {
            Env env{*this, features | featureAMM};
            Account const gw{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(30'000), seller, buyer, gw);
            env.trust(USD(30'000), seller, buyer);
            env.close();
            env(pay(gw, seller, USD(20'000)));
            AMM amm{env, seller, XRP(10'000), USD(10'000)};

            auto const id = mintAndSell(env, seller, "amm", XRP(10));
            auto tx = uritoken::buy(buyer, id);
            tx[sfAmount.jsonName] = XRP(11).value().getJson(JsonOptions::none);
            tx[sfBrokerAccount.jsonName] = toBase58(amm.ammAccount());
            env(tx, ter(tecNO_PERMISSION));
        }
    }

    void
    testXAH(FeatureBitset const& features)
    {
        testcase("XAH settlement");
        using namespace jtx;

        Env env{*this, features};
        Account const seller{"seller"};
        Account const buyer{"buyer"};
        Account const broker{"broker"};
        env.fund(XRP(1000), seller, buyer, broker);
        env(fset(seller, asfDepositAuth));
        env(fset(broker, asfDepositAuth));
        env.close();

        auto const id = mintAndSell(env, seller, "xah", XRP(100));
        auto const sellerBefore = env.balance(seller);
        auto const buyerBefore = env.balance(buyer);
        auto const brokerBefore = env.balance(broker);
        auto const buyerOwners = env.ownerCount(buyer);
        auto const brokerOwners = env.ownerCount(broker);
        auto const fee = env.current()->fees().base;

        env(brokeredBuy(buyer, id, XRP(110), broker));
        env.close();

        BEAST_EXPECT(env.balance(seller) == sellerBefore + XRP(100));
        BEAST_EXPECT(env.balance(broker) == brokerBefore + XRP(10));
        BEAST_EXPECT(env.balance(buyer) == buyerBefore - XRP(110) - fee);
        BEAST_EXPECT(env.ownerCount(buyer) == buyerOwners + 1);
        BEAST_EXPECT(env.ownerCount(broker) == brokerOwners);

        auto const sle = token(env, id);
        if (!BEAST_EXPECT(sle))
            return;
        BEAST_EXPECT((*sle)[sfOwner] == buyer.id());
        BEAST_EXPECT(!sle->isFieldPresent(sfAmount));
        BEAST_EXPECT(!sle->isFieldPresent(sfDestination));
    }

    void
    testXAHReserve(FeatureBitset const& features)
    {
        testcase("XAH reserve boundary");
        using namespace jtx;

        Env env{*this, features};
        Account const seller{"seller"};
        Account const buyer{"buyer"};
        Account const broker{"broker"};
        env.fund(XRP(1000), seller, broker);
        env.fund(XRP(300), buyer);
        env.close();

        auto const id = mintAndSell(env, seller, "reserve", XRP(1));
        auto const sellerBefore = env.balance(seller);
        auto const brokerBefore = env.balance(broker);

        // The buyer needs the next owner reserve plus the full signed amount,
        // not merely the seller's one-XAH listing amount.
        env(brokeredBuy(buyer, id, XRP(51), broker),
            ter(tecINSUFFICIENT_FUNDS));

        BEAST_EXPECT(env.balance(seller) == sellerBefore);
        BEAST_EXPECT(env.balance(broker) == brokerBefore);
        auto const sle = token(env, id);
        BEAST_EXPECT(sle && (*sle)[sfOwner] == seller.id());
    }

    void
    testIOU(FeatureBitset const& features)
    {
        testcase("IOU settlement");
        using namespace jtx;

        Account const seller{"seller"};
        Account const buyer{"buyer"};
        Account const broker{"broker"};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];

        // Both recipients bear the transfer rate; buyer debit remains exact.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer, broker, gw);
            env(rate(gw, 1.25));
            env.trust(USD(1000), seller, buyer, broker);
            env.close();
            env(trust(gw, seller["USD"](1000), tfClearNoRipple));
            env(trust(gw, buyer["USD"](1000), tfClearNoRipple));
            env(trust(gw, broker["USD"](1000), tfClearNoRipple));
            env.close();
            env(pay(gw, buyer, USD(1000)));
            auto const id = mintAndSell(env, seller, "rate", USD(100));
            auto const buyerBefore = env.balance(buyer, USD.issue());
            auto const brokerOwners = env.ownerCount(broker);

            env(brokeredBuy(buyer, id, USD(110), broker));
            env.close();

            BEAST_EXPECT(env.balance(seller, USD.issue()) == USD(80));
            BEAST_EXPECT(env.balance(broker, USD.issue()) == USD(8));
            BEAST_EXPECT(
                env.balance(buyer, USD.issue()) == buyerBefore - USD(110));
            BEAST_EXPECT(env.ownerCount(broker) == brokerOwners);
        }

        // The issuer may be the broker and needs no trust line.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer, gw);
            env(rate(gw, 1.25));
            env.trust(USD(1000), seller, buyer);
            env.close();
            env(trust(gw, seller["USD"](1000), tfClearNoRipple));
            env(trust(gw, buyer["USD"](1000), tfClearNoRipple));
            env.close();
            env(pay(gw, buyer, USD(1000)));
            auto const id = mintAndSell(env, seller, "issuer", USD(100));
            auto const buyerBefore = env.balance(buyer, USD.issue());

            env(brokeredBuy(buyer, id, USD(110), gw));
            env.close();

            BEAST_EXPECT(env.balance(seller, USD.issue()) == USD(80));
            BEAST_EXPECT(
                env.balance(buyer, USD.issue()) == buyerBefore - USD(110));
        }

        // Existing line limits are not consulted and no owner object is added.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer, broker, gw);
            env.trust(USD(1000), seller, buyer);
            env.trust(USD(1), broker);
            env.close();
            env(trust(gw, seller["USD"](1000), tfClearNoRipple));
            env(trust(gw, buyer["USD"](1000), tfClearNoRipple));
            env(trust(gw, broker["USD"](1), tfClearNoRipple));
            env.close();
            env(pay(gw, buyer, USD(100)));
            env(pay(gw, broker, USD(1)));
            auto const id = mintAndSell(env, seller, "limit", USD(10));
            auto const brokerOwners = env.ownerCount(broker);

            env(brokeredBuy(buyer, id, USD(20), broker));
            env.close();

            BEAST_EXPECT(env.balance(broker, USD.issue()) == USD(11));
            BEAST_EXPECT(env.ownerCount(broker) == brokerOwners);
        }
    }

    void
    testIOUPermissions(FeatureBitset const& features)
    {
        testcase("IOU permissions");
        using namespace jtx;

        Account const seller{"seller"};
        Account const buyer{"buyer"};
        Account const broker{"broker"};
        Account const gw{"gateway"};
        auto const USD = gw["USD"];

        // A frozen broker line rejects the broker leg atomically.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer, broker, gw);
            env.trust(USD(1000), seller, buyer, broker);
            env.close();
            env(trust(gw, seller["USD"](1000), tfClearNoRipple));
            env(trust(gw, buyer["USD"](1000), tfClearNoRipple));
            env(pay(gw, buyer, USD(100)));
            env(trust(gw, broker["USD"](1000), tfSetFreeze | tfClearNoRipple));
            env.close();
            auto const id = mintAndSell(env, seller, "freeze", USD(10));
            auto const buyerBefore = env.balance(buyer, USD.issue());

            env(brokeredBuy(buyer, id, USD(11), broker), ter(tecFROZEN));
            BEAST_EXPECT(env.balance(seller, USD.issue()) == USD(0));
            BEAST_EXPECT(env.balance(broker, USD.issue()) == USD(0));
            BEAST_EXPECT(env.balance(buyer, USD.issue()) == buyerBefore);
        }

        // Under RequireAuth, an existing but unauthorized broker line fails.
        {
            Env env{*this, features};
            auto const sellerUSD = seller["USD"];
            auto const buyerUSD = buyer["USD"];
            env.fund(XRP(1000), seller, buyer, broker, gw);
            env(fset(gw, asfRequireAuth));
            env.close();
            env.trust(USD(1000), seller, buyer, broker);
            env(trust(gw, sellerUSD(1000)),
                txflags(tfSetfAuth | tfClearNoRipple));
            env(trust(gw, buyerUSD(1000)),
                txflags(tfSetfAuth | tfClearNoRipple));
            env(trust(gw, broker["USD"](1000)), txflags(tfClearNoRipple));
            env.close();
            env(pay(gw, buyer, USD(100)));
            auto const id = mintAndSell(env, seller, "auth", USD(10));

            env(brokeredBuy(buyer, id, USD(11), broker), ter(tecNO_AUTH));
        }

        // NoRipple on the broker leg rejects the whole transaction; the
        // already-computed seller leg must also be rolled back.
        {
            Env env{*this, features};
            env.fund(XRP(1000), seller, buyer, broker, gw);
            env.trust(USD(1000), seller, buyer, broker);
            env.close();
            env(trust(gw, seller["USD"](1000), tfClearNoRipple));
            env(trust(gw, buyer["USD"](1000), tfClearNoRipple));
            env(pay(gw, buyer, USD(100)));
            env(trust(gw, broker["USD"](1000), tfSetNoRipple));
            env.close();
            auto const id = mintAndSell(env, seller, "no-ripple", USD(10));
            auto const sellerBefore = env.balance(seller, USD.issue());
            auto const buyerBefore = env.balance(buyer, USD.issue());
            auto const brokerBefore = env.balance(broker, USD.issue());

            env(brokeredBuy(buyer, id, USD(11), broker), ter(tecPATH_DRY));

            BEAST_EXPECT(env.balance(seller, USD.issue()) == sellerBefore);
            BEAST_EXPECT(env.balance(buyer, USD.issue()) == buyerBefore);
            BEAST_EXPECT(env.balance(broker, USD.issue()) == brokerBefore);
            auto const sle = token(env, id);
            BEAST_EXPECT(sle && (*sle)[sfOwner] == seller.id());
        }
    }

public:
    void
    run() override
    {
        auto const features = jtx::supported_amendments();
        testAmendment(features);
        testMalformed(features);
        testPreclaim(features);
        testXAH(features);
        testXAHReserve(features);
        testIOU(features);
        testIOUPermissions(features);
    }
};

BEAST_DEFINE_TESTSUITE(URITokenBroker, app, ripple);

}  // namespace test
}  // namespace ripple
