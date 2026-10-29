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
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/tx/applySteps.h>
#include <xrpl/protocol/jss.h>

namespace ripple {
namespace test {

class HookDefinitionUpdate_test : public beast::unit_test::suite
{
    // Minimal hook with both entry points, compiled from:
    //   (module
    //     (type (func (param i32 i32) (result i32)))
    //     (type (func (param i32 i32 i64) (result i64)))
    //     (type (func (param i32) (result i64)))
    //     (import "env" "_g" (func $_g (type 0)))
    //     (import "env" "accept" (func $accept (type 1)))
    //     (memory 1)
    //     (export "memory" (memory 0))
    //     (func $hook (type 2) (param i32) (result i64)
    //       i32.const 0 i32.const 0 i64.const 0 call $accept)
    //     (func $cbak (type 2) (param i32) (result i64)
    //       i32.const 0 i32.const 0 i64.const 0 call $accept)
    //     (export "hook" (func $hook))
    //     (export "cbak" (func $cbak)))
    // Under featureHookFeeV2 both cost 4 instructions + 1 api call = 104 (plus
    // one for the final end, 105).
    static inline std::vector<uint8_t> const hookWithCbak = {
        0x00U, 0x61U, 0x73U, 0x6DU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x13U,
        0x03U, 0x60U, 0x02U, 0x7FU, 0x7FU, 0x01U, 0x7FU, 0x60U, 0x03U, 0x7FU,
        0x7FU, 0x7EU, 0x01U, 0x7EU, 0x60U, 0x01U, 0x7FU, 0x01U, 0x7EU, 0x02U,
        0x17U, 0x02U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x02U, 0x5FU, 0x67U, 0x00U,
        0x00U, 0x03U, 0x65U, 0x6EU, 0x76U, 0x06U, 0x61U, 0x63U, 0x63U, 0x65U,
        0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x03U, 0x02U, 0x02U, 0x02U, 0x05U,
        0x03U, 0x01U, 0x00U, 0x01U, 0x07U, 0x18U, 0x03U, 0x06U, 0x6DU, 0x65U,
        0x6DU, 0x6FU, 0x72U, 0x79U, 0x02U, 0x00U, 0x04U, 0x68U, 0x6FU, 0x6FU,
        0x6BU, 0x00U, 0x02U, 0x04U, 0x63U, 0x62U, 0x61U, 0x6BU, 0x00U, 0x03U,
        0x0AU, 0x17U, 0x02U, 0x0AU, 0x00U, 0x41U, 0x00U, 0x41U, 0x00U, 0x42U,
        0x00U, 0x10U, 0x01U, 0x0BU, 0x0AU, 0x00U, 0x41U, 0x00U, 0x41U, 0x00U,
        0x42U, 0x00U, 0x10U, 0x01U, 0x0BU};

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

        // temINVALID_FLAG: without tfValidateGuards the transaction would do
        // nothing but burn its fee
        {
            Env env{*this, features};
            env.fund(XRP(1000), alice);
            env.close();

            env(hookDefUpdateTx(alice, "", 0), ter(temINVALID_FLAG));
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
    }

    void
    testCallbackCostMigration(FeatureBitset features)
    {
        testcase("hook definition update callback cost migration");

        using namespace test::jtx;
        auto const alice = Account("alice");
        auto const bob = Account("bob");

        Env env{*this, features - featureHookFeeV2};
        env.fund(XRP(1000), alice, bob);
        env.close();

        uint256 const hash = ripple::sha512Half_s(
            ripple::Slice(hookWithCbak.data(), hookWithCbak.size()));
        auto const defKeylet = keylet::hookDefinition(hash);

        env(hook(alice, {{hso(hookWithCbak)}}, 0), fee(XRP(1)));
        env.close();

        // base fee plus the execution fee of alice's hook chain
        auto const invokeFee = [&]() {
            auto const jv = invoke::invoke(alice);
            return calculateBaseFee(*env.current(), *env.jt(jv).stx);
        };
        auto const base = env.current()->fees().base;

        // before: V1 fields, fee charged from sfFee
        {
            auto const def = env.le(defKeylet);
            if (!BEAST_EXPECT(def))
                return;
            BEAST_EXPECT(def->isFieldPresent(sfFee));
            BEAST_EXPECT(def->isFieldPresent(sfHookCallbackFee));
            BEAST_EXPECT(!def->isFieldPresent(sfHookCost));
            BEAST_EXPECT(!def->isFieldPresent(sfHookCallbackCost));
            BEAST_EXPECT(
                invokeFee() == base + def->getFieldAmount(sfFee).xrp());
        }

        env.enableFeature(featureHookFeeV2);
        env.close();

        // anyone, not only the hook owner, can migrate the definition
        env(hookDefUpdateTx(bob, to_string(hash)), fee(XRP(1)));
        env.close();

        {
            auto const def = env.le(defKeylet);
            if (!BEAST_EXPECT(def))
                return;
            BEAST_EXPECT(!def->isFieldPresent(sfFee));
            BEAST_EXPECT(!def->isFieldPresent(sfHookCallbackFee));
            BEAST_EXPECT(def->isFieldPresent(sfHookCost));
            BEAST_EXPECT(def->isFieldPresent(sfHookCallbackCost));
            BEAST_EXPECTS(
                def->getFieldU64(sfHookCost) == 105,
                std::to_string(def->getFieldU64(sfHookCost)));
            BEAST_EXPECTS(
                def->getFieldU64(sfHookCallbackCost) == 105,
                std::to_string(def->getFieldU64(sfHookCallbackCost)));
            BEAST_EXPECT(
                invokeFee() ==
                base + hook::hookCostToFee(def->getFieldU64(sfHookCost)));
        }

        // second update is a no-op for the hook definition
        env(hookDefUpdateTx(bob, to_string(hash)), fee(XRP(1)));
        env.close();
        {
            auto const meta = env.meta();
            if (!BEAST_EXPECT(meta))
                return;
            for (auto const& node : meta->getFieldArray(sfAffectedNodes))
                BEAST_EXPECT(
                    node.getFieldU16(sfLedgerEntryType) != ltHOOK_DEFINITION);
        }
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
        testCallbackCostMigration(features);
    }
};

BEAST_DEFINE_TESTSUITE(HookDefinitionUpdate, app, ripple);

}  // namespace test
}  // namespace ripple
