//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2019 Ripple Labs Inc.

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

#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class BaseFee_test : public beast::unit_test::suite
{
    static uint256
    tokenid(jtx::Account const& account, std::string const& uri)
    {
        auto const k = keylet::uritoken(account, Blob(uri.begin(), uri.end()));
        return k.key;
    }

    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->NETWORK_ID = networkID;
            Section config;
            config.append(
                {"reference_fee = 10",
                 "account_reserve = 1000000",
                 "owner_reserve = 200000"});
            auto setup = setup_FeeVote(config);
            cfg->FEES = setup;
            return cfg;
        });
    }

    void
    testInvoke(FeatureBitset features)
    {
        testcase("invoke w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};
        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto tx = uritoken::mint(alice, uri);
        tx[jss::HookParameters] = Json::Value{Json::arrayValue};
        tx[jss::HookParameters][0U] = Json::Value{};
        tx[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
        tx[jss::HookParameters][0U][jss::HookParameter]
            [jss::HookParameterName] = "CAFE";
        tx[jss::HookParameters][0U][jss::HookParameter]
            [jss::HookParameterValue] = "DEADBEEF";
        auto const jtx = env.jt(tx);

        // build tx_blob
        Json::Value params;
        params[jss::tx_blob] = strHex(jtx.stx->getSerializer().slice());

        // fee request
        auto const jrr = env.rpc("json", "fee", to_string(params));
        std::cout << "RESULT: " << jrr << "\n";

        // verify hooks fee
        auto const hooksFee = jrr[jss::result][jss::fee_hooks_feeunits];
        BEAST_EXPECT(hooksFee == "16");
    }

    void
    testURITokenFee(FeatureBitset features)
    {
        testcase("uritoken w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};
        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto tx = uritoken::mint(alice, uri);
        tx[jss::HookParameters] = Json::Value{Json::arrayValue};
        tx[jss::HookParameters][0U] = Json::Value{};
        tx[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
        tx[jss::HookParameters][0U][jss::HookParameter]
            [jss::HookParameterName] = "CAFE";
        tx[jss::HookParameters][0U][jss::HookParameter]
            [jss::HookParameterValue] = "DEADBEEF";
        auto const jtx = env.jt(tx);

        // build tx_blob
        Json::Value params;
        params[jss::tx_blob] = strHex(jtx.stx->getSerializer().slice());

        // fee request
        auto const jrr = env.rpc("json", "fee", to_string(params));
        std::cout << "RESULT: " << jrr << "\n";

        // verify hooks fee
        auto const hooksFee = jrr[jss::result][jss::fee_hooks_feeunits];
        BEAST_EXPECT(hooksFee == "16");
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testURITokenFee(features);
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

BEAST_DEFINE_TESTSUITE(BaseFee, app, ripple);

}  // namespace test
}  // namespace ripple
