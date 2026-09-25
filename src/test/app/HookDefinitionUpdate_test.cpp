//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  S
    OFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <test/jtx.h>
#include <test/jtx/genesis.h>
#include <test/jtx/hook.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

class HookDefinitionUpdate_test : public beast::unit_test::suite
{
    static Json::Value
    hookDefUpdateTx(
        jtx::Account const& account,
        std::string const& hookHash,
        uint32_t flags = tfValidateGuards)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::HookDefinitionUpdate;
        jv[jss::Account] = account.human();
        jv[jss::HookHash] = hookHash;
        jv[jss::Flags] = flags;
        return jv;
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("hook definition update invalid preflight");

        using namespace test::jtx;

        auto const alice = Account("alice");

        // temDISABLED
        {
            Env env{*this, features - featureHookFeeV2};
            env.fund(XRP(1000), alice);
            env.close();

            env(hookDefUpdateTx(alice, ""), ter(temDISABLED));
        }

        // temINVALID_FLAG
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice);
            env.close();

            env(hookDefUpdateTx(alice, "", 0x00000002), ter(temINVALID_FLAG));
        }
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("hook definition update invalid preclaim");

        using namespace test::jtx;
        auto const alice = Account("alice");

        // tecNO_ENTRY
        {
            Env env{*this};
            env.fund(XRP(1000), alice);
            env.close();

            env(hookDefUpdateTx(alice, ""), ter(tecNO_ENTRY));
        }
    }

    void
    testDoApply(FeatureBitset features)
    {
        testcase("hook definition update do apply");

        using namespace test::jtx;
        auto const alice = Account("alice");

        Env env{*this, features - featureHookFeeV2};
        env.fund(XRP(1000), alice);
        env.close();

        auto const hookCode = jtx::genesis::AcceptHook;
        uint256 const hash = ripple::sha512Half_s(
            ripple::Slice(hookCode.data(), hookCode.size()));

        // prepare: Create Hook Definition without HookFeeV2
        auto hookObj = hso(hookCode);
        env(hook(alice, {{hookObj}}, 0), fee(XRP(1)));
        env.close();
        // prepare: enable HookFeeV2
        env.enableFeature(featureHookFeeV2);
        env.close();

        // 1. Update Hook Definition (Fee -> Cost)
        {
            auto leb = env.le(keylet::hookDefinition(hash));
            BEAST_EXPECT(leb->isFieldPresent(sfFee));
            BEAST_EXPECT(!leb->isFieldPresent(sfHookCost));
            env(hookDefUpdateTx(alice, to_string(hash)), fee(XRP(1)));
            env.close();

            auto lea = env.le(keylet::hookDefinition(hash));
            BEAST_EXPECT(lea->isFieldPresent(sfHookCost));
            BEAST_EXPECT(!lea->isFieldPresent(sfFee));
        }
        // 2. Update Hook Definition (Cost -> Cost)
        {
            env(hookDefUpdateTx(alice, to_string(hash)), fee(XRP(1)));
            env.close();
            auto const meta = env.meta();
            BEAST_EXPECT(meta->isFieldPresent(sfAffectedNodes));

            auto& nodes = meta->getFieldArray(sfAffectedNodes);
            BEAST_EXPECT(nodes.size() == 1);
            // no update for ltHookDefinition
            BEAST_EXPECT(
                nodes[0].getFieldU16(sfLedgerEntryType) == ltACCOUNT_ROOT);
        }

        // TODO: test callback cost
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
        testDoApply(features);
    }
};

BEAST_DEFINE_TESTSUITE(HookDefinitionUpdate, app, ripple);

}  // namespace test
}  // namespace ripple
