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

#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class Invoke_test : public beast::unit_test::suite
{
    static Json::Value
    invoke(jtx::Account const& account)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::Invoke;
        jv[jss::Account] = account.human();
        return jv;
    }

    XRPAmount
    calcParamFee(Json::Value jv)
    {
        uint64_t paramBytes = 0;
        auto const& params = jv["HookParameters"];
        for (auto const& param : params)
        {
            auto const& hparam = param["HookParameter"];
            paramBytes +=
                (hparam.isMember("HookParameterName")
                     ? hparam["HookParameterName"].asString().size() / 2
                     : 0) +
                (hparam.isMember("HookParameterValue")
                     ? hparam["HookParameterValue"].asString().size() / 2
                     : 0);
        }
        return XRPAmount{static_cast<XRPAmount>(paramBytes)};
    }

    void
    testInvalidPreflight1(FeatureBitset features)
    {
        testcase("invoke invalid preflight");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const bob = Account("bob");

        env.fund(XRP(1000), alice, bob);
        env.close();

        std::string jsonBlob = R"json({
            "mykey": "DEADBEEF",
        })json";

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("invoke invalid preflight");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkConfig(21337)};

        auto const alice = Account("alice");

        env.fund(XRP(1000), alice);
        env.close();

        //----------------------------------------------------------------------
        // preflight

        // temMALFORMED - Invoke: blob was more than 128kib
        {
            ripple::Blob blob;
            blob.resize(129 * 1024);

            XRPAmount const extraFee =
                XRPAmount{static_cast<XRPAmount>(blob.size())};

            env(invoke::invoke(alice),
                invoke::blob(strHex(blob)),
                fee(10 + extraFee),
                ter(temMALFORMED));
        }
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("invoke invalid preclaim");

        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337),
                features - featureHooks};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(invoke::invoke(alice), ter(temDISABLED));
        }

        // terNO_ACCOUNT
        {
            test::jtx::Env env{*this, network::makeNetworkConfig(21337)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            env.memoize(alice);

            Json::Value tx = invoke::invoke(alice);
            tx[jss::Sequence] = 0;
            env(tx, fee(feeDrops), ter(terNO_ACCOUNT));
        }

        // tecNO_TARGET
        {
            test::jtx::Env env{*this, network::makeNetworkConfig(21337)};
            auto const feeDrops = env.current()->fees().base;

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.memoize(bob);

            env.fund(XRP(1000), alice);
            env.close();

            env(invoke::invoke(alice),
                invoke::dest(bob),
                fee(feeDrops),
                ter(tecNO_TARGET));
        }
    }

    void
    testInvoke(FeatureBitset features)
    {
        testcase("invoke");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, network::makeNetworkConfig(21337), features};
        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // W/ Blob
        {
            ripple::Blob blob;
            blob.resize(128 * 1024);
            XRPAmount const extraFee =
                XRPAmount{static_cast<XRPAmount>(blob.size())};
            env(invoke::invoke(alice),
                invoke::blob(strHex(blob)),
                fee(feeDrops + extraFee),
                ter(tesSUCCESS));
        }

        // No Destination
        {
            env(invoke::invoke(alice), ter(tesSUCCESS));
        }
        // W/ Destination
        {
            env(invoke::invoke(alice), invoke::dest(bob), ter(tesSUCCESS));
        }
    }

    void
    testUsingTickets(FeatureBitset features)
    {
        testcase("invoke using tickets");
        using namespace jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};
        auto const alice = Account("alice");
        env.fund(XRP(10000), alice);
        std::uint32_t aliceTicketSeq{env.seq(alice) + 1};
        env(ticket::create(alice, 10));
        std::uint32_t const aliceSeq{env.seq(alice)};
        env.require(owners(alice, 10));

        env(invoke::invoke(alice),
            ticket::use(aliceTicketSeq++),
            ter(tesSUCCESS));
        env(invoke::invoke(alice),
            ticket::use(aliceTicketSeq++),
            ter(tesSUCCESS));

        env.require(tickets(alice, env.seq(alice) - aliceTicketSeq));
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        env.require(owners(alice, 8));
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testWithFeats(all);
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testInvalidPreflight(features);
        testInvalidPreclaim(features);
        testInvoke(features);
        testUsingTickets(features);
    }
};

BEAST_DEFINE_TESTSUITE(Invoke, app, ripple);

}  // namespace test
}  // namespace ripple
