//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL Labs

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
#include <ripple/app/hook/Enum.h>
#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/app/tx/impl/SetHook.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/app/SetJSHook_wasm.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <unordered_map>

// DA TODO: Move duplicated functions to jtx
// overrideFlag

namespace ripple {

namespace test {

#define DEBUG_TESTS 1

using TestHook = std::vector<uint8_t> const&;

// Identical to BEAST_EXPECT except it returns from the function
// if the condition isn't met (and would otherwise therefore cause a crash)
#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }

#define HASH_WASM(x)                                                           \
    [[maybe_unused]] uint256 const x##_hash =                                  \
        ripple::sha512Half_s(ripple::Slice(x##_wasm.data(), x##_wasm.size())); \
    [[maybe_unused]] std::string const x##_hash_str = to_string(x##_hash);     \
    [[maybe_unused]] Keylet const x##_keylet = keylet::hookDefinition(x##_hash);

class SetJSHook_test : public beast::unit_test::suite
{
private:
    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }

public:
// This is a large fee, large enough that we can set most small test hooks
// without running into fee issues we only want to test fee code specifically in
// fee unit tests, the rest of the time we want to ignore it.
#define HSFEE fee(100'000'000)
#define HSDROPS drops(99'000)
#define M(m) memo(m, "", "")
    void
    testHooksOwnerDir(FeatureBitset features)
    {
        testcase("Test owner directory");

        using namespace jtx;

        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(10000), alice, gw);
        env.close();
        env.trust(USD(100000), alice);
        env.close();
        env(pay(gw, alice, USD(10000)));

        for (int i = 1; i < 34; ++i)
        {
            std::string const uri(i, '?');
            env(uritoken::mint(alice, uri));
        }
        env.close();

        env(ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS, overrideFlag)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        env(ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS, overrideFlag)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // delete hook
        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};
        Json::Value iv;
        iv[jss::Flags] = 1;
        iv[jss::CreateCode] = "";
        jv[jss::Hooks][0U][jss::Hook] = iv;

        env(jv, HSFEE, ter(tesSUCCESS));
        env.close();
    }

    void
    testHooksDisabled(FeatureBitset features)
    {
        testcase("Check for disabled amendment");
        using namespace jtx;
        Env env{*this, features - featureHooks};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        env(ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS, overrideFlag)}}, 0),
            M("Hooks Disabled"),
            HSFEE,
            ter(temDISABLED));
    }

    void
    testTxStructure(FeatureBitset features)
    {
        testcase("Checks malformed transactions");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Test outer structure

        env(ripple::test::jtx::hook(alice, {}, 0),
            M("Must have a hooks field"),
            HSFEE,
            ter(temMALFORMED));

        env(ripple::test::jtx::hook(alice, {{}}, 0),
            M("Must have a non-empty hooks field"),
            HSFEE,
            ter(temMALFORMED));

        env(ripple::test::jtx::hook(
                alice,
                {{hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS),
                  hsov1(accept_wasm, 1, HSDROPS)}},
                0),
            M("Must have fewer than 11 entries"),
            HSFEE,
            ter(temMALFORMED));

        {
            Json::Value jv;
            jv[jss::Account] = alice.human();
            jv[jss::TransactionType] = jss::SetHook;
            jv[jss::Flags] = 0;
            jv[jss::Hooks] = Json::Value{Json::arrayValue};

            Json::Value iv;
            iv[jss::MemoData] = "DEADBEEF";
            iv[jss::MemoFormat] = "";
            iv[jss::MemoType] = "";
            jv[jss::Hooks][0U][jss::Memo] = iv;
            env(jv,
                M("Hooks Array must contain Hook objects"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }
    }

    void
    testInstall(FeatureBitset features)
    {
        testcase("Checks malformed install operation");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        auto const bob = Account{"bob"};
        env.fund(XRP(10000), bob);

        // create a hook that we can then install
        {
            env(ripple::test::jtx::hook(
                    bob,
                    {{hsov1(accept_wasm, 1, HSDROPS),
                      hsov1(rollback_wasm, 1, HSDROPS)}},
                    0),
                M("First set = tesSUCCESS"),
                HSFEE,
                ter(tesSUCCESS));
        }

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        // can't set api version
        {
            Json::Value iv;
            iv[jss::HookHash] = accept_hash_str;
            iv[jss::HookApiVersion] = 0U;
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook Install operation cannot set apiversion"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // can't set non-existent hook
        {
            Json::Value iv;
            iv[jss::HookHash] =
                "DEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBEEFDEADBE"
                "EF";
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook Install operation cannot set non existent hook hash"),
                HSFEE,
                ter(terNO_HOOK));
            env.close();
        }

        // can set extant hook
        {
            Json::Value iv;
            iv[jss::HookHash] = accept_hash_str;
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook Install operation can set extant hook hash"),
                HSFEE,
                ter(tesSUCCESS));
            env.close();
        }

        // can't set extant hook over other hook without override flag
        {
            Json::Value iv;
            iv[jss::HookHash] = rollback_hash_str;
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook Install operation can set extant hook hash"),
                HSFEE,
                ter(tecREQUIRES_FLAG));
            env.close();
        }

        // can set extant hook over other hook with override flag
        {
            Json::Value iv;
            iv[jss::HookHash] = rollback_hash_str;
            iv[jss::Flags] = hsfOVERRIDE;
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook Install operation can set extant hook hash"),
                HSFEE,
                ter(tesSUCCESS));
            env.close();
        }
    }

    void
    testDelete(FeatureBitset features)
    {
        testcase("Checks malformed delete operation");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        // flag required
        {
            Json::Value iv;
            iv[jss::CreateCode] = "";
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook DELETE operation must include hsfOVERRIDE flag"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // invalid flags
        {
            Json::Value iv;
            iv[jss::CreateCode] = "";
            iv[jss::Flags] = "2147483648";
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook DELETE operation must include hsfOVERRIDE flag"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // grants, parameters, hookon, hookapiversion, hooknamespace keys must
        // be absent
        for (auto const& [key, value] : JSSMap{
                 {jss::HookGrants, Json::arrayValue},
                 {jss::HookParameters, Json::arrayValue},
                 {jss::HookOn,
                  "000000000000000000000000000000000000000000000000000000000000"
                  "0000"},
                 {jss::HookApiVersion, "1"},
                 {jss::HookNamespace, to_string(uint256{beast::zero})}})
        {
            Json::Value iv;
            iv[jss::CreateCode] = "";
            iv[key] = value;
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook DELETE operation cannot include: grants, params, "
                  "hookon, apiversion, namespace"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // create and delete single hook
        {
            {
                Json::Value jv = ripple::test::jtx::hook(
                    alice, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0);
                env(jv, M("Normal accept create"), HSFEE, ter(tesSUCCESS));
                env.close();
            }

            BEAST_REQUIRE(env.le(accept_keylet));

            Json::Value iv;
            iv[jss::CreateCode] = "";
            iv[jss::Flags] = hsfOVERRIDE;
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv, M("Normal hook DELETE"), HSFEE);
            env.close();

            // check to ensure definition is deleted and hooks object too
            auto const def = env.le(accept_keylet);
            auto const hook = env.le(keylet::hook(Account("alice").id()));

            BEAST_EXPECT(!def);
            BEAST_EXPECT(!hook);
        }

        // create four hooks then delete the second last one
        {
            // create
            {
                Json::Value jv = ripple::test::jtx::hook(
                    alice,
                    {{hsov1(accept_wasm, 1, HSDROPS),
                      hsov1(makestate_wasm, 1, HSDROPS),
                      hsov1(rollback_wasm, 1, HSDROPS),
                      hsov1(accept2_wasm, 1, HSDROPS)}},
                    0);
                env(jv, M("Create four"), HSFEE, ter(tesSUCCESS));
                env.close();
            }

            // delete third and check
            {
                Json::Value iv;
                iv[jss::CreateCode] = "";
                iv[jss::Flags] = hsfOVERRIDE;
                for (uint8_t i = 0; i < 4; ++i)
                    jv[jss::Hooks][i][jss::Hook] = Json::Value{};
                jv[jss::Hooks][2U][jss::Hook] = iv;

                env(jv, M("Normal hooki DELETE (third pos)"), HSFEE);
                env.close();

                // check the hook definitions are consistent with reference
                // count dropping to zero on the third
                auto const accept_def = env.le(accept_keylet);
                auto const rollback_def = env.le(rollback_keylet);
                auto const makestate_def = env.le(makestate_keylet);
                auto const accept2_def = env.le(accept2_keylet);

                BEAST_REQUIRE(accept_def);
                BEAST_EXPECT(!rollback_def);
                BEAST_REQUIRE(makestate_def);
                BEAST_REQUIRE(accept2_def);

                // check the hooks array is correct
                auto const hook = env.le(keylet::hook(Account("alice").id()));
                BEAST_REQUIRE(hook);

                auto const& hooks = hook->getFieldArray(sfHooks);
                BEAST_REQUIRE(hooks.size() == 4);

                // make sure only the third is deleted
                BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookHash));
                BEAST_REQUIRE(hooks[1].isFieldPresent(sfHookHash));
                BEAST_EXPECT(!hooks[2].isFieldPresent(sfHookHash));
                BEAST_REQUIRE(hooks[3].isFieldPresent(sfHookHash));

                // check hashes on the three remaining
                BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);
                BEAST_EXPECT(
                    hooks[1].getFieldH256(sfHookHash) == makestate_hash);
                BEAST_EXPECT(hooks[3].getFieldH256(sfHookHash) == accept2_hash);
            }

            // delete rest and check
            {
                Json::Value iv;
                iv[jss::CreateCode] = "";
                iv[jss::Flags] = hsfOVERRIDE;
                for (uint8_t i = 0; i < 4; ++i)
                {
                    if (i != 2U)
                        jv[jss::Hooks][i][jss::Hook] = iv;
                    else
                        jv[jss::Hooks][i][jss::Hook] = Json::Value{};
                }

                env(jv,
                    M("Normal hook DELETE (first, second, fourth pos)"),
                    HSFEE);
                env.close();

                // check the hook definitions are consistent with reference
                // count dropping to zero on the third
                auto const accept_def = env.le(accept_keylet);
                auto const rollback_def = env.le(rollback_keylet);
                auto const makestate_def = env.le(makestate_keylet);
                auto const accept2_def = env.le(accept2_keylet);

                BEAST_EXPECT(!accept_def);
                BEAST_EXPECT(!rollback_def);
                BEAST_EXPECT(!makestate_def);
                BEAST_EXPECT(!accept2_def);

                // check the hooks object is gone
                auto const hook = env.le(keylet::hook(Account("alice").id()));
                BEAST_EXPECT(!hook);
            }
        }
    }

    void
    testNSDelete(FeatureBitset features)
    {
        testcase("Checks malformed nsdelete operation");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        auto const bob = Account{"bob"};
        env.fund(XRP(10000), bob);

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        for (auto const& [key, value] : JSSMap{
                 {jss::HookGrants, Json::arrayValue},
                 {jss::HookParameters, Json::arrayValue},
                 {jss::HookOn,
                  "000000000000000000000000000000000000000000000000000000000000"
                  "0000"},
                 {jss::HookApiVersion, "1"},
             })
        {
            Json::Value iv;
            iv[key] = value;
            iv[jss::Flags] = hsfNSDELETE;
            iv[jss::HookNamespace] = to_string(uint256{beast::zero});
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Hook NSDELETE operation cannot include: grants, params, "
                  "hookon, apiversion"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        auto const key = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
                 0x00U, 0x00U, 0x00U, 0x00U, 'k',   'e',   'y',   0x00U})
                .data());

        auto const ns = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU})
                .data());

        auto const stateKeylet =
            keylet::hookState(Account("alice").id(), key, ns);

        // create a namespace
        std::string ns_str =
            "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFE";
        {
            // create hook
            Json::Value jv = ripple::test::jtx::hook(
                alice, {{hsov1(makestate_wasm, 1, HSDROPS)}}, 0);

            jv[jss::Hooks][0U][jss::Hook][jss::HookNamespace] = ns_str;
            env(jv, M("Create makestate hook"), HSFEE, ter(tesSUCCESS));
            env.close();

            // run hook
            env(pay(bob, alice, XRP(1)),
                M("Run create state hook"),
                fee(XRP(1)));
            env.close();

            // check if the hookstate object was created
            auto const hookstate = env.le(stateKeylet);
            BEAST_EXPECT(!!hookstate);

            // check if the value was set correctly
            auto const& data = hookstate->getFieldVL(sfHookStateData);

            BEAST_REQUIRE(data.size() == 6);
            BEAST_EXPECT(
                data[0] == 'v' && data[1] == 'a' && data[2] == 'l' &&
                data[3] == 'u' && data[4] == 'e' && data[5] == '\0');
        }

        // delete the namespace
        {
            Json::Value iv;
            iv[jss::Flags] = hsfNSDELETE;
            iv[jss::HookNamespace] = ns_str;
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv, M("Normal NSDELETE operation"), HSFEE, ter(tesSUCCESS));
            env.close();

            // ensure the hook is still installed
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);

            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() > 0);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == makestate_hash);

            // ensure the directory is gone
            auto const dirKeylet =
                keylet::hookStateDir(Account("alice").id(), ns);
            BEAST_EXPECT(!env.le(dirKeylet));

            // ensure the state object is gone
            BEAST_EXPECT(!env.le(stateKeylet));
        }
    }

    void
    testCreate(FeatureBitset features)
    {
        testcase("Checks malformed create operation");
        using namespace jtx;
        Env env{*this, features};

        auto const bob = Account{"bob"};
        env.fund(XRP(10000), bob);

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        // test normal create and missing override flag
        {
            env(ripple::test::jtx::hook(
                    bob, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0),
                M("First set = tesSUCCESS"),
                HSFEE,
                ter(tesSUCCESS));

            env(ripple::test::jtx::hook(
                    bob, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0),
                M("Second set = tecREQUIRES_FLAG"),
                HSFEE,
                ter(tecREQUIRES_FLAG));
            env.close();
        }

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        // DA TODO: FAILING
        // // payload too large
        // {
        //     env(ripple::test::jtx::hook(alice, {{hsov1(long_wasm, 1)}}, 0),
        //         M("If CreateCode is present, then it must be less than
        //         64kib"), HSFEE, ter(temMALFORMED));
        //     env.close();
        // }

        // namespace missing
        {
            Json::Value iv;
            iv[jss::CreateCode] = strHex(accept_wasm);
            iv[jss::HookApiVersion] = 1U;
            iv[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "00";
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("HSO Create operation must contain namespace"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // api version missing
        {
            Json::Value iv;
            iv[jss::CreateCode] = strHex(accept_wasm);
            iv[jss::HookNamespace] = to_string(uint256{beast::zero});
            iv[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "00";
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("HSO Create operation must contain api version"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // api version wrong
        {
            Json::Value iv;
            iv[jss::CreateCode] = strHex(accept_wasm);
            iv[jss::HookNamespace] = to_string(uint256{beast::zero});
            iv[jss::HookApiVersion] = 2U;
            iv[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "00";
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("HSO Create operation must contain valid api version"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // hookon missing
        {
            Json::Value iv;
            iv[jss::CreateCode] = strHex(accept_wasm);
            iv[jss::HookNamespace] = to_string(uint256{beast::zero});
            iv[jss::HookApiVersion] = 1U;
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("HSO Create operation must contain hookon"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // hook hash present
        {
            Json::Value jv = ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0);
            Json::Value iv = jv[jss::Hooks][0U];
            iv[jss::Hook][jss::HookHash] = to_string(uint256{beast::zero});
            jv[jss::Hooks][0U] = iv;
            env(jv,
                M("Cannot have both CreateCode and HookHash"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // correctly formed
        {
            Json::Value jv = ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0);
            env(jv, M("Normal accept"), HSFEE, ter(tesSUCCESS));
            env.close();

            auto const def = env.le(accept_keylet);
            auto const hook = env.le(keylet::hook(Account("alice").id()));

            // check if the hook definition exists
            BEAST_EXPECT(!!def);

            // check if the user account has a hooks object
            BEAST_EXPECT(!!hook);

            // check if the hook is correctly set at position 1
            BEAST_EXPECT(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() > 0);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check if the wasm binary was correctly set
            BEAST_EXPECT(def->isFieldPresent(sfCreateCode));
            auto const& wasm = def->getFieldVL(sfCreateCode);
            auto const wasm_hash =
                sha512Half_s(ripple::Slice(wasm.data(), wasm.size()));
            BEAST_EXPECT(wasm_hash == accept_hash);
        }

        // add a second hook
        {
            Json::Value jv = ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0);
            Json::Value iv = jv[jss::Hooks][0U];
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = Json::Value{};
            jv[jss::Hooks][1U] = iv;
            env(jv,
                M("Normal accept, second position"),
                HSFEE,
                ter(tesSUCCESS));
            env.close();

            auto const def = env.le(accept_keylet);
            auto const hook = env.le(keylet::hook(Account("alice").id()));

            // check if the hook definition exists
            BEAST_EXPECT(!!def);

            // check if the user account has a hooks object
            BEAST_EXPECT(!!hook);

            // check if the hook is correctly set at position 2
            BEAST_EXPECT(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() > 1);
            BEAST_EXPECT(hooks[1].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[1].getFieldH256(sfHookHash) == accept_hash);

            // check if the reference count was correctly incremented
            BEAST_EXPECT(def->isFieldPresent(sfReferenceCount));
            // two references from alice, one from bob (first test above)
            BEAST_EXPECT(def->getFieldU64(sfReferenceCount) == 3ULL);
        }

        auto const rollback_hash = ripple::sha512Half_s(
            ripple::Slice(rollback_wasm.data(), rollback_wasm.size()));

        // test override
        {
            Json::Value jv = ripple::test::jtx::hook(
                alice, {{hsov1(rollback_wasm, 1, HSDROPS)}}, 0);
            jv[jss::Hooks][0U][jss::Hook][jss::Flags] = hsfOVERRIDE;
            env(jv, M("Rollback override"), HSFEE, ter(tesSUCCESS));
            env.close();

            auto const rollback_def = env.le(rollback_keylet);
            auto const accept_def = env.le(accept_keylet);
            auto const hook = env.le(keylet::hook(Account("alice").id()));

            // check if the hook definition exists
            BEAST_EXPECT(rollback_def);
            BEAST_EXPECT(accept_def);

            // check if the user account has a hooks object
            BEAST_EXPECT(hook);

            // check if the hook is correctly set at position 1
            BEAST_EXPECT(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() > 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == rollback_hash);
            BEAST_EXPECT(hooks[1].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[1].getFieldH256(sfHookHash) == accept_hash);

            // check if the wasm binary was correctly set
            BEAST_EXPECT(rollback_def->isFieldPresent(sfCreateCode));
            auto const& wasm = rollback_def->getFieldVL(sfCreateCode);
            auto const wasm_hash =
                sha512Half_s(ripple::Slice(wasm.data(), wasm.size()));
            BEAST_EXPECT(wasm_hash == rollback_hash);

            // check if the reference count was correctly incremented
            BEAST_EXPECT(rollback_def->isFieldPresent(sfReferenceCount));
            BEAST_EXPECT(rollback_def->getFieldU64(sfReferenceCount) == 1ULL);

            // check if the reference count was correctly decremented
            BEAST_EXPECT(accept_def->isFieldPresent(sfReferenceCount));
            BEAST_EXPECT(accept_def->getFieldU64(sfReferenceCount) == 2ULL);
        }
    }

    void
    testUpdate(FeatureBitset features)
    {
        testcase("Checks malformed update operation");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        auto const bob = Account{"bob"};
        env.fund(XRP(10000), bob);

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        // first create the hook
        {
            Json::Value iv;
            iv[jss::CreateCode] = strHex(accept_wasm);
            iv[jss::HookNamespace] = to_string(uint256{beast::zero});
            iv[jss::HookApiVersion] = 1U;
            iv[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "00";
            iv[jss::Fee] = "100";
            iv[jss::HookParameters] = Json::Value{Json::arrayValue};
            iv[jss::HookParameters][0U] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterName] = "AAAAAAAAAAAA";
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterValue] = "BBBBBB";

            iv[jss::HookParameters][1U] = Json::Value{};
            iv[jss::HookParameters][1U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][1U][jss::HookParameter]
              [jss::HookParameterName] = "CAFE";
            iv[jss::HookParameters][1U][jss::HookParameter]
              [jss::HookParameterValue] = "FACADE";

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv, M("Create accept"), HSFEE, ter(tesSUCCESS));
            env.close();
        }

        // all alice operations below are then updates

        // must not specify override flag
        {
            Json::Value iv;
            iv[jss::Flags] = hsfOVERRIDE;
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("Override flag not allowed on update"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // must not specify NSDELETE unless also Namespace
        {
            Json::Value iv;
            iv[jss::Flags] = hsfNSDELETE;
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("NSDELETE flag not allowed on update unless HookNamespace "
                  "also present"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // api version not allowed in update
        {
            Json::Value iv;
            iv[jss::HookApiVersion] = 1U;
            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;

            env(jv,
                M("ApiVersion not allowed in update"),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // try individually updating the various allowed fields
        {
            Json::Value params{Json::arrayValue};
            params[0U][jss::HookParameter] = Json::Value{};
            params[0U][jss::HookParameter][jss::HookParameterName] = "CAFE";
            params[0U][jss::HookParameter][jss::HookParameterValue] = "BABE";

            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = accept_hash_str;

            for (auto const& [key, value] : JSSMap{
                     {jss::HookOn,
                      "00000000000000000000000000000000000000000000000000000000"
                      "00000001"},
                     {jss::HookNamespace,
                      "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFE"
                      "CAFECAFE"},
                     {jss::HookParameters, params},
                     {jss::HookGrants, grants}})
            {
                Json::Value iv;
                iv[key] = value;
                jv[jss::Hooks][0U] = Json::Value{};
                jv[jss::Hooks][0U][jss::Hook] = iv;

                env(jv, M("Normal update"), HSFEE, ter(tesSUCCESS));
                env.close();
            }

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check all fields were updated to correct values
            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookOn));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookOn) == UINT256_BIT[0]);

            auto const ns = uint256::fromVoid(
                (std::array<uint8_t, 32>{
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU})
                    .data());
            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookNamespace));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookNamespace) == ns);

            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookParameters));
            const auto& p = hooks[0].getFieldArray(sfHookParameters);
            BEAST_REQUIRE(p.size() == 1);
            BEAST_REQUIRE(p[0].isFieldPresent(sfHookParameterName));
            BEAST_REQUIRE(p[0].isFieldPresent(sfHookParameterValue));

            const auto pn = p[0].getFieldVL(sfHookParameterName);
            BEAST_REQUIRE(pn.size() == 2);
            BEAST_REQUIRE(pn[0] == 0xCAU && pn[1] == 0xFEU);

            const auto pv = p[0].getFieldVL(sfHookParameterValue);
            BEAST_REQUIRE(pv.size() == 2);
            BEAST_REQUIRE(pv[0] == 0xBAU && pv[1] == 0xBEU);

            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookGrants));
            const auto& g = hooks[0].getFieldArray(sfHookGrants);
            BEAST_REQUIRE(g.size() == 1);
            BEAST_REQUIRE(g[0].isFieldPresent(sfHookHash));
            BEAST_REQUIRE(g[0].getFieldH256(sfHookHash) == accept_hash);
        }

        // reset hookon and namespace to defaults
        {
            for (auto const& [key, value] : JSSMap{
                     {jss::HookOn,
                      "00000000000000000000000000000000000000000000000000000000"
                      "00000000"},
                     {jss::HookNamespace, to_string(uint256{beast::zero})}})
            {
                Json::Value iv;
                iv[key] = value;
                jv[jss::Hooks][0U] = Json::Value{};
                jv[jss::Hooks][0U][jss::Hook] = iv;

                env(jv, M("Reset to default"), HSFEE, ter(tesSUCCESS));
                env.close();
            }

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // ensure the two fields are now absent (because they were reset to
            // the defaults on the hook def)
            BEAST_EXPECT(!hooks[0].isFieldPresent(sfHookOn));
            BEAST_EXPECT(!hooks[0].isFieldPresent(sfHookNamespace));
        }

        // add three additional parameters
        std::map<ripple::Blob, ripple::Blob> params{
            {{0xFEU, 0xEDU, 0xFAU, 0xCEU}, {0xF0U, 0x0DU}},
            {{0xA0U}, {0xB0U}},
            {{0xCAU, 0xFEU}, {0xBAU, 0xBEU}},
            {{0xAAU}, {0xBBU, 0xCCU}}};
        {
            Json::Value iv;
            iv[jss::HookParameters] = Json::Value{Json::arrayValue};
            iv[jss::HookParameters][0U] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterName] = "FEEDFACE";
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterValue] = "F00D";

            iv[jss::HookParameters][1U] = Json::Value{};
            iv[jss::HookParameters][1U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][1U][jss::HookParameter]
              [jss::HookParameterName] = "A0";
            iv[jss::HookParameters][1U][jss::HookParameter]
              [jss::HookParameterValue] = "B0";

            iv[jss::HookParameters][2U] = Json::Value{};
            iv[jss::HookParameters][2U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][2U][jss::HookParameter]
              [jss::HookParameterName] = "AA";
            iv[jss::HookParameters][2U][jss::HookParameter]
              [jss::HookParameterValue] = "BBCC";

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv, M("Add three parameters"), HSFEE, ter(tesSUCCESS));
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check all the previous parameters plus the new ones
            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookParameters));
            const auto& p = hooks[0].getFieldArray(sfHookParameters);
            BEAST_REQUIRE(p.size() == params.size());

            std::set<ripple::Blob> already;

            for (uint8_t i = 0; i < params.size(); ++i)
            {
                const auto pn = p[i].getFieldVL(sfHookParameterName);
                const auto pv = p[i].getFieldVL(sfHookParameterValue);

                // make sure it's not a duplicate entry
                BEAST_EXPECT(already.find(pn) == already.end());

                // make  sure it exists
                BEAST_EXPECT(params.find(pn) != params.end());

                // make sure the value matches
                BEAST_EXPECT(params[pn] == pv);
                already.emplace(pn);
            }
        }

        // try to reset CAFE parameter to default
        {
            Json::Value iv;
            iv[jss::HookParameters] = Json::Value{Json::arrayValue};
            iv[jss::HookParameters][0U] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterName] = "CAFE";

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Reset cafe param to default using Absent Value"),
                HSFEE,
                ter(tesSUCCESS));
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            params.erase({0xCAU, 0xFEU});

            // check there right number of parameters exist
            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookParameters));
            const auto& p = hooks[0].getFieldArray(sfHookParameters);
            BEAST_REQUIRE(p.size() == params.size());

            // and that they still have the expected values and that there are
            // no duplicates
            std::set<ripple::Blob> already;
            for (uint8_t i = 0; i < params.size(); ++i)
            {
                const auto pn = p[i].getFieldVL(sfHookParameterName);
                const auto pv = p[i].getFieldVL(sfHookParameterValue);

                // make sure it's not a duplicate entry
                BEAST_EXPECT(already.find(pn) == already.end());

                // make  sure it exists
                BEAST_EXPECT(params.find(pn) != params.end());

                // make sure the value matches
                BEAST_EXPECT(params[pn] == pv);
                already.emplace(pn);
            }
        }

        // now re-add CAFE parameter but this time as an explicit blank (Empty
        // value)
        {
            Json::Value iv;
            iv[jss::HookParameters] = Json::Value{Json::arrayValue};
            iv[jss::HookParameters][0U] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterName] = "CAFE";
            iv[jss::HookParameters][0U][jss::HookParameter]
              [jss::HookParameterValue] = "";

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv,
                M("Set cafe param to blank using Empty Value"),
                HSFEE,
                ter(tesSUCCESS));
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            params[Blob{0xCAU, 0xFEU}] = Blob{};

            // check there right number of parameters exist
            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookParameters));
            const auto& p = hooks[0].getFieldArray(sfHookParameters);
            BEAST_REQUIRE(p.size() == params.size());

            // and that they still have the expected values and that there are
            // no duplicates
            std::set<ripple::Blob> already;
            for (uint8_t i = 0; i < params.size(); ++i)
            {
                const auto pn = p[i].getFieldVL(sfHookParameterName);
                const auto pv = p[i].getFieldVL(sfHookParameterValue);

                // make sure it's not a duplicate entry
                BEAST_EXPECT(already.find(pn) == already.end());

                // make  sure it exists
                BEAST_EXPECT(params.find(pn) != params.end());

                // make sure the value matches
                BEAST_EXPECT(params[pn] == pv);
                already.emplace(pn);
            }
        }

        // try to delete all parameters (reset to defaults) using EMA (Empty
        // Parameters Array)
        {
            Json::Value iv;
            iv[jss::HookParameters] = Json::Value{Json::arrayValue};

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = iv;
            env(jv, M("Unset all params on hook"), HSFEE, ter(tesSUCCESS));
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check there right number of parameters exist
            BEAST_REQUIRE(!hooks[0].isFieldPresent(sfHookParameters));
        }

        // try to set each type of field on a non existent hook
        {
            Json::Value params{Json::arrayValue};
            params[0U][jss::HookParameter] = Json::Value{};
            params[0U][jss::HookParameter][jss::HookParameterName] = "CAFE";
            params[0U][jss::HookParameter][jss::HookParameterValue] = "BABE";

            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = accept_hash_str;

            for (auto const& [key, value] : JSSMap{
                     {jss::HookOn,
                      "00000000000000000000000000000000000000000000000000000000"
                      "00000001"},
                     {jss::HookNamespace,
                      "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFE"
                      "CAFECAFE"},
                     {jss::HookParameters, params},
                     {jss::HookGrants, grants}})
            {
                Json::Value iv;
                iv[key] = value;
                jv[jss::Hooks][0U] = Json::Value{};
                jv[jss::Hooks][0U][jss::Hook] = Json::Value{};
                jv[jss::Hooks][1U] = Json::Value{};
                jv[jss::Hooks][1U][jss::Hook] = iv;

                env(jv,
                    M("Invalid update on non existent hook"),
                    HSFEE,
                    ter(tecNO_ENTRY));
                env.close();
            }

            // ensure hook still exists and that there was no created new entry
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);
            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 1);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);
        }

        // test adding multiple grants
        {
            {
                // add a second hook
                env(ripple::test::jtx::hook(
                        alice, {{{}, hsov1(accept_wasm, 1, HSDROPS)}}, 0),
                    M("Add second hook"),
                    HSFEE,
                    ter(tesSUCCESS));
            }

            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = rollback_hash_str;
            grants[0U][jss::HookGrant][jss::Authorize] = bob.human();

            grants[1U][jss::HookGrant] = Json::Value{};
            grants[1U][jss::HookGrant][jss::HookHash] = accept_hash_str;

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = Json::objectValue;
            jv[jss::Hooks][1U] = Json::Value{};
            jv[jss::Hooks][1U][jss::Hook] = Json::Value{};
            jv[jss::Hooks][1U][jss::Hook][jss::HookGrants] = grants;

            env(jv, M("Add grants"), HSFEE);
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);

            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 2);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check there right number of grants exist
            // hook 0 should have 1 grant
            BEAST_REQUIRE(hooks[0].isFieldPresent(sfHookGrants));
            BEAST_REQUIRE(hooks[0].getFieldArray(sfHookGrants).size() == 1);
            // hook 1 should have 2 grants
            {
                BEAST_REQUIRE(hooks[1].isFieldPresent(sfHookGrants));
                auto const& grants = hooks[1].getFieldArray(sfHookGrants);
                BEAST_REQUIRE(grants.size() == 2);

                BEAST_REQUIRE(grants[0].isFieldPresent(sfHookHash));
                BEAST_REQUIRE(grants[0].isFieldPresent(sfAuthorize));
                BEAST_REQUIRE(grants[1].isFieldPresent(sfHookHash));
                BEAST_EXPECT(!grants[1].isFieldPresent(sfAuthorize));

                BEAST_EXPECT(
                    grants[0].getFieldH256(sfHookHash) == rollback_hash);
                BEAST_EXPECT(grants[0].getAccountID(sfAuthorize) == bob.id());

                BEAST_EXPECT(grants[1].getFieldH256(sfHookHash) == accept_hash);
            }
        }

        // update grants
        {
            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = makestate_hash_str;

            jv[jss::Hooks][0U] = Json::Value{};
            jv[jss::Hooks][0U][jss::Hook] = Json::objectValue;
            jv[jss::Hooks][1U] = Json::Value{};
            jv[jss::Hooks][1U][jss::Hook] = Json::Value{};
            jv[jss::Hooks][1U][jss::Hook][jss::HookGrants] = grants;

            env(jv, M("update grants"), HSFEE);
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);

            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 2);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check there right number of grants exist
            // hook 1 should have 1 grant
            {
                BEAST_REQUIRE(hooks[1].isFieldPresent(sfHookGrants));
                auto const& grants = hooks[1].getFieldArray(sfHookGrants);
                BEAST_REQUIRE(grants.size() == 1);
                BEAST_REQUIRE(grants[0].isFieldPresent(sfHookHash));
                BEAST_EXPECT(
                    grants[0].getFieldH256(sfHookHash) == makestate_hash);
            }
        }

        // use an empty grants array to reset the grants
        {
            jv[jss::Hooks][0U] = Json::objectValue;
            jv[jss::Hooks][0U][jss::Hook] = Json::objectValue;
            jv[jss::Hooks][1U] = Json::Value{};
            jv[jss::Hooks][1U][jss::Hook] = Json::Value{};
            jv[jss::Hooks][1U][jss::Hook][jss::HookGrants] = Json::arrayValue;

            env(jv, M("clear grants"), HSFEE);
            env.close();

            // ensure hook still exists
            auto const hook = env.le(keylet::hook(Account("alice").id()));
            BEAST_REQUIRE(hook);

            BEAST_REQUIRE(hook->isFieldPresent(sfHooks));
            auto const& hooks = hook->getFieldArray(sfHooks);
            BEAST_EXPECT(hooks.size() == 2);
            BEAST_EXPECT(hooks[0].isFieldPresent(sfHookHash));
            BEAST_EXPECT(hooks[0].getFieldH256(sfHookHash) == accept_hash);

            // check there right number of grants exist
            // hook 1 should have 0 grants
            BEAST_REQUIRE(!hooks[1].isFieldPresent(sfHookGrants));
        }
    }

    void
    testWithTickets(FeatureBitset features)
    {
        testcase("with tickets");
        using namespace jtx;

        Env env{*this, features};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);

        std::uint32_t aliceTicketSeq{env.seq(alice) + 1};
        env(ticket::create(alice, 10));
        std::uint32_t const aliceSeq{env.seq(alice)};
        env.require(owners(alice, 10));

        env(ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0),
            HSFEE,
            ticket::use(aliceTicketSeq++),
            ter(tesSUCCESS));

        env.require(tickets(alice, env.seq(alice) - aliceTicketSeq));
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        env.require(owners(alice, 9 + 1));
    }

    // void
    // testWasm(FeatureBitset features)
    // {
    //     testcase("Checks malformed hook binaries");
    //     using namespace jtx;
    //     Env env{*this, features};

    //     auto const alice = Account{"alice"};
    //     env.fund(XRP(10000), alice);

    //     env(ripple::test::jtx::hook(alice, {{hsov1(illegalfunc_wasm, 1)}},
    //     0),
    //         M("Must only contain hook and cbak"),
    //         HSFEE,
    //         ter(temMALFORMED));
    // }

    void
    test_accept(FeatureBitset features)
    {
        testcase("Test accept() hookapi");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        env(ripple::test::jtx::hook(
                alice, {{hsov1(accept_wasm, 1, HSDROPS)}}, 0),
            M("Install Accept Hook"),
            HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)), M("Test Accept Hook"), fee(XRP(1)));
        env.close();
    }

    void
    test_rollback(FeatureBitset features)
    {
        testcase("Test rollback() hookapi");
        using namespace jtx;
        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        env(ripple::test::jtx::hook(
                alice, {{hsov1(rollback_wasm, 1, HSDROPS)}}, 0),
            M("Install Rollback Hook"),
            HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)),
            M("Test Rollback Hook"),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));
        env.close();
    }

    void
    test_float_compare(FeatureBitset features)
    {
        testcase("Test float_compare");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var INVALID_FLOAT = -10024
                var EQ = 1
                var LT = 2
                var GT = 4
                var LTE = 3
                var GTE = 5
                var NEQ = 6
                var Hook = (arg) => {
                {
                    ASSERT(float_compare(-1, -2, EQ) == INVALID_FLOAT)
                    ASSERT(float_compare(0, -2, EQ) == INVALID_FLOAT)
                    ASSERT(float_compare(-1, 0, EQ) == INVALID_FLOAT)
                }
                {
                    ASSERT(float_compare(0, 0, 8) == INVALID_ARGUMENT)
                    ASSERT(float_compare(0, 0, 16) == INVALID_ARGUMENT)
                    ASSERT(float_compare(0, 0, ~7) == INVALID_ARGUMENT)
                    ASSERT(float_compare(0, 0, 7) == INVALID_ARGUMENT)
                    ASSERT(float_compare(0, 0, 0) == INVALID_ARGUMENT)
                }
                {
                    ASSERT(float_compare(0, 0, EQ))
                    ASSERT(float_compare(0, float_one(), LT))
                    ASSERT(float_compare(0, float_one(), GT) == 0)
                    ASSERT(float_compare(0, float_one(), GTE) == 0)
                    ASSERT(float_compare(0, float_one(), LTE))
                    ASSERT(float_compare(0, float_one(), NEQ))
                    const large_negative = 1622844335003378700
                    const small_negative = 1352229899321149e3
                    const small_positive = 5713898440837103e3
                    const large_positive = 7749425685711506e3
                    ASSERT(float_compare(large_negative, small_negative, LT))
                    ASSERT(float_compare(large_negative, small_negative, LTE))
                    ASSERT(float_compare(large_negative, small_negative, NEQ))
                    ASSERT(float_compare(large_negative, small_negative, GT) == 0)
                    ASSERT(float_compare(large_negative, small_negative, GTE) == 0)
                    ASSERT(float_compare(large_negative, small_negative, EQ) == 0)
                    ASSERT(float_compare(large_negative, large_positive, LT))
                    ASSERT(float_compare(large_negative, large_positive, LTE))
                    ASSERT(float_compare(large_negative, large_positive, NEQ))
                    ASSERT(float_compare(large_negative, large_positive, GT) == 0)
                    ASSERT(float_compare(large_negative, large_positive, GTE) == 0)
                    ASSERT(float_compare(large_negative, large_positive, EQ) == 0)
                    ASSERT(float_compare(small_negative, small_positive, LT))
                    ASSERT(float_compare(small_negative, small_positive, LTE))
                    ASSERT(float_compare(small_negative, small_positive, NEQ))
                    ASSERT(float_compare(small_negative, small_positive, GT) == 0)
                    ASSERT(float_compare(small_negative, small_positive, GTE) == 0)
                    ASSERT(float_compare(small_negative, small_positive, EQ) == 0)
                    ASSERT(float_compare(small_positive, large_positive, LT))
                    ASSERT(float_compare(small_positive, large_positive, LTE))
                    ASSERT(float_compare(small_positive, large_positive, NEQ))
                    ASSERT(float_compare(small_positive, large_positive, GT) == 0)
                    ASSERT(float_compare(small_positive, large_positive, GTE) == 0)
                    ASSERT(float_compare(small_positive, large_positive, EQ) == 0)
                    ASSERT(float_compare(small_negative, 0, LT))
                    ASSERT(float_compare(large_negative, 0, LT))
                    ASSERT(float_compare(small_positive, 0, GT))
                    ASSERT(float_compare(large_positive, 0, GT))
                }
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_compare"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_compare"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_divide(FeatureBitset features)
    {
        testcase("Test float_divide");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                function frexp(value) {
                if (value === BigInt(0)) return [0, 0]
                const data = new DataView(new ArrayBuffer(8))
                data.setFloat64(0, Number(value))
                let bits = (data.getUint32(0) >>> 20) & 2047
                if (bits === 0) {
                    data.setFloat64(0, Number(value) * Math.pow(2, 64))
                    bits = ((data.getUint32(0) >>> 20) & 2047) - 64
                }
                const exponent = bits - 1022
                const mantissa = Number(value) / Math.pow(2, exponent)
                return [mantissa, exponent]
                }
                function floatMantissa(x) {
                if (x === BigInt(0)) return 0
                const [mantissa, exponent] = frexp(x)
                return mantissa * Math.pow(2, 52)
                }
                function floatExponent(x) {
                if (x === BigInt(0)) return 0
                const [mantissa, exponent] = frexp(x)
                return exponent
                }
                function ASSERT_EQUAL(x, y) {
                const px = x
                const py = y
                let mx = floatMantissa(px)
                let my = floatMantissa(py)
                const diffexp = floatExponent(px) - floatExponent(py)
                if (diffexp === 1) mx *= 10
                if (diffexp === -1) my *= 10
                let diffman = mx - my
                if (diffman < 0) diffman *= -1
                if (Math.abs(diffexp) > 1 || diffman > 5e6 || mx < 0 || my < 0) {
                    rollback('x', 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var DIVISION_BY_ZERO = -25
                var XFL_OVERFLOW = -30
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_divide(void 0, float_one()) === INVALID_ARGUMENT)
                ASSERT(float_divide(float_one(), void 0) === INVALID_ARGUMENT)
                ASSERT(float_divide(-1, float_one()) === INVALID_FLOAT)
                ASSERT(float_divide(float_one(), 0) === DIVISION_BY_ZERO)
                ASSERT(float_divide(0, float_one()) === 0n)
                ASSERT(float_divide(float_one(), float_one()) === float_one())
                ASSERT(
                    float_divide(float_one(), float_negate(float_one())) ===
                    float_negate(float_one())
                )
                ASSERT(
                    float_divide(float_negate(float_one()), float_one()) ===
                    float_negate(float_one())
                )
                ASSERT(
                    float_divide(float_negate(float_one()), float_negate(float_one())) ===
                    float_one()
                )
                ASSERT_EQUAL(
                    float_divide(float_one(), 6107881094714392576n),
                    6071852297695428608n
                )
                ASSERT_EQUAL(
                    float_divide(6234216452170766464n, 6144532891733356544n),
                    6168530993200328528n
                )
                ASSERT_EQUAL(
                    float_divide(1478426356228633688n, 6846826132016365020n),
                    711756787386903390n
                )
                ASSERT(
                    float_divide(4638834963451748340n, float_one()) === 4638834963451748340n
                )
                ASSERT(float_divide(4638834963451748340n, 7441363081262569392n) === 0n)
                ASSERT(
                    float_divide(6846826132016365020n, 4638834963451748340n) === XFL_OVERFLOW
                )
                ASSERT_EQUAL(
                    float_divide(
                    3121244226425810900n,
                    2135203055881892282n
                    /* -9.50403176301817e+36 */
                    ),
                    7066645550312560102n
                    /* 5.001334595622374e+54 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    2473507938381460320n,
                    6365869885731270068n
                    /* 6787211884129716 */
                    ),
                    2187897766692155363n
                    /* -8.155547044835299e+39 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    1716271542690607496n,
                    3137794549622534856n
                    /* -3.28920897266964e+92 */
                    ),
                    4667220053951274769n
                    /* 1.490839995440913e-79 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    1588045991926420391n,
                    5933338827267685794n
                    /* 6.601717648113058e-9 */
                    ),
                    1733591650950017206n
                    /* -420939403974674.2 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    5880783758174228306n,
                    1396720886139976383n
                    /* -0.00009612200909863615 */
                    ),
                    1341481714205255877n
                    /* -8.416224503589061e-8 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    5567703563029955929n,
                    2184969513100691140n
                    /* -5.227293453371076e+39 */
                    ),
                    236586937995245543n
                    /* -2.399757371979751e-69 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    7333313065548121054n,
                    1755926008837497886n
                    /* -8529353417745438 */
                    ),
                    2433647177826281173n
                    /* -1.703379046213333e+53 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    1172441975040622050n,
                    6692015311011173216n
                    /* 8.673463993357152e+33 */
                    ),
                    560182767210134346n
                    /* -1.736413416192842e-51 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    577964843368607493n,
                    6422931182144699580n
                    /* 9805312769113276000 */
                    ),
                    235721135837751035n
                    /* -1.533955214485243e-69 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    6039815413139899240n,
                    2117655488444284242n
                    /* -9.970862834892113e+35 */
                    ),
                    779625635892827768n
                    /* -5.006499985102456e-39 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    1353563835098586141n,
                    6450909070545770298n
                    /* 175440415122002600000 */
                    ),
                    992207753070525611n
                    /* -1.415835049016491e-27 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    6382158843584616121n,
                    5373794957212741595n
                    /* 5.504201387110363e-40 */
                    ),
                    7088854809772330055n
                    /* 9.196195545910343e+55 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    2056891719200540975n,
                    1754532627802542730n
                    /* -7135972382790282 */
                    ),
                    6381651867337939070n
                    /* 45547949813167340 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    5730152450208688630n,
                    1663581695074866883n
                    /* -62570322025.24355 */
                    ),
                    921249452789827075n
                    /* -2.515128806245891e-31 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    6234301156018475310n,
                    2868710604383082256n
                    /* -4.4212413754468e+77 */
                    ),
                    219156721749007916n
                    /* -2.983939635224108e-70 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    2691125731495874243n,
                    7394070851520237320n
                    /* 8.16746263262388e+72 */
                    ),
                    1377640825464715759n
                    /* -0.000008546538744084975 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    5141867696142208039n,
                    5369434678231981897n
                    /* 1.143922406350665e-40 */
                    ),
                    5861466794943198400n
                    /* 6.7872793615536e-13 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    638296190872832492n,
                    5161669734904371378n
                    /* 9.551761192523954e-52 */
                    ),
                    1557396184145861422n
                    /* -81579.12330410798 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    2000727145906286285n,
                    2096625200460673392n
                    /* -6.954973360763248e+34 */
                    ),
                    5982403476503576795n
                    /* 0.000001623171355558107 */
                )
                ASSERT_EQUAL(
                    float_divide(
                    640472838055334326n,
                    5189754252349396763n
                    /* 1.607481618585371e-50 */
                    ),
                    1537425431139169736n
                    /* -6201.557833201096 */
                )
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_divide"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_divide"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_int(FeatureBitset features)
    {
        testcase("Test float_int");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var TOO_BIG = -3
                var INVALID_ARGUMENT = -7
                var CANT_RETURN_NEGATIVE = -33
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_int(void 0, 1, 1) === INVALID_ARGUMENT)
                ASSERT(float_int(1, void 0, 1) === INVALID_ARGUMENT)
                ASSERT(float_int(1, 1, void 0) === INVALID_ARGUMENT)
                ASSERT(float_int(-1, 0, 0) === INVALID_FLOAT)
                ASSERT(float_int(float_one(), 0, 0) === 1)
                ASSERT(float_int(5729808726015270912n, 0, 0) === 0)
                ASSERT(float_int(5729808726015270912n, 15, 0) === 0)
                ASSERT(float_int(5729808726015270912n, 16, 0) === INVALID_ARGUMENT)
                ASSERT(float_int(float_one(), 15, 0) === 1e15)
                ASSERT(float_int(float_one(), 14, 0) === 1e14)
                ASSERT(float_int(float_one(), 13, 0) === 1e13)
                ASSERT(float_int(float_one(), 12, 0) === 1e12)
                ASSERT(float_int(float_one(), 11, 0) === 1e11)
                ASSERT(float_int(float_one(), 10, 0) === 1e10)
                ASSERT(float_int(float_one(), 9, 0) === 1e9)
                ASSERT(float_int(float_one(), 8, 0) === 1e8)
                ASSERT(float_int(float_one(), 7, 0) === 1e7)
                ASSERT(float_int(float_one(), 6, 0) === 1e6)
                ASSERT(float_int(float_one(), 5, 0) === 1e5)
                ASSERT(float_int(float_one(), 4, 0) === 1e4)
                ASSERT(float_int(float_one(), 3, 0) === 1e3)
                ASSERT(float_int(float_one(), 2, 0) === 100)
                ASSERT(float_int(float_one(), 1, 0) === 10)
                ASSERT(float_int(float_one(), 0, 0) === 1)
                ASSERT(float_int(6360317241828374919n, 0, 0) === 1234567981234567)
                ASSERT(float_int(6360317241828374919n, 1, 0) === TOO_BIG)
                ASSERT(float_int(6360317241828374919n, 15, 0) === TOO_BIG)
                ASSERT(float_int(6090101264186145159n, 0, 0) === 1)
                ASSERT(float_int(6090101264186145159n, 1, 0) === 12)
                ASSERT(float_int(6090101264186145159n, 2, 0) === 123)
                ASSERT(float_int(6090101264186145159n, 3, 0) === 1234)
                ASSERT(float_int(6090101264186145159n, 4, 0) === 12345)
                ASSERT(float_int(6090101264186145159n, 5, 0) === 123456)
                ASSERT(float_int(6090101264186145159n, 6, 0) === 1234567)
                ASSERT(float_int(6090101264186145159n, 7, 0) === 12345679)
                ASSERT(float_int(6090101264186145159n, 8, 0) === 123456798)
                ASSERT(float_int(6090101264186145159n, 9, 0) === 1234567981)
                ASSERT(float_int(6090101264186145159n, 10, 0) === 12345679812)
                ASSERT(float_int(6090101264186145159n, 11, 0) === 123456798123)
                ASSERT(float_int(6090101264186145159n, 12, 0) === 1234567981234)
                ASSERT(float_int(6090101264186145159n, 13, 0) === 12345679812345)
                ASSERT(float_int(6090101264186145159n, 14, 0) === 123456798123456)
                ASSERT(float_int(6090101264186145159n, 15, 0) === 1234567981234567)
                ASSERT(float_int(1478415245758757255n, 0, 1) === 1)
                ASSERT(float_int(1478415245758757255n, 1, 1) === 12)
                ASSERT(float_int(1478415245758757255n, 2, 1) === 123)
                ASSERT(float_int(1478415245758757255n, 3, 1) === 1234)
                ASSERT(float_int(1478415245758757255n, 4, 1) === 12345)
                ASSERT(float_int(1478415245758757255n, 5, 1) === 123456)
                ASSERT(float_int(1478415245758757255n, 6, 1) === 1234567)
                ASSERT(float_int(1478415245758757255n, 7, 1) === 12345679)
                ASSERT(float_int(1478415245758757255n, 8, 1) === 123456798)
                ASSERT(float_int(1478415245758757255n, 9, 1) === 1234567981)
                ASSERT(float_int(1478415245758757255n, 10, 1) === 12345679812)
                ASSERT(float_int(1478415245758757255n, 11, 1) === 123456798123)
                ASSERT(float_int(1478415245758757255n, 12, 1) === 1234567981234)
                ASSERT(float_int(1478415245758757255n, 13, 1) === 12345679812345)
                ASSERT(float_int(1478415245758757255n, 14, 1) === 123456798123456)
                ASSERT(float_int(1478415245758757255n, 15, 1) === 1234567981234567)
                ASSERT(float_int(1478415245758757255n, 15, 0) === CANT_RETURN_NEGATIVE)
                ASSERT(float_int(5819885286543915399n, 15, 0) === 1)
                for (let i = 1; i < 15; ++i)
                    ASSERT(float_int(5819885286543915399n, i, 0) === 0)
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_int"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_int"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_invert(FeatureBitset features)
    {
        testcase("Test float_invert");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var float_exponent = (f) => ((((f) >> 54n) & 0xFFn) - 97n)
                var ASSERT_EQUAL = (x, y) => {
                const px = (x);
                const py = (y);
                let mx = Number(float_mantissa(px));
                let my = Number(float_mantissa(py));
                let diffexp = Number(float_exponent(px)) - Number(float_exponent(py));
                if (diffexp == 1)
                    mx *= 10;
                if (diffexp == -1)
                    my *= 10;
                let diffman = mx - my;
                if (diffman < 0) diffman *= -1;
                if (diffexp < 0) diffexp *= -1;
                if (diffexp > 1 || diffman > 5000000 || mx < 0 || my < 0)
                    rollback('', 1);
                }
                var INVALID_ARGUMENT = -7
                var DIVISION_BY_ZERO = -25
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_invert(void 0) === INVALID_ARGUMENT)
                ASSERT(float_invert(0) === DIVISION_BY_ZERO)
                ASSERT(float_invert(-1) === INVALID_FLOAT)
                ASSERT(float_invert(float_one()) === float_one())
                ASSERT_EQUAL(float_invert(6107881094714392576n), 6071852297695428608n)
                ASSERT_EQUAL(float_invert(6126125493223874560n), 6042953581977277640n)
                ASSERT_EQUAL(float_invert(6360317241747140351n), 5808736320061298978n)
                ASSERT_EQUAL(float_invert(4630700416936869888n), 7549032975472951296n)
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_invert"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_invert"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_log(FeatureBitset features)
    {
        testcase("Test float_log");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var COMPLEX_NOT_SUPPORTED = -39
                var Hook = (arg) => {
                ASSERT(float_log(void 0) === INVALID_ARGUMENT)
                if (float_log(0) !== INVALID_ARGUMENT) rollback('false', 0)
                ASSERT(float_log(6349533412187342878n) === 6108373858112734914n)
                if (float_log(1532223873305968640n) !== COMPLEX_NOT_SUPPORTED)
                    rollback('false', 0)
                ASSERT(float_log(6143909891733356544n) === 6091866696204910592n)
                ASSERT(float_log(6071976107695428608n) === 1468659350345448364n)
                ASSERT(float_log(5783744921543716864n) === 1496890038311378526n)
                ASSERT(float_log(7206759403792793600n) === 6113081094714392576n)
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_log"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_log"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_mantissa(FeatureBitset features)
    {
        testcase("Test float_mantissa");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_mantissa(void 0) === INVALID_ARGUMENT)
                {
                    ASSERT(float_mantissa(-1) === INVALID_FLOAT)
                    ASSERT(float_mantissa(-11010191919n) === INVALID_FLOAT)
                }
                ASSERT(float_mantissa(0) === 0)
                {
                    ASSERT(float_mantissa(float_one()) === 1000000000000000n)
                    ASSERT(float_mantissa(float_negate(float_one())) === 1000000000000000n)
                }
                {
                    ASSERT(
                    float_mantissa(
                        4763370308433150973n
                        /* 7.569101929907197e-74 */
                    ) === 7569101929907197n
                    )
                    ASSERT(
                    float_mantissa(
                        668909658849475214n
                        /* -2.376913998641806e-45 */
                    ) === 2376913998641806n
                    )
                    ASSERT(
                    float_mantissa(
                        962271544155031248n
                        /* -7.508423152486096e-29 */
                    ) === 7508423152486096n
                    )
                    ASSERT(
                    float_mantissa(
                        7335644976228470276n
                        /* 3.784782869302788e+69 */
                    ) === 3784782869302788n
                    )
                    ASSERT(
                    float_mantissa(
                        2837780149340315954n
                        /* -9.519583351644467e+75 */
                    ) === 9519583351644466n
                    )
                    ASSERT(
                    float_mantissa(
                        2614004940018599738n
                        /* -1.917156143712058e+63 */
                    ) === 1917156143712058n
                    )
                    ASSERT(
                    float_mantissa(
                        4812250541755005603n
                        /* 2.406139723315875e-71 */
                    ) === 2406139723315875n
                    )
                    ASSERT(
                    float_mantissa(
                        5140304866732560580n
                        /* 6.20129153019514e-53 */
                    ) === 6201291530195140n
                    )
                    ASSERT(
                    float_mantissa(
                        1124677839589482624n
                        /* -7.785132001599617e-20 */
                    ) === 7785132001599616n
                    )
                    ASSERT(
                    float_mantissa(
                        5269336076015865585n
                        /* 9.131711247126257e-46 */
                    ) === 9131711247126257n
                    )
                    ASSERT(
                    float_mantissa(
                        2296179634826760368n
                        /* -8.3510241225484e+45 */
                    ) === 8351024122548400n
                    )
                    ASSERT(
                    float_mantissa(
                        1104028240398536470n
                        /* -5.149931320135446e-21 */
                    ) === 5149931320135446n
                    )
                    ASSERT(
                    float_mantissa(
                        2691222059222981864n
                        /* -7.076681310166248e+67 */
                    ) === 7076681310166248n
                    )
                    ASSERT(
                    float_mantissa(
                        6113256168823855946n
                        /* 63.7507410946337 */
                    ) === 6375074109463370n
                    )
                    ASSERT(
                    float_mantissa(
                        311682216630003626n
                        /* -5.437441968809898e-65 */
                    ) === 5437441968809898n
                    )
                    ASSERT(
                    float_mantissa(
                        794955605753965262n
                        /* -2.322071336757966e-38 */
                    ) === 2322071336757966n
                    )
                    ASSERT(
                    float_mantissa(
                        204540636400815950n
                        /* -6.382252796514126e-71 */
                    ) === 6382252796514126n
                    )
                    ASSERT(
                    float_mantissa(
                        5497195278343034975n
                        /* 2.803732951029855e-33 */
                    ) === 2803732951029855n
                    )
                    ASSERT(
                    float_mantissa(
                        1450265914369875626n
                        /* -0.09114033611316906 */
                    ) === 9114033611316906n
                    )
                    ASSERT(
                    float_mantissa(
                        7481064015089962668n
                        /* 5.088633654939308e+77 */
                    ) === 5088633654939308n
                    )
                }
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_mantissa"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_mantissa"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_mulratio(FeatureBitset features)
    {
        testcase("Test float_mulratio");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var XFL_OVERFLOW = -30
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_mulratio(void 0, 1, 1, 1) === INVALID_ARGUMENT)
                ASSERT(float_mulratio(float_one(), void 0, 1, 1) === INVALID_ARGUMENT)
                ASSERT(float_mulratio(float_one(), 1, void 0, 1) === INVALID_ARGUMENT)
                ASSERT(float_mulratio(float_one(), 1, 1, void 0) === INVALID_ARGUMENT)
                ASSERT(float_mulratio(-1, 0, 1, 1) === INVALID_FLOAT)
                ASSERT(float_mulratio(float_one(), 0, 0, 1) === 0n)
                ASSERT(float_mulratio(0, 0, 1, 1) === 0n)
                ASSERT(float_mulratio(float_one(), 0, 1, 1) === float_one())
                ASSERT(
                    float_mulratio(float_negate(float_one()), 0, 1, 1) ===
                    float_negate(float_one())
                )
                ASSERT(
                    float_mulratio(7801234554605699072n, 0, 4294967295, 1) === XFL_OVERFLOW
                )
                ASSERT(float_mulratio(7801234554605699072n, 0, 10, 1) === XFL_OVERFLOW)
                ASSERT(float_mulratio(3189548536178311168n, 0, 10, 1) === XFL_OVERFLOW)
                ASSERT(float_mulratio(3189548536178311168n, 0, 1, 1) === 3189548536178311168n)
                ASSERT(
                    float_mulratio(2296131684119423544n, 0, 2210828011, 2814367554) ===
                    2294351094683836182n
                )
                ASSERT(
                    float_mulratio(565488225163275031n, 0, 2373474507, 4203973264) ===
                    562422045628095449n
                )
                ASSERT(
                    float_mulratio(2292703263479286183n, 0, 3170020147, 773892643) ===
                    2307839765178024100n
                )
                ASSERT(
                    float_mulratio(758435948837102675n, 0, 3802740780, 1954123588) ===
                    760168290112163547n
                )
                ASSERT(
                    float_mulratio(3063742137774439410n, 0, 2888815591, 4122448592) ===
                    3053503824756415637n
                )
                ASSERT(
                    float_mulratio(974014561126802184n, 0, 689168634, 3222648522) ===
                    957408554638995792n
                )
                ASSERT(
                    float_mulratio(2978333847445611553n, 0, 1718558513, 2767410870) ===
                    2976075722223325259n
                )
                ASSERT(
                    float_mulratio(6577058837932757648n, 0, 1423256719, 1338068927) ===
                    6577173649752398013n
                )
                ASSERT(
                    float_mulratio(2668681541248816636n, 0, 345215754, 4259223936) ===
                    2650183845127530219n
                )
                ASSERT(
                    float_mulratio(651803640367065917n, 0, 327563234, 1191613855) ===
                    639534906402789367n
                )
                ASSERT(
                    float_mulratio(3154958130393015979n, 0, 1304112625, 3024066701) ===
                    3153571282364880741n
                )
                ASSERT(
                    float_mulratio(1713286099776800976n, 0, 1902151138, 2927030061) ===
                    1712614441093927707n
                )
                ASSERT(
                    float_mulratio(2333142120591277120n, 0, 914099656, 108514965) ===
                    2349692988167140473n
                )
                ASSERT(
                    float_mulratio(995968561418010814n, 0, 1334462574, 846156977) ===
                    998955931389416093n
                )
                ASSERT(
                    float_mulratio(6276035843030312442n, 0, 2660687613, 236740983) ===
                    6294920527635363073n
                )
                ASSERT(
                    float_mulratio(7333118474702086419n, 0, 46947714, 2479204760) ===
                    7298214153648998534n
                )
                ASSERT(
                    float_mulratio(2873297486994296492n, 0, 880591893, 436034100) ===
                    2884122995598532758n
                )
                ASSERT(
                    float_mulratio(1935815261812737573n, 0, 3123665800, 3786746543) ===
                    1934366328810191207n
                )
                ASSERT(
                    float_mulratio(7249556282125616118n, 0, 2378803159, 2248850590) ===
                    7250005170160875416n
                )
                ASSERT(
                    float_mulratio(311005347529659996n, 0, 992915590, 2433548552) ===
                    308187142737041831n
                )
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_mulratio"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_mulratio"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_multiply(FeatureBitset features)
    {
        testcase("Test float_multiply");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var XFL_OVERFLOW = -30
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_multiply(void 0, float_one()) === INVALID_ARGUMENT)
                ASSERT(float_multiply(float_one(), void 0) === INVALID_ARGUMENT)
                ASSERT(float_multiply(-1, float_one()) === INVALID_FLOAT)
                ASSERT(float_multiply(float_one(), 0) === 0n)
                ASSERT(float_multiply(0, float_one()) === 0n)
                ASSERT(float_multiply(float_one(), float_one()) === float_one())
                ASSERT(
                    float_multiply(float_one(), float_negate(float_one())) ===
                    float_negate(float_one())
                )
                ASSERT(
                    float_multiply(float_negate(float_one()), float_one()) ===
                    float_negate(float_one())
                )
                ASSERT(
                    float_multiply(float_negate(float_one()), float_negate(float_one())) ===
                    float_one()
                )
                ASSERT(
                    float_multiply(7801234554605699072n, 7801234554605699072n) === XFL_OVERFLOW
                )
                ASSERT(
                    float_multiply(7801234554605699072n, 6107881094714392576n) === XFL_OVERFLOW
                )
                ASSERT(
                    float_multiply(6107881094714392576n, 7801234554605699072n) === XFL_OVERFLOW
                )
                ASSERT(
                    float_multiply(3189548536178311168n, 6107881094714392576n) === XFL_OVERFLOW
                )
                ASSERT(
                    float_multiply(3189548536178311168n, float_one()),
                    3189548536178311168n
                )
                ASSERT(
                    float_multiply(float_one(), 3189548536178311168n),
                    3189548536178311168n
                )
                ASSERT(
                    float_multiply(
                    7791757438262485039n,
                    4759088999670263908n
                    /* 3.287793167020132e-74 */
                    ) === 6470304726017852129n
                    /* 3.135661113819873e+21 */
                )
                ASSERT(
                    float_multiply(
                    7534790022873909775n,
                    1017891960669847079n
                    /* -9.085644138855975e-26 */
                    ) === 2472307761756037978n
                    /* -4.335165957006171e+55 */
                )
                ASSERT(
                    float_multiply(
                    2813999069907898454n,
                    4962524721184225460n
                    /* 8.56513107667986e-63 */
                    ) === 1696567870013294731n
                    /* -3214410121988.235 */
                )
                ASSERT(
                    float_multiply(
                    2151742066453140308n,
                    437647738130579252n
                    /* -5.302173903011636e-58 */
                    ) === 5732835652591705549n
                    /* 4.256926576434637e-20 */
                )
                ASSERT(
                    float_multiply(
                    5445302332922546340n,
                    7770966530708354172n
                    /* 6.760773121619068e+93 */
                    ) === 7137051085305881332n
                    /* 3.349275551015668e+58 */
                )
                ASSERT(
                    float_multiply(
                    2542989542826132533n,
                    6308418769944702613n
                    /* 3379291626008.213 */
                    ) === 2775217422137696933n
                    /* -1.000051677471398e+72 */
                )
                ASSERT(
                    float_multiply(
                    5017652318929433511n,
                    6601401767766764916n
                    /* 8.131913296358772e+28 */
                    ) === 5538267259220228819n
                    /* 7.846916809259732e-31 */
                )
                ASSERT(
                    float_multiply(
                    892430323307269235n,
                    1444078017997143500n
                    /* -0.0292613723858478 */
                    ) === 5479222755754111850n
                    /* 2.845608871588714e-34 */
                )
                ASSERT(
                    float_multiply(
                    7030632722283214253n,
                    297400838197636668n
                    /* -9.170462045924924e-66 */
                    ) === 1247594596364389994n
                    /* -4.601099210133098e-13 */
                )
                ASSERT(
                    float_multiply(
                    1321751204165279730n,
                    2451801790748530375n
                    /* -1.843593458980551e+54 */
                    ) === 6918764256086244704n
                    /* 1.235228445162848e+46 */
                )
                ASSERT(
                    float_multiply(
                    2055496484261758590n,
                    2079877890137711361n
                    /* -8.222061547283201e+33 */
                    ) === 7279342234795540004n
                    /* 1.525236964818469e+66 */
                )
                ASSERT(
                    float_multiply(
                    2439875962311968674n,
                    4707485682591872793n
                    /* 5.727671617074969e-77 */
                    ) === 1067392794851803610n
                    /* -4.543282792366554e-23 */
                )
                ASSERT(
                    float_multiply(
                    6348574818322812800n,
                    6474046245013515838n
                    /* 6.877180109483582e+21 */
                    ) === 6742547427357110773n
                    /* 5.162384810848757e+36 */
                )
                ASSERT(
                    float_multiply(
                    1156137305783593424n,
                    351790564990861307n
                    /* -9.516993310703611e-63 */
                    ) === 4650775291275116746n
                    /* 3.060475828764875e-80 */
                )
                ASSERT(
                    float_multiply(
                    5786888485280994123n,
                    6252137323085080394n
                    /* 1141040294.831946 */
                    ) === 5949619829273756853n
                    /* 4.868321144702132e-8 */
                )
                ASSERT(
                    float_multiply(
                    2078182880999439640n,
                    1662438186251269392n
                    /* -51135233789.26864 */
                    ) === 6884837854131013999n
                    /* 3.33762350889611e+44 */
                )
                ASSERT(
                    float_multiply(
                    1823781083140711248n,
                    1120252241608199010n
                    /* -3.359534020316002e-20 */
                    ) === 6090320310700749729n
                    /* 1.453614495839137 */
                )
                ASSERT(
                    float_multiply(
                    6617782604883935174n,
                    6185835042802056262n
                    /* 689635.404973575 */
                    ) === 6723852137583788318n
                    /* 4.481493547008287e+35 */
                )
                ASSERT(
                    float_multiply(
                    333952667495151166n,
                    1556040883317758614n
                    /* -68026.1150230799 */
                    ) === 5032611291744396930n
                    /* 6.594107598923394e-59 */
                )
                ASSERT(
                    float_multiply(
                    2326968399632616779n,
                    707513695207834635n
                    /* -4.952153338037259e-43 */
                    ) === 6180479299649214949n
                    /* 154061.0896894437 */
                )
                ASSERT(
                    float_multiply(
                    1271003508324696477n,
                    5321949753651889765n
                    /* 7.702193354704484e-43 */
                    ) === 512101972406838313n
                    /* -7.698814141342762e-54 */
                )
                ASSERT(
                    float_multiply(
                    1928646740923345323n,
                    4639329980209973352n
                    /* 9.629563273103463e-81 */
                    ) === 487453886143282122n
                    /* -1.065126387268554e-55 */
                )
                ASSERT(
                    float_multiply(
                    6023906813956669432n,
                    944348444470060009n
                    /* -7.599721976996842e-30 */
                    ) === 888099590592064433n
                    /* -5.394063627447218e-33 */
                )
                ASSERT(
                    float_multiply(
                    6580290597764062787n,
                    6164319297265300034n
                    /* 33950.07022461506 */
                    ) === 6667036882686408592n
                    /* 1.709434178074513e+32 */
                )
                ASSERT(
                    float_multiply(
                    2523439530503240484n,
                    5864448766677980801n
                    /* 9.769251096336e-13 */
                    ) === 2307233895764065602n
                    /* -1.39088655037165e+46 */
                )
                ASSERT(
                    float_multiply(
                    6760707453987140465n,
                    5951641080643457645n
                    /* 6.889572514402925e-8 */
                    ) === 6632955645489194550n
                    /* 3.656993999824438e+30 */
                )
                ASSERT(
                    float_multiply(
                    6494270716308443375n,
                    564752637895553836n
                    /* -6.306284101612332e-51 */
                    ) === 978508199357889360n
                    /* -5.730679845862224e-28 */
                )
                ASSERT(
                    float_multiply(
                    6759145618427534062n,
                    4721897842483633304n
                    /* 2.125432999353496e-76 */
                    ) === 5394267403342547164n
                    /* 7.962249007433949e-39 */
                )
                ASSERT(
                    float_multiply(
                    1232673571201806425n,
                    6884256144221925318n
                    /* 2.75591359980743e+44 */
                    ) === 2037747561727791011n
                    /* -2.12053015632682e+31 */
                )
                ASSERT(
                    float_multiply(
                    1427694775835421031n,
                    4883952867277976402n
                    /* 2.050871208358738e-67 */
                    ) === 225519204318055258n
                    /* -9.34642220427145e-70 */
                )
                ASSERT(
                    float_multiply(
                    5843509949864662087n,
                    5264483986612843822n
                    /* 4.279621844104494e-46 */
                    ) === 5028946513739275800n
                    /* 2.929329593802264e-59 */
                )
                ASSERT(
                    float_multiply(
                    6038444022009738988n,
                    7447499078040748850n
                    /* 7.552493624689458e+75 */
                    ) === 7406652183825856092n
                    /* 2.734396428760669e+73 */
                )
                ASSERT(
                    float_multiply(
                    939565473697468970n,
                    1100284903077087966n
                    /* -1.406593998686942e-21 */
                    ) === 5174094397561240824n
                    /* 3.962025339911417e-51 */
                )
                ASSERT(
                    float_multiply(
                    5694071830210473617n,
                    5536709154363579683n
                    /* 6.288811952610595e-31 */
                    ) === 5143674525748709390n
                    /* 9.570950546343951e-53 */
                )
                ASSERT(
                    float_multiply(
                    600729862341871819n,
                    6330630279715378440n
                    /* 75764028872020.56 */
                    ) === 851415551394320909n
                    /* -4.738821448667662e-35 */
                )
                ASSERT(
                    float_multiply(
                    1876763139233864902n,
                    4849561230315278754n
                    /* 3.688031264625058e-69 */
                    ) === 649722744589988028n
                    /* -1.204398248636604e-46 */
                )
                ASSERT(
                    float_multiply(
                    3011947542126279863n,
                    1557732559110376235n
                    /* -84942.87294925611 */
                    ) === 7713172080438368541n
                    /* 3.009518380079389e+90 */
                )
                ASSERT(
                    float_multiply(
                    5391579936313268788n,
                    1018647290024655822n
                    /* -9.840973493664718e-26 */
                    ) === 329450072133864645n
                    /* -5.190898963188932e-64 */
                )
                ASSERT(
                    float_multiply(
                    2815029221608845312n,
                    4943518985822088837n
                    /* 7.57379422402522e-64 */
                    ) === 1678961648155863225n
                    /* -362258677403.8713 */
                )
                ASSERT(
                    float_multiply(
                    1377509900308195934n,
                    7702104197062186199n
                    /* 9.95603351337903e+89 */
                    ) === 2998768765665354001n
                    /* -8.378613091344656e+84 */
                )
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_multiply"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_multiply"), fee(XRP(1)));
            env.close();
        }
    }

    // void
    // test_emit(FeatureBitset features)

    void
    test_otxn_field(FeatureBitset features)
    {
        testcase("Test otxn_field");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const INVALID_FIELD = -17
            const sfAccount = 0x80001

            const ASSERT = (x, code) => {
                if (!x) {
                    rollback(x.toString(), code);
                }
            }

            const Hook = (arg) => {
                const acc = otxn_field(sfAccount);
                ASSERT(typeof acc != 'number');
                ASSERT(otxn_field(1) == INVALID_FIELD);

                ASSERT(acc.length == 20);

                let acc2 = hook_account();
                ASSERT(acc2.length == 20);

                for (var i = 0; i < 20; ++i)
                    ASSERT(acc[i] == acc2[i]);

                return accept("0", 0);
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set otxn_field"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(alice, bob, XRP(1)), M("test otxn_field"), fee(XRP(1)));
    }

    void
    test_float_negate(FeatureBitset features)
    {
        testcase("Test float_negate");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback(x.toString(), 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_negate(void 0) === INVALID_ARGUMENT)
                {
                    ASSERT(float_negate(-1) === INVALID_FLOAT)
                    ASSERT(float_negate(-11010191919n) === INVALID_FLOAT)
                }
                ASSERT(float_negate(0) === 0)
                {
                    ASSERT(float_negate(float_one()) !== float_one())
                    ASSERT(float_negate(float_negate(float_one())) === float_one())
                }
                {
                    ASSERT(float_negate(6488646939756037240n) === 1876960921328649336n)
                    ASSERT(float_negate(float_one()) === 1478180677777522688n)
                    ASSERT(float_negate(1838620299498162368n) === 6450306317925550272n)
                }
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_negate"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_negate"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_one(FeatureBitset features)
    {
        testcase("Test float_one");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var Hook = (arg) => {
                const f = float_one()
                f === 6089866696204910592n ? accept('', 2) : rollback('', 1)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_one"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_one"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_root(FeatureBitset features)
    {
        testcase("Test float_root");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback('ASSERT.error', 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var COMPLEX_NOT_SUPPORTED = -39
                var Hook = (arg) => {
                ASSERT(float_root(void 0, 0) === INVALID_ARGUMENT)
                ASSERT(float_root(0, void 0) === INVALID_ARGUMENT)
                ASSERT(float_root(float_one(), 2), float_one())
                ASSERT(float_root(6097866696204910592n, 2) === 6091866696204910592n)
                ASSERT(float_root(6143909891733356544n, 3) === 6098866696204910590n)
                ASSERT(float_root(1478180677777522688n, 2) === COMPLEX_NOT_SUPPORTED)
                ASSERT(float_root(0, 10) === 0n)
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_root"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_root"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_set(FeatureBitset features)
    {
        testcase("Test float_set");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback('ASSERT.error', 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_set(void 0, 0) === INVALID_ARGUMENT)
                ASSERT(float_set(0, void 0) === INVALID_ARGUMENT)
                ASSERT(float_set(2147483648, 0) === INVALID_ARGUMENT)
                ASSERT(float_set(-5, 0) === 0)
                ASSERT(float_set(50, 0) === 0)
                ASSERT(float_set(-50, 0) === 0)
                ASSERT(float_set(0, 0) === 0)
                ASSERT(float_set(-97, 1) === INVALID_FLOAT)
                ASSERT(float_set(97, 1) === INVALID_FLOAT)
                ASSERT(float_set(-5, 6541432897943971n) === 6275552114197674403n)
                ASSERT(float_set(-83, 7906202688397446n) === 4871793800248533126n)
                ASSERT(float_set(76, 4760131426754533n) === 7732937091994525669n)
                ASSERT(float_set(37, -8019384286534438n) === 2421948784557120294n)
                ASSERT(float_set(50, 5145342538007840n) === 7264947941859247392n)
                ASSERT(float_set(-70, 4387341302202416n) === 5102462119485603888n)
                ASSERT(float_set(-26, -1754544005819476n) === 1280776838179040340n)
                ASSERT(float_set(36, 8261761545780560n) === 7015862781734272336n)
                ASSERT(float_set(35, 7975622850695472n) === 6997562244529705264n)
                ASSERT(float_set(17, -4478222822793996n) === 2058119652903740172n)
                ASSERT(float_set(-53, 5506604247857835n) === 5409826157092453035n)
                ASSERT(float_set(-60, 5120164869507050n) === 5283338928147728362n)
                ASSERT(float_set(41, 5176113875683063n) === 7102849126611584759n)
                ASSERT(float_set(-54, -3477931844992923n) === 778097067752718235n)
                ASSERT(float_set(21, 6345031894305479n) === 6743730074440567495n)
                ASSERT(float_set(-23, 5091583691147091n) === 5949843091820201811n)
                ASSERT(float_set(-33, 7509684078851678n) === 5772117207113086558n)
                ASSERT(float_set(-72, -1847771838890268n) === 452207734575939868n)
                ASSERT(float_set(71, -9138413713437220n) === 3035557363306410532n)
                ASSERT(float_set(28, 4933894067102586n) === 6868419726179738490n)
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_set"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_set"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_sign(FeatureBitset features)
    {
        testcase("Test float_sign");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x) => {
                if (!x) {
                    rollback('ASSERT.error', 0)
                }
                }
                var INVALID_ARGUMENT = -7
                var INVALID_FLOAT = -10024
                var Hook = (arg) => {
                ASSERT(float_sign(void 0) === INVALID_ARGUMENT)
                {
                    ASSERT(float_sign(-1) === INVALID_FLOAT)
                    ASSERT(float_sign(-11010191919n) === INVALID_FLOAT)
                }
                ASSERT(float_sign(0) === 0)
                ASSERT(float_sign(float_one()) === 0)
                ASSERT(float_sign(float_negate(float_one())) === 1)
                ASSERT(
                    float_sign(
                    7248434512952957686n
                    /* 6.646312141200119e+64 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    889927818394811978n
                    /* -7.222291430194763e-33 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    5945816149233111421n
                    /* 1.064641104056701e-8 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    6239200145838704863n
                    /* 621826155.7938399 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    6992780785042190360n
                    /* 3.194163363180568e+50 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    6883099933108789087n
                    /* 1.599702486671199e+44 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    890203738162163464n
                    /* -7.498211197546248e-33 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    4884803073052080964n
                    /* 2.9010769824633e-67 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    2688292350356944394n
                    /* -4.146972444128778e+67 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    4830109852288093280n
                    /* 2.251051746921568e-70 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    294175951907940320n
                    /* -5.945575756228576e-66 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    7612037404955382316n
                    /* 9.961233953985069e+84 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    7520840929603658997n
                    /* 8.83675114967167e+79 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    4798982086157926282n
                    /* 7.152082635718538e-72 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    689790136568817905n
                    /* -5.242993208502513e-44 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    5521738045011558042n
                    /* 9.332101110070938e-32 */
                    ) === 0
                )
                ASSERT(
                    float_sign(
                    728760820583452906n
                    /* -8.184880204173546e-42 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    2272937984362856794n
                    /* -3.12377216812681e+44 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    1445723661896317830n
                    /* -0.0457178113775911 */
                    ) === 1
                )
                ASSERT(
                    float_sign(
                    5035721527359772724n
                    /* 9.704343214299189e-59 */
                    ) === 0
                )
                accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_sign"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_sign"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_sto(FeatureBitset features)
    {
        testcase("Test float_sto");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                const INVALID_FLOAT = -10024
                const INVALID_ARGUMENT = -7
                const ASSERT = (x) => {
                    if (!x) rollback(x.toString(), 1)
                }
                const sfAmount = (6 << 16) + 1
                const sfDeliveredAmount = (6 << 16) + 18
                const cur1 = ['U'.charCodeAt(0), 'S'.charCodeAt(0), 'D'.charCodeAt(0)]
                const cur1full = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ...cur1, 0, 0, 0, 0, 0]

                const BUFFER_EQUAL_20 = (buf1, buf2) => {
                    if (buf1.length !== 20 || buf2.length !== 20) return false
                    for (let i = 0; i < 20; i++) {
                        if (buf1[i] !== buf2[i]) return false
                    }
                    return true
                }

                const Hook = (reserved) => {
                    let y
                    const cur2 = new Array(20).fill(0)

                    const iss = hook_account()
                    ASSERT(typeof iss !== 'number' && iss.length === 20)
                    // zero issuer/currency pointers must be accompanied by 0 length
                    // TODO: https://github.com/Xahau/xahaud/issues/461
                    // ASSERT(float_sto([0], undefined, 0n, 0) === INVALID_ARGUMENT)
                    // ASSERT(float_sto(undefined, [0], 0n, 0) === INVALID_ARGUMENT)

                    // zero issuer/currency lengths must tbe accompanied by 0 pointers
                    ASSERT(float_sto([], undefined, 0n, 0) === INVALID_ARGUMENT)
                    ASSERT(float_sto(undefined, [], 0n, 0) === INVALID_ARGUMENT)

                    // issuer without currency is invalid
                    ASSERT(float_sto(undefined, iss, 0n, sfAmount) === INVALID_ARGUMENT)

                    // currency without issuer is invalid
                    ASSERT(float_sto(cur1, undefined, 0n, sfAmount) === INVALID_ARGUMENT)

                    // currency and issuer with field code 0 = XRP is invalid
                    ASSERT(float_sto(cur1, iss, 0n, 0) === INVALID_ARGUMENT)

                    // invalid XFL
                    ASSERT(float_sto(cur2, iss, -1n, sfAmount) === INVALID_FLOAT)

                    // currency and issuer with field code not XRP is valid (XFL = 1234567.0)
                    // try with a three letter currency
                    y = float_sto(cur1, iss, 6198187654261802496n, sfAmount)
                    ASSERT(typeof y !== 'number' && y.length === 49)

                    // check the output contains the correct currency code
                    ASSERT(BUFFER_EQUAL_20(y.slice(9, 29), cur1full))

                    // again with a 20 byte currency
                    y = float_sto(cur2, iss, 6198187654261802496n, sfAmount)
                    ASSERT(typeof y !== 'number' && y.length === 49)

                    // check the output contains the correct currency code
                    ASSERT(BUFFER_EQUAL_20(y.slice(9, 29), cur2))

                    // check the output contains the correct issuer
                    ASSERT(BUFFER_EQUAL_20(y.slice(29, 49), iss))

                    // check the field code is correct
                    ASSERT(y[0] === 0x61) // sfAmount

                    // reverse the operation and check the XFL amount is correct
                    ASSERT(float_sto_set(y) === 6198187654261802496n)

                    // test 0
                    y = float_sto(cur2, iss, 0n, sfAmount)
                    ASSERT(typeof y !== 'number' && y.length === 49)
                    ASSERT(float_sto_set(y) === 0n)

                    y = float_sto(cur2, iss, 6198187654261802496n, sfDeliveredAmount)
                    ASSERT(typeof y !== 'number' && y.length === 50)

                    // check the first 2 bytes
                    ASSERT(y[0] === 0x60 && y[1] === 0x12)

                    // same checks as above moved along one
                    // check the output contains the correct currency code
                    ASSERT(BUFFER_EQUAL_20(y.slice(10, 30), cur2))

                    // check the output contains the correct issuer
                    ASSERT(BUFFER_EQUAL_20(y.slice(30, 50), iss))

                    // reverse the operation and check the XFL amount is correct
                    ASSERT(float_sto_set(y) === 6198187654261802496n)

                    // and the same again except use -1 as field code to supress field type bytes
                    {
                        // zero the serialized amount bytes
                        for (let i = 0; i < 8; i++) y[2 + i] = 0
                        let z
                        // request fieldcode -1 = only serialize the number
                        z = float_sto(undefined, undefined, 6198187654261802496n, 0xffffffff)
                        ASSERT(typeof z !== 'number' && z.length === 8)
                        for (let i = 0; i < 8; i++) y[2 + i] = z[i]

                        // reverse the operation and check the XFL amount is correct
                        ASSERT(float_sto_set(y) === 6198187654261802496n)

                        // try again with some different xfls
                        z = float_sto(undefined, undefined, 1244912689067196128n, 0xffffffff)

                        ASSERT(typeof z !== 'number' && z.length === 8)
                        for (let i = 0; i < 8; i++) y[2 + i] = z[i]

                        ASSERT(float_sto_set(y) === 1244912689067196128n)

                        // test 0
                        z = float_sto(undefined, undefined, 0n, 0xffffffff)
                        ASSERT(typeof z !== 'number' && z.length === 8)
                        for (let i = 0; i < 8; i++) y[2 + i] = z[i]

                        ASSERT(float_sto_set(y) === 0n)
                    }

                    // finally test xrp
                    {
                        // zero the serialized amount bytes
                        for (let i = 0; i < 8; i++) y[2 + i] = 0

                        // request fieldcode 0 = xrp amount serialized
                        let z
                        z = float_sto(undefined, undefined, 6198187654261802496n, 0)

                        ASSERT(typeof z !== 'number' && z.length === 8)
                        for (let i = 0; i < 8; i++) y[1 + i] = z[i]

                        y[0] = 0x61

                        ASSERT(float_sto_set(y.slice(0, 9)) === 6198187654261802496n)

                        // test 0
                        z = float_sto(undefined, undefined, 0n, 0)
                        for (let i = 0; i < 8; i++) y[1 + i] = z[i]
                        ASSERT(float_sto_set(y.slice(0, 9)) === 0n)
                        //6198187654373024496
                    }

                    return accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_sto"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_sto"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_sto_set(FeatureBitset features)
    {
        testcase("Test float_sto_set");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                const NOT_AN_OBJECT = -23
                const ASSERT = (x) => {
                   if (!x) rollback(x.toString(), 1)
                }

                // 1234567000000000 * 10**-9, currency USD, issuer 7C4C8D5B2FDA1D16E9A4F5BB579AC2926C146235 (alice)
                const iou =
                    //6198187654261802496
                    [
                        0x61, 0xd6, 0x04, 0x62, 0xd5, 0x07, 0x7c, 0x86, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x53, 0x44,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x4c, 0x8d, 0x5b, 0x2f, 0xda, 0x1d,
                        0x16, 0xe9, 0xa4, 0xf5, 0xbb, 0x57, 0x9a, 0xc2, 0x92, 0x6c, 0x14, 0x62,
                        0x35,
                    ]

                // as above but value is negative
                const iou_neg =
                    //1586501635834414592
                    [
                        0x61, 0x96, 0x04, 0x62, 0xd5, 0x07, 0x7c, 0x86, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x53, 0x44,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x4c, 0x8d, 0x5b, 0x2f, 0xda, 0x1d,
                        0x16, 0xe9, 0xa4, 0xf5, 0xbb, 0x57, 0x9a, 0xc2, 0x92, 0x6c, 0x14, 0x62,
                        0x35,
                    ]

                // as above but value is 0
                const iou_zero =
                    // 0
                    [
                        0x61, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x53, 0x44,
                        0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0x4c, 0x8d, 0x5b, 0x2f, 0xda, 0x1d,
                        0x16, 0xe9, 0xa4, 0xf5, 0xbb, 0x57, 0x9a, 0xc2, 0x92, 0x6c, 0x14, 0x62,
                        0x35,
                    ]

                // XRP short code 1234567 drops
                const xrp_short =
                    //6198187654261802496
                    [0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x12, 0xd6, 0x87]

                // XRP long code 755898701447 drops
                const xrp_long =
                    //6294584066823682416
                    [0x60, 0x11, 0x40, 0x00, 0x00, 0xaf, 0xff, 0x12, 0xd6, 0x87]

                // XRP negative 1234567 drops
                const xrp_neg =
                    //1586501635834414592
                    [0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x12, 0xd6, 0x87]

                // XRP negative zero
                const xrp_neg_zero =
                    // 0
                    [0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]

                // XRP positive zero
                const xrp_pos_zero =
                    // 0
                    [0x61, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]

                const Hook = (reserved) => {
                    // too small check
                    ASSERT(float_sto_set(Array(7).fill(0)) === NOT_AN_OBJECT)

                    // garbage check
                    ASSERT(float_sto_set(Array(9).fill(0)) === NOT_AN_OBJECT)
                    ASSERT(float_sto_set(Array(8).fill(0)) === 0n)

                    ASSERT(float_sto_set(iou) === 6198187654261802496n)
                    ASSERT(float_sto_set(xrp_short) === 6198187654261802496n)
                    ASSERT(float_sto_set(iou_neg) === 1586501635834414592n)
                    ASSERT(float_sto_set(xrp_neg) === 1586501635834414592n)
                    ASSERT(float_sto_set(xrp_pos_zero) === 0n)
                    ASSERT(float_sto_set(xrp_neg_zero) === 0n)
                    ASSERT(float_sto_set(iou_zero) === 0n)
                    ASSERT(float_sto_set(xrp_long) === 6294584066823682416n)

                    return accept('', 0)
                }

            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_sto_set"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_sto_set"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_float_sum(FeatureBitset features)
    {
        testcase("Test float_sum");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                const float_exponent = (f) => Number(((f >> 54n) & 0xffn) - 97n)
                const ASSERT_EQUAL = (x, y) => {
                    const px = x
                    const py = y
                    let mx = float_mantissa(px)
                    let my = float_mantissa(py)
                    let diffexp = float_exponent(px) - float_exponent(py)
                    if (diffexp === 1) mx *= 10n
                    if (diffexp === -1) my *= 10n
                    let diffman = mx - my
                    if (diffman < 0n) diffman *= -1n
                    if (diffexp < 0) diffexp *= -1
                    if (diffexp > 1 || diffman > 5000000n || mx < 0n || my < 0n) rollback('', 0)
                }
                const Hook = (reserved) => {
                    // 1 + 1 = 2
                    ASSERT_EQUAL(6090866696204910592n, float_sum(float_one(), float_one()))

                    // 1 - 1 = 0
                    ASSERT_EQUAL(0n, float_sum(float_one(), float_negate(float_one())))

                    // 45678 + 0.345678 = 45678.345678
                    ASSERT_EQUAL(
                        6165492124810638528n,
                        float_sum(6165492090242838528n, 6074309077695428608n)
                    )

                    // -151864512641 + 100000000000000000 = 99999848135487359
                    ASSERT_EQUAL(
                        6387097057170171072n,
                        float_sum(1676857706508234512n, 6396111470866104320n)
                    )

                    // auto generated random sums
                    ASSERT_EQUAL(
                        float_sum(
                        95785354843184473n /* -5.713362295774553e-77 */,
                        7607324992379065667n /* 5.248821377668419e+84 */
                        ),
                        7607324992379065667n /* 5.248821377668419e+84 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        1011203427860697296n /* -2.397111329706192e-26 */,
                        7715811566197737722n /* 5.64900413944857e+90 */
                        ),
                        7715811566197737722n /* 5.64900413944857e+90 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        6507979072644559603n /* 4.781210721563379e+23 */,
                        422214339164556094n /* -7.883173446470462e-59 */
                        ),
                        6507979072644559603n /* 4.781210721563379e+23 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        129493221419941559n /* -3.392431853567671e-75 */,
                        6742079437952459317n /* 4.694395406197301e+36 */
                        ),
                        6742079437952459317n /* 4.694395406197301e+36 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        5172806703808250354n /* 2.674331586920946e-51 */,
                        3070396690523275533n /* -7.948943911338253e+88 */
                        ),
                        3070396690523275533n /* -7.948943911338253e+88 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        2440992231195047997n /* -9.048432414980156e+53 */,
                        4937813945440933271n /* 1.868753842869655e-64 */
                        ),
                        2440992231195047996n /* -9.048432414980156e+53 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        7351918685453062372n /* 2.0440935844129e+70 */,
                        6489541496844182832n /* 4.358033430668592e+22 */
                        ),
                        7351918685453062372n /* 2.0440935844129e+70 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        4960621423606196948n /* 6.661833498651348e-63 */,
                        6036716382996689576n /* 0.001892882320224936 */
                        ),
                        6036716382996689576n /* 0.001892882320224936 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        1342689232407435206n /* -9.62374270576839e-8 */,
                        5629833007898276923n /* 9.340672939897915e-26 */
                        ),
                        1342689232407435206n /* -9.62374270576839e-8 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        7557687707019793516n /* 9.65473154684222e+81 */,
                        528084028396448719n /* -5.666471621471183e-53 */
                        ),
                        7557687707019793516n /* 9.65473154684222e+81 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        130151633377050812n /* -4.050843810676924e-75 */,
                        2525286695563827336n /* -3.270904236349576e+58 */
                        ),
                        2525286695563827336n /* -3.270904236349576e+58 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        5051914485221832639n /* 7.88290256687712e-58 */,
                        7518727241611221951n /* 6.723063157234623e+79 */
                        ),
                        7518727241611221951n /* 6.723063157234623e+79 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        3014788764095798870n /* -6.384213012307542e+85 */,
                        7425019819707800346n /* 3.087633801222938e+74 */
                        ),
                        3014788764095767995n /* -6.384213012276667e+85 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        4918950856932792129n /* 1.020063844210497e-65 */,
                        7173510242188034581n /* 3.779635414204949e+60 */
                        ),
                        7173510242188034581n /* 3.779635414204949e+60 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        20028000442705357n /* -2.013601933223373e-81 */,
                        95248745393457140n /* -5.17675284604722e-77 */
                        ),
                        95248946753650462n /* -5.176954206240542e-77 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        5516870225060928024n /* 4.46428115944092e-32 */,
                        7357202055584617194n /* 7.327463715967722e+70 */
                        ),
                        7357202055584617194n /* 7.327463715967722e+70 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        2326103538819088036n /* -2.2461310959121e+47 */,
                        1749360946246242122n /* -1964290826489674 */
                        ),
                        2326103538819088036n /* -2.2461310959121e+47 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        1738010758208819410n /* -862850129854894.6 */,
                        2224610859005732191n /* -8.83984233944816e+41 */
                        ),
                        2224610859005732192n /* -8.83984233944816e+41 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        4869534730307487904n /* 5.647132747352224e-68 */,
                        2166841923565712115n /* -5.114102427874035e+38 */
                        ),
                        2166841923565712115n /* -5.114102427874035e+38 */
                    )
                    ASSERT_EQUAL(
                        float_sum(
                        1054339559322014937n /* -9.504445772059864e-24 */,
                        1389511416678371338n /* -0.0000240273144825857 */
                        ),
                        1389511416678371338n /* -0.0000240273144825857 */
                    )

                    accept('', 0)
                }
            )[test.hook]"];

            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set float_sum"),
                HSFEE);
            env.close();

            env(pay(bob, alice, XRP(1)), M("test float_sum"), fee(XRP(1)));
            env.close();
        }
    }

    void
    test_hook_account(FeatureBitset features)
    {
        testcase("Test hook_account");
        using namespace jtx;

        auto const test = [&](Account alice) -> void {
            Env env{*this, features};

            auto const bob = Account{"bob"};
            env.fund(XRP(10000), alice);
            env.fund(XRP(10000), bob);

            TestHook hook = jswasm[
                R"[test.hook](
                const ASSERT = (x, code) => {
                if (!x) {
                    rollback(x.toString(), code)
                }
                }

                const toHex = (arr) => {
                return arr
                    .map((num) => num.toString(16).padStart(2, '0'))
                    .join('')
                    .toUpperCase()
                }

                const Hook = (arg) => {
                let acc2 = hook_account()
                trace('acc2', acc2, false)
                ASSERT(acc2.length == 20)
                return accept(toHex(acc2), 0)
                }
            )[test.hook]"];

            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set hook_account"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test hook_account"), fee(XRP(1)));

            {
                auto meta = env.meta();

                // ensure hook execution occured
                BEAST_REQUIRE(meta);
                BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

                // ensure there was only one hook execution
                auto const hookExecutions =
                    meta->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(hookExecutions.size() == 1);

                // get the data in the return string of the extention
                auto const tmpRet =
                    hookExecutions[0].getFieldVL(sfHookReturnString);

                // DA: TODO Fix `accept` and `rollback` and remove these lines
                std::string tmpStr(tmpRet.begin(), tmpRet.end());
                auto const tmpBlob = strUnHex(tmpStr);
                Blob const retStr = Blob(tmpBlob->begin(), tmpBlob->end());

                // check that it matches the account id
                BEAST_EXPECT(retStr.size() == 20);
                auto const a = alice.id();
                BEAST_EXPECT(memcmp(retStr.data(), a.data(), 20) == 0);
            }

            // install the same hook bob
            env(ripple::test::jtx::hook(
                    bob, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set hook_account 2"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test hook_account 2"), fee(XRP(1)));

            // there should be two hook executions, the first should be bob's
            // address the second should be alice's
            {
                auto meta = env.meta();

                // ensure hook execution occured
                BEAST_REQUIRE(meta);
                BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

                // ensure there were two hook executions
                auto const hookExecutions =
                    meta->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(hookExecutions.size() == 2);

                {
                    // get the data in the return string of the extention
                    auto const tmpRet =
                        hookExecutions[0].getFieldVL(sfHookReturnString);

                    // DA: TODO Fix `accept` and `rollback` and remove these
                    // lines
                    std::string tmpStr(tmpRet.begin(), tmpRet.end());
                    auto const tmpBlob = strUnHex(tmpStr);
                    Blob const retStr = Blob(tmpBlob->begin(), tmpBlob->end());

                    // check that it matches the account id
                    BEAST_EXPECT(retStr.size() == 20);
                    auto const b = bob.id();
                    BEAST_EXPECT(memcmp(retStr.data(), b.data(), 20) == 0);
                }

                {
                    // get the data in the return string of the extention
                    auto const tmpRet =
                        hookExecutions[1].getFieldVL(sfHookReturnString);

                    // DA: TODO Fix `accept` and `rollback` and remove these
                    // lines
                    std::string tmpStr(tmpRet.begin(), tmpRet.end());
                    auto const tmpBlob = strUnHex(tmpStr);
                    Blob const retStr = Blob(tmpBlob->begin(), tmpBlob->end());

                    // check that it matches the account id
                    BEAST_EXPECT(retStr.size() == 20);
                    auto const a = alice.id();
                    BEAST_EXPECT(memcmp(retStr.data(), a.data(), 20) == 0);
                }
            }
        };

        test(Account{"alice"});
        test(Account{"cho"});
    }

    void
    test_hook_again(FeatureBitset features)
    {
        testcase("Test hook_again");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const PREREQUISITE_NOT_MET = -9
            const ALREADY_SET = -8

            const Hook = (r) => {
            if (r > 0) {
                if (hook_again() != PREREQUISITE_NOT_MET) return rollback('', 253)

                return accept('', 1)
            }

            if (hook_again() != 1) return rollback('', 254)

            if (hook_again() != ALREADY_SET) return rollback('', 255)
            return accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set hook_again"),
            HSFEE);
        env.close();

        env(pay(bob, alice, XRP(1)), M("test hook_again"), fee(XRP(1)));
        env.close();

        auto meta = env.meta();

        // ensure hook execution occured
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        // ensure there were two executions
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 2);

        // get the data in the return code of the execution
        bool const fixV2 = env.current()->rules().enabled(fixXahauV2);
        if (fixV2)
        {
            BEAST_EXPECT(hookExecutions[0].getFieldU32(sfFlags) == 5);
            BEAST_EXPECT(hookExecutions[1].getFieldU32(sfFlags) == 0);
        }

        BEAST_EXPECT(hookExecutions[0].getFieldU64(sfHookReturnCode) == 0);
        BEAST_EXPECT(hookExecutions[1].getFieldU64(sfHookReturnCode) == 1);

        // RH TODO: test hook_again on a weak execution not following a strong
        // execution to make sure it fails
    }

    void
    test_hook_hash(FeatureBitset features)
    {
        testcase("Test hook_hash");
        using namespace jtx;

        auto const test = [&](Account alice) -> void {
            Env env{*this, features};
            // Env env{*this, envconfig(), features, nullptr,
            //     beast::severities::kTrace
            // };

            auto const bob = Account{"bob"};
            env.fund(XRP(10000), alice);
            env.fund(XRP(10000), bob);

            TestHook hook = jswasm[R"[test.hook](
                const ASSERT = (x, code) => {
                if (!x) {
                    rollback(x.toString(), code)
                }
                }

                const toHex = (arr) => {
                return arr
                    .map((num) => num.toString(16).padStart(2, '0'))
                    .join('')
                    .toUpperCase()
                }

                const Hook = (arg) => {
                const hash = hook_hash(-1)
                ASSERT(hash.length == 32, 0)
                trace('hash', hash, false)

                // return the hash as the return string
                return accept(toHex(hash), 0)
                }
            )[test.hook]"];

            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set hook_hash"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test hook_hash"), fee(XRP(1)));

            {
                auto meta = env.meta();

                // ensure hook execution occured
                BEAST_REQUIRE(meta);
                BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

                // ensure there was only one hook execution
                auto const hookExecutions =
                    meta->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(hookExecutions.size() == 1);

                // get the data in the return string of the extention
                auto const tmpRet =
                    hookExecutions[0].getFieldVL(sfHookReturnString);

                // DA: TODO Fix `accept` and `rollback` and remove these lines
                std::string tmpStr(tmpRet.begin(), tmpRet.end());
                auto const tmpBlob = strUnHex(tmpStr);
                Blob const retStr = Blob(tmpBlob->begin(), tmpBlob->end());

                // check that it matches the hook hash
                BEAST_EXPECT(retStr.size() == 32);

                auto const hash = hookExecutions[0].getFieldH256(sfHookHash);
                BEAST_EXPECT(memcmp(hash.data(), retStr.data(), 32) == 0);
            }

            TestHook hook2 = jswasm[R"[test.hook](
                const ASSERT = (x, code) => {
                if (!x) {
                    rollback(x.toString(), code)
                }
                }

                const toHex = (arr) => {
                return arr
                    .map((num) => num.toString(16).padStart(2, '0'))
                    .join('')
                    .toUpperCase()
                }

                const Hook = (arg) => {
                const hash = hook_hash(-1)
                ASSERT(hash.length == 32, 0)
                trace('hash', hash, false)

                // return the hash as the return string
                return accept(toHex(hash), 2)
                }
            )[test.hook]"];

            // install a slightly different hook on bob
            env(ripple::test::jtx::hook(
                    bob, {{hsov1(hook2, 1, HSDROPS, overrideFlag)}}, 0),
                M("set hook_hash 2"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test hook_hash 2"), fee(XRP(1)));

            // there should be two hook executions, the first should have bob's
            // hook hash the second should have alice's hook hash
            {
                auto meta = env.meta();

                // ensure hook execution occured
                BEAST_REQUIRE(meta);
                BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

                // ensure there was only one hook execution
                auto const hookExecutions =
                    meta->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(hookExecutions.size() == 2);

                // get the data in the return string of the extention
                auto const tmpRet1 =
                    hookExecutions[0].getFieldVL(sfHookReturnString);

                // DA: TODO Fix `accept` and `rollback` and remove these lines
                std::string tmpStr1(tmpRet1.begin(), tmpRet1.end());
                auto const tmpBlob1 = strUnHex(tmpStr1);
                Blob const retStr1 = Blob(tmpBlob1->begin(), tmpBlob1->end());

                // check that it matches the hook hash
                BEAST_EXPECT(retStr1.size() == 32);

                auto const hash1 = hookExecutions[0].getFieldH256(sfHookHash);
                BEAST_EXPECT(memcmp(hash1.data(), retStr1.data(), 32) == 0);

                // get the data in the return string of the extention
                auto const tmpRet2 =
                    hookExecutions[1].getFieldVL(sfHookReturnString);

                // DA: TODO Fix `accept` and `rollback` and remove these lines
                std::string tmpStr2(tmpRet2.begin(), tmpRet2.end());
                auto const tmpBlob2 = strUnHex(tmpStr2);
                Blob const retStr2 = Blob(tmpBlob2->begin(), tmpBlob2->end());

                // check that it matches the hook hash
                BEAST_EXPECT(retStr2.size() == 32);

                auto const hash2 = hookExecutions[1].getFieldH256(sfHookHash);
                BEAST_EXPECT(memcmp(hash2.data(), retStr2.data(), 32) == 0);

                // make sure they're not the same
                BEAST_EXPECT(memcmp(hash1.data(), hash2.data(), 32) != 0);

                // compute the hashes
                auto computedHash2 = ripple::sha512Half_s(
                    ripple::Slice(hook.data(), hook.size()));

                auto computedHash1 = ripple::sha512Half_s(
                    ripple::Slice(hook2.data(), hook2.size()));

                // ensure the computed hashes match
                BEAST_EXPECT(computedHash1 == hash1);
                BEAST_EXPECT(computedHash2 == hash2);
            }
        };

        test(Account{"alice"});
    }

    void
    test_hook_param(FeatureBitset features)
    {
        testcase("Test hook_param");
        using namespace jtx;
        Env env{*this, features};
        // Env env{*this, envconfig(), features, nullptr,
        //     beast::severities::kTrace
        // };

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x) => {
            if (!x) {
                rollback(x.toString(), 0)
            }
            }
            var DOESNT_EXIST = -5
            var INVALID_ARGUMENT = -7
            var names = [
            '706172616d30',
            '706172616d31',
            '706172616d32',
            '706172616d33',
            '706172616d34',
            '706172616d35',
            '706172616d36',
            '706172616d37',
            '706172616d38',
            '706172616d39',
            '706172616d3130',
            '706172616d3131',
            '706172616d3132',
            '706172616d3133',
            '706172616d3134',
            '706172616d3135',
            ]
            var values = [
            [118, 97, 108, 117, 101, 48],
            [118, 97, 108, 117, 101, 49],
            [118, 97, 108, 117, 101, 50],
            [118, 97, 108, 117, 101, 51],
            [118, 97, 108, 117, 101, 52],
            [118, 97, 108, 117, 101, 53],
            [118, 97, 108, 117, 101, 54],
            [118, 97, 108, 117, 101, 55],
            [118, 97, 108, 117, 101, 56],
            [118, 97, 108, 117, 101, 57],
            [118, 97, 108, 117, 101, 49, 48],
            [118, 97, 108, 117, 101, 49, 49],
            [118, 97, 108, 117, 101, 49, 50],
            [118, 97, 108, 117, 101, 49, 51],
            [118, 97, 108, 117, 101, 49, 52],
            [118, 97, 108, 117, 101, 49, 53],
            ]
            var Hook = (arg) => {
            ASSERT(hook_param(0) === INVALID_ARGUMENT)
            ASSERT(hook_param('') === INVALID_ARGUMENT)
            ASSERT(hook_param([]) === INVALID_ARGUMENT)
            ASSERT(hook_param(void 0) === INVALID_ARGUMENT)
            ASSERT(hook_param(null) === INVALID_ARGUMENT)
            ASSERT(hook_param([0]) === DOESNT_EXIST)
            for (let i = 0; i < 16; ++i) {
                const s = 6 + (i < 10 ? 0 : 1)
                const buf = hook_param(names[i])
                ASSERT(buf.length === s)
                ASSERT(
                buf[0] === 118 &&
                    buf[1] === 97 &&
                    buf[2] === 108 &&
                    buf[3] === 117 &&
                    buf[4] === 101
                )
                ASSERT(buf[buf.length - 1] === values[i][buf.length - 1])
                ASSERT(buf[buf.length - 2] === values[i][buf.length - 2])
            }
            return accept('', 0)
            }
        )[test.hook]"];

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        Json::Value iv;
        iv[jss::CreateCode] = strHex(hook);
        iv[jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        iv[jss::Fee] = "99000";
        iv[jss::HookApiVersion] = 1U;
        iv[jss::HookNamespace] = to_string(uint256{beast::zero});
        Json::Value params{Json::arrayValue};
        for (uint32_t i = 0; i < 16; ++i)
        {
            Json::Value pv;
            Json::Value piv;
            piv[jss::HookParameterName] = strHex("param" + std::to_string(i));
            piv[jss::HookParameterValue] = strHex("value" + std::to_string(i));
            pv[jss::HookParameter] = piv;
            params[i] = pv;
        }
        iv[jss::HookParameters] = params;
        jv[jss::Hooks][0U][jss::Hook] = iv;
        env(jv, M("set hook_param"), HSFEE, ter(tesSUCCESS));
        env.close();

        // invoke
        env(pay(bob, alice, XRP(1)), M("test hook_param"), fee(XRP(1)));
        env.close();
    }

    void
    test_hook_param_set(FeatureBitset features)
    {
        testcase("Test hook_param_set");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook checker_wasm = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            const DOESNT_EXIST = -5

            const names = ['param0', 'param1', 'param2', 'param3']

            function toHex(str) {
            let hexString = ''
            for (let i = 0; i < str.length; i++) {
                hexString += str.charCodeAt(i).toString(16).padStart(2, '0')
            }
            return hexString.toUpperCase()
            }

            const Hook = (arg) => {
            // TEQU: https://github.com/Xahau/xahaud/issues/444
            // ASSERT(hook_param(toHex('checker')) == DOESNT_EXIST)

            // this entry should havebeen added by the setter
            let buf = hook_param(toHex('hello'))
            ASSERT(buf.length === 5)
            ASSERT(
                buf[0] == 'w'.charCodeAt(0) &&
                buf[1] == 'o'.charCodeAt(0) &&
                buf[2] == 'r'.charCodeAt(0) &&
                buf[3] == 'l'.charCodeAt(0) &&
                buf[4] == 'd'.charCodeAt(0)
            )

            // these pre-existing entries should be modified by the setter
            for (let i = 0; i < 4; ++i) {
                buf = hook_param(toHex(names[i]))
                ASSERT(buf.length == 6)
                ASSERT(
                buf[0] == 'v'.charCodeAt(0) &&
                    buf[1] == 'a'.charCodeAt(0) &&
                    buf[2] == 'l'.charCodeAt(0) &&
                    buf[3] == 'u'.charCodeAt(0) &&
                    buf[4] == 'e'.charCodeAt(0) &&
                    buf[5] == '0'.charCodeAt(0) + i
                )
            }

            return accept('', 0)
            }
        )[test.hook]"];

        TestHook setter_wasm = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            function toHex(str) {
            let hexString = ''
            for (let i = 0; i < str.length; i++) {
                hexString += str.charCodeAt(i).toString(16).padStart(2, '0')
            }
            return hexString.toUpperCase()
            }

            const names = ['param0', 'param1', 'param2', 'param3']
            const values = ['value0', 'value1', 'value2', 'value3']

            const Hook = (arg) => {
            const checker_hash = hook_param(toHex('checker'))
            ASSERT(checker_hash.length === 32)

            for (let i = 0; i < 4; ++i) {
                ASSERT(
                hook_param_set(toHex(values[i]), toHex(names[i]), checker_hash) ==
                    6
                )
            }

            // "delete" the checker entry" for when the checker runs
            // TEQU: https://github.com/Xahau/xahaud/issues/444
            // ASSERT(hook_param_set('', toHex('checker'), checker_hash) == 0)

            // add a parameter that did not previously exist
            ASSERT(hook_param_set(toHex('world'), toHex('hello'), checker_hash) == 5)

            // ensure this hook's parameters did not change
            for (let i = 0; i < 4; ++i) {
                const buf = hook_param(toHex(names[i]))
                ASSERT(buf.length == 6)

                ASSERT(
                buf[0] == 'v'.charCodeAt(0) &&
                    buf[1] == 'a'.charCodeAt(0) &&
                    buf[2] == 'l'.charCodeAt(0) &&
                    buf[3] == 'u'.charCodeAt(0) &&
                    buf[4] == 'e'.charCodeAt(0) &&
                    buf[5] == '0'.charCodeAt(0)
                )
            }

            return accept('', 0)
            }
        )[test.hook]"];

        HASH_WASM(checker);

        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::SetHook;
        jv[jss::Flags] = 0;
        jv[jss::Hooks] = Json::Value{Json::arrayValue};

        Json::Value iv;
        iv[jss::CreateCode] = strHex(setter_wasm);
        iv[jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        iv[jss::Fee] = "99000";
        iv[jss::HookApiVersion] = 1U;
        iv[jss::HookNamespace] = to_string(uint256{beast::zero});

        Json::Value checkerpv;
        {
            Json::Value piv;
            piv[jss::HookParameterName] = strHex(std::string("checker"));
            piv[jss::HookParameterValue] = checker_hash_str;
            checkerpv[jss::HookParameter] = piv;
        }

        Json::Value params{Json::arrayValue};
        for (uint32_t i = 0; i < 4; ++i)
        {
            Json::Value pv;
            Json::Value piv;
            piv[jss::HookParameterName] = strHex("param" + std::to_string(i));
            piv[jss::HookParameterValue] = strHex(std::string("value0"));
            pv[jss::HookParameter] = piv;
            params[i] = pv;
        }
        params[4U] = checkerpv;

        iv[jss::HookParameters] = params;
        jv[jss::Hooks][0U][jss::Hook] = iv;

        {
            iv[jss::CreateCode] = strHex(checker_wasm);
            Json::Value params{Json::arrayValue};
            params[0U] = checkerpv;
            iv[jss::HookParameters] = params;
            jv[jss::Hooks][3U][jss::Hook] = iv;
            jv[jss::Hooks][1U][jss::Hook] = Json::objectValue;
            jv[jss::Hooks][2U][jss::Hook] = Json::objectValue;
        }

        env(jv, M("set hook_param_set"), HSFEE, ter(tesSUCCESS));
        env.close();

        // invoke
        env(pay(bob, alice, XRP(1)), M("test hook_param_set"), fee(XRP(1)));
        env.close();
    }

    void
    test_hook_pos(FeatureBitset features)
    {
        testcase("Test hook_pos");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const Hook = (arg) => {
            return accept('', hook_pos())
            }
        )[test.hook]"];

        // install the hook on alice in all four spots
        env(ripple::test::jtx::hook(
                alice,
                {{hsov1(hook, 1, HSDROPS),
                  hsov1(hook, 1, HSDROPS),
                  hsov1(hook, 1, HSDROPS),
                  hsov1(hook, 1, HSDROPS)}},
                0),
            M("set hook_pos"),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // invoke the hooks
        env(pay(bob, alice, XRP(1)), M("test hook_pos"), fee(XRP(1)));
        env.close();

        auto meta = env.meta();

        // ensure hook execution occured
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        // ensure there was four hook executions
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 4);

        // get the data in the return code of the execution
        BEAST_EXPECT(hookExecutions[0].getFieldU64(sfHookReturnCode) == 0);
        BEAST_EXPECT(hookExecutions[1].getFieldU64(sfHookReturnCode) == 1);
        BEAST_EXPECT(hookExecutions[2].getFieldU64(sfHookReturnCode) == 2);
        BEAST_EXPECT(hookExecutions[3].getFieldU64(sfHookReturnCode) == 3);
    }

    void
    test_hook_skip(FeatureBitset features)
    {
        testcase("Test hook_skip");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook skip_wasm = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            const DOESNT_EXIST = -5
            const INVALID_ARGUMENT = -7
            const sfInvoiceID = 0x50011

            const Hook = (arg) => {
            // bounds checks
            ASSERT(hook_skip('00'.repeat(31), 1) === INVALID_ARGUMENT)
            ASSERT(hook_skip('00'.repeat(31), 2) === INVALID_ARGUMENT)
            ASSERT(hook_skip('00'.repeat(33), 1) === INVALID_ARGUMENT)
            ASSERT(hook_skip('00'.repeat(33), 2) === INVALID_ARGUMENT)

            // garbage check
            ASSERT(hook_skip('00'.repeat(32), 0) === DOESNT_EXIST)
            ASSERT(hook_skip('00'.repeat(32), 1) === DOESNT_EXIST)
            ASSERT(hook_skip('00'.repeat(32), 2) === INVALID_ARGUMENT)

            // the hook to skip is passed in by invoice id
            const skip = otxn_field(sfInvoiceID)
            ASSERT(skip.length === 32)

            // get this hook's hash
            const hash = hook_hash(hook_pos())
            ASSERT(hash.length === 32)

            // to test if the "remove" function works in the api we will add this hook hash itself and then
            // remove it again. Therefore if the hook is placed at positions 0 and 3, the one at 3 should still
            // run
            ASSERT(hook_skip(hash, 1) === DOESNT_EXIST)
            ASSERT(hook_skip(hash, 0) === 1)
            ASSERT(hook_skip(hash, 1) === 1)

            // finally skip the hook hash indicated by invoice id
            ASSERT(hook_skip(skip, 0))

            return accept('', hook_pos())
            }
        )[test.hook]"];

        TestHook pos_wasm = jswasm[R"[test.hook](
            const Hook = (arg) => {
            return accept('', 255)
            }
        )[test.hook]"];

        HASH_WASM(pos);

        // install the hook on alice in one places
        env(ripple::test::jtx::hook(
                alice,
                {{hsov1(skip_wasm, 1, HSDROPS),
                  hsov1(pos_wasm, 1, HSDROPS),
                  hsov1(pos_wasm, 1, HSDROPS),
                  hsov1(skip_wasm, 1, HSDROPS)}},
                0),
            M("set hook_skip"),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // invoke the hooks
        {
            Json::Value json = pay(bob, alice, XRP(1));
            json[jss::InvoiceID] = pos_hash_str;
            env(json, fee(XRP(1)), M("test hook_skip"), ter(tesSUCCESS));
            env.close();
        }

        auto meta = env.meta();

        // ensure hook execution occured
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        // ensure there was four hook executions
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 2);

        // get the data in the return code of the execution
        BEAST_EXPECT(hookExecutions[0].getFieldU64(sfHookReturnCode) == 0);
        BEAST_EXPECT(hookExecutions[1].getFieldU64(sfHookReturnCode) == 3);
    }

    void
    test_ledger_last_hash(FeatureBitset features)
    {
        testcase("Test ledger_last_hash");
        using namespace jtx;

        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            const toHex = (arr) => {
            return arr
                .map((num) => num.toString(16).padStart(2, '0'))
                .join('')
                .toUpperCase()
            }

            const Hook = (arg) => {
            const hash = ledger_last_hash()
            ASSERT(hash.length === 32)

            // return the hash
            return accept(toHex(hash), 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set ledger_last_hash"),
            HSFEE);
        env.close();

        for (uint32_t i = 0; i < 3; ++i)
        {
            auto const llh =
                env.app().getLedgerMaster().getClosedLedger()->info().hash;

            env(pay(bob, alice, XRP(1)),
                M("test ledger_last_hash"),
                fee(XRP(1)));

            auto meta = env.meta();

            // ensure hook execution occured
            BEAST_REQUIRE(meta);
            BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

            // ensure there was only one hook execution
            auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(hookExecutions.size() == 1);

            // get the data in the return string of the extention
            auto const tmpRet =
                hookExecutions[0].getFieldVL(sfHookReturnString);

            // DA: TODO Fix `accept` and `rollback` and remove these lines
            std::string tmpStr(tmpRet.begin(), tmpRet.end());
            auto const tmpBlob = strUnHex(tmpStr);
            Blob const retStr = Blob(tmpBlob->begin(), tmpBlob->end());

            // check that it matches the expected size (32 bytes)
            BEAST_EXPECT(retStr.size() == 32);

            BEAST_EXPECT(llh == uint256::fromVoid(retStr.data()));
        }
    }

    void
    test_ledger_last_time(FeatureBitset features)
    {
        testcase("Test ledger_last_time");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const Hook = (arg) => {
            return accept('', ledger_last_time())
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set ledger_last_time"),
            HSFEE);
        env.close();

        // invoke the hook a few times
        for (uint32_t i = 0; i < 3; ++i)
        {
            int64_t llc = std::chrono::duration_cast<std::chrono::seconds>(
                              env.app()
                                  .getLedgerMaster()
                                  .getCurrentLedger()
                                  ->info()
                                  .parentCloseTime.time_since_epoch())
                              .count();

            env(pay(bob, alice, XRP(1)),
                M("test ledger_last_time"),
                fee(XRP(1)));
            env.close();
            {
                auto meta = env.meta();

                // ensure hook execution occured
                BEAST_REQUIRE(meta);
                BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

                // ensure there was only one hook execution
                auto const hookExecutions =
                    meta->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(hookExecutions.size() == 1);

                // get the data in the return code of the execution
                auto const rc = hookExecutions[0].getFieldU64(sfHookReturnCode);

                // check that it matches the last ledger seq number
                BEAST_EXPECT(llc == rc);
            }
        }
    }

    void
    test_ledger_nonce(FeatureBitset features)
    {
        testcase("Test ledger_nonce");
        using namespace jtx;

        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            const toHex = (arr) => {
            return arr
                .map((num) => num.toString(16).padStart(2, '0'))
                .join('')
                .toUpperCase()
            }

            const Hook = (arg) => {
            // Test out of bounds check
            let nonce = ledger_nonce()
            ASSERT(nonce.length == 32)
            nonce = nonce.concat(ledger_nonce())
            ASSERT(nonce.length == 64)

            // return the two nonces as the return string
            accept(toHex(nonce), 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set ledger_nonce"),
            HSFEE);
        env.close();

        // invoke the hook
        auto const seq =
            env.app().getLedgerMaster().getCurrentLedger()->info().seq;
        auto const llc = env.app()
                             .getLedgerMaster()
                             .getCurrentLedger()
                             ->info()
                             .parentCloseTime.time_since_epoch()
                             .count();
        auto const llh =
            env.app().getLedgerMaster().getCurrentLedger()->info().hash;

        env(pay(bob, alice, XRP(1)), M("test ledger_nonce"), fee(XRP(1)));
        auto const txid = env.txid();

        auto meta = env.meta();

        // ensure hook execution occured
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        // ensure there was only one hook execution
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        // get the data in the return string of the extention
        auto const tmpRet = hookExecutions[0].getFieldVL(sfHookReturnString);

        // DA: TODO Fix `accept` and `rollback` and remove these lines
        std::string tmpStr(tmpRet.begin(), tmpRet.end());
        auto const tmpBlob = strUnHex(tmpStr);
        Blob const retStr = Blob(tmpBlob->begin(), tmpBlob->end());

        // check that it matches the expected size (two nonces = 64 bytes)
        BEAST_EXPECT(retStr.size() == 64);

        auto const computed_hash_1 = ripple::sha512Half(
            ripple::HashPrefix::hookNonce,
            seq,
            llc,
            llh,
            txid,
            (uint16_t)0UL,
            alice.id());
        auto const computed_hash_2 = ripple::sha512Half(
            ripple::HashPrefix::hookNonce,
            seq,
            llc,
            llh,
            txid,
            (uint16_t)1UL,  // second nonce
            alice.id());

        BEAST_EXPECT(computed_hash_1 == uint256::fromVoid(retStr.data()));
        BEAST_EXPECT(computed_hash_2 == uint256::fromVoid(retStr.data() + 32));
    }

    void
    test_ledger_seq(FeatureBitset features)
    {
        testcase("Test ledger_seq");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const Hook = (arg) => {
            return accept('', ledger_seq())
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set ledger_seq"),
            HSFEE);
        env.close();

        // invoke the hook a few times
        for (uint32_t i = 0; i < 3; ++i)
        {
            env(pay(bob, alice, XRP(1)), M("test ledger_seq"), fee(XRP(1)));
            env.close();
            {
                auto meta = env.meta();

                // ensure hook execution occured
                BEAST_REQUIRE(meta);
                BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

                // ensure there was only one hook execution
                auto const hookExecutions =
                    meta->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(hookExecutions.size() == 1);

                // get the data in the return code of the execution
                auto const rc = hookExecutions[0].getFieldU64(sfHookReturnCode);

                // check that it matches the last ledger seq number
                BEAST_EXPECT(
                    env.app().getLedgerMaster().getClosedLedger()->info().seq ==
                    rc);
            }
        }
    }

    void
    test_otxn_id(FeatureBitset features)
    {
        testcase("Test otxn_id");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var Hook = (arg) => {
            const id = otxn_id(0)
            ASSERT(id.length === 32, 1)
            ASSERT(otxn_slot(1) === 1, 2)
            const buf = slot(1)
            const size = buf.length
            ASSERT(size > 0, 3)
            buf.unshift(0)
            buf.unshift(78)
            buf.unshift(88)
            buf.unshift(84)
            const hash = util_sha512h(buf)
            ASSERT(hash.length === 32, 4)
            for (let i = 0; i < 32; ++i) ASSERT(hash[i] === id[i], 5)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set otxn_id"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test otxn_id"), fee(XRP(1)));
    }

    void
    test_otxn_slot(FeatureBitset features)
    {
        testcase("Test otxn_slot");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var INVALID_ARGUMENT = -7
            var NO_FREE_SLOTS = -6
            var Hook = (arg) => {
            ASSERT(otxn_slot(256) === INVALID_ARGUMENT, 1)
            ASSERT(otxn_slot(1) === 1, 2)
            const id = otxn_id(0)
            ASSERT(id.length === 32, 1)
            ASSERT(otxn_slot(1) === 1, 3)
            const buf = slot(1)
            const size = buf.length
            ASSERT(size > 0, 4)
            buf.unshift(0)
            buf.unshift(78)
            buf.unshift(88)
            buf.unshift(84)
            const hash = util_sha512h(buf)
            ASSERT(hash.length === 32, 5)
            for (let i = 0; i < 32; ++i) ASSERT(hash[i] === id[i], 6)
            for (let i = 0; i < 254; ++i) ASSERT(otxn_slot(0) > 0, 7)
            ASSERT(otxn_slot(0) == NO_FREE_SLOTS, 8)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set otxn_slot"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test otxn_slot"), fee(XRP(1)));
    }

    void
    test_otxn_type(FeatureBitset features)
    {
        testcase("Test otxn_type");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var sfTransactionType = 65538
            var Hook = (arg) => {
            ASSERT(otxn_slot(1) === 1, 1)
            ASSERT(slot_subfield(1, sfTransactionType, 2) === 2, 2)
            const tt = slot(2, true)
            ASSERT(tt === otxn_type(), 3)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set otxn_type"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test otxn_type"), fee(XRP(1)));

        // invoke it another way
        Json::Value jv;
        jv[jss::Account] = alice.human();
        jv[jss::TransactionType] = jss::AccountSet;
        jv[jss::Flags] = 0;

        // invoke the hook
        env(jv, M("test otxn_type 2"), fee(XRP(1)));

        // RH TODO: test behaviour on emit failure
    }

    void
    test_otxn_param(FeatureBitset features)
    {
        testcase("Test otxn_param");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var INVALID_ARGUMENT = -7
            var names = [
            '706172616d30',
            '706172616d31',
            '706172616d32',
            '706172616d33',
            '706172616d34',
            '706172616d35',
            '706172616d36',
            '706172616d37',
            '706172616d38',
            '706172616d39',
            '706172616d3130',
            '706172616d3131',
            '706172616d3132',
            '706172616d3133',
            '706172616d3134',
            '706172616d3135',
            ]
            var values = [
            [118, 97, 108, 117, 101, 48],
            [118, 97, 108, 117, 101, 49],
            [118, 97, 108, 117, 101, 50],
            [118, 97, 108, 117, 101, 51],
            [118, 97, 108, 117, 101, 52],
            [118, 97, 108, 117, 101, 53],
            [118, 97, 108, 117, 101, 54],
            [118, 97, 108, 117, 101, 55],
            [118, 97, 108, 117, 101, 56],
            [118, 97, 108, 117, 101, 57],
            [118, 97, 108, 117, 101, 49, 48],
            [118, 97, 108, 117, 101, 49, 49],
            [118, 97, 108, 117, 101, 49, 50],
            [118, 97, 108, 117, 101, 49, 51],
            [118, 97, 108, 117, 101, 49, 52],
            [118, 97, 108, 117, 101, 49, 53],
            ]
            var ba = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1,
            ]
            var a = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1,
            ]
            var Hook = (arg) => {
            ASSERT(otxn_param([]) === INVALID_ARGUMENT, 1)
            ASSERT(otxn_param('') === INVALID_ARGUMENT, 2)
            ASSERT(otxn_param(ba) === INVALID_ARGUMENT, 3)
            ASSERT(otxn_param(a) === DOESNT_EXIST, 4)
            for (let i = 0; i < 16; ++i) {
                const s = 6 + (i < 10 ? 0 : 1)
                const buf = otxn_param(names[i])
                ASSERT(buf.length === s, 5)
                ASSERT(
                buf[0] === 118 &&
                    buf[1] === 97 &&
                    buf[2] === 108 &&
                    buf[3] === 117 &&
                    buf[4] === 101,
                6
                )
                ASSERT(buf[buf.length - 1] === values[i][buf.length - 1], 6)
                ASSERT(buf[buf.length - 2] === values[i][buf.length - 2], 7)
            }
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set otxn_param"),
            HSFEE);
        env.close();

        // invoke
        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = bob.human();
        invoke[jss::Destination] = alice.human();

        Json::Value params{Json::arrayValue};
        for (uint32_t i = 0; i < 16; ++i)
        {
            Json::Value pv;
            Json::Value piv;
            piv[jss::HookParameterName] = strHex("param" + std::to_string(i));
            piv[jss::HookParameterValue] = strHex("value" + std::to_string(i));
            pv[jss::HookParameter] = piv;
            params[i] = pv;
        }
        invoke[jss::HookParameters] = params;

        env(invoke, M("test otxn_param"), fee(XRP(1)));
        env.close();
    }

    void
    test_otxn_json(FeatureBitset features)
    {
        testcase("Test otxn_json");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            function arrayEqual(arr1, arr2) {
            if (arr1.length !== arr2.length) return false
            for (let i = 0; i < arr1.length; i++) {
                if (arr1[i] !== arr2[i]) return false
            }
            return true
            }
            var Hook = (arg) => {
            const txn = otxn_json()
            ASSERT(typeof txn === 'object', 1)
            ASSERT(txn.TransactionType === 'Invoke', 2)
            ASSERT(arrayEqual(util_accid(txn.Destination), hook_account()), 4)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set otxn_json"),
            HSFEE);
        env.close();

        // invoke
        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = bob.human();
        invoke[jss::Destination] = alice.human();

        env(invoke, M("test otxn_json"), fee(XRP(1)));
        env.close();
    }

    void
    test_slot(FeatureBitset features)
    {
        testcase("Test slot");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var INVALID_ARGUMENT = -7
            var DOESNT_EXIST = -5
            var TOO_BIG = -3
            var KEYLET_ACCOUNT = 3
            var sfBalance = 393218
            var Hook = (arg) => {
            ASSERT(slot(null) === INVALID_ARGUMENT, 1)
            ASSERT(slot(void 0) === INVALID_ARGUMENT, 1)
            ASSERT(slot('') === INVALID_ARGUMENT, 1)
            ASSERT(slot(0, null) === INVALID_ARGUMENT, 2)
            ASSERT(slot(0) === DOESNT_EXIST, 3)
            const acc = hook_account()
            ASSERT(20 === acc.length, 4)
            const kl = util_keylet(KEYLET_ACCOUNT, acc)
            ASSERT(34 === kl.length, 5)
            const slot_no = slot_set(kl, 0)
            ASSERT(slot_no > 0, 6)
            const size = slot_size(slot_no)
            ASSERT(size > 0, 7)
            ASSERT(slot(slot_no, true) === TOO_BIG, 8)
            const buf = slot(slot_no)
            ASSERT(buf.length == size, 9)
            ASSERT(sto_validate(buf) === 1, 10)
            ASSERT(sto_subfield(buf, sfBalance) > 0, 11)
            ASSERT(slot_subfield(slot_no, sfBalance, 200) === 200, 12)
            ASSERT(slot(200, true) > 0, 13)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot"), fee(XRP(1)));
    }

    void
    test_slot_clear(FeatureBitset features)
    {
        testcase("Test slot_clear");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var Hook = (arg) => {
            ASSERT(otxn_slot(1) === 1, 1)
            ASSERT(slot_size(1) > 0, 2)
            ASSERT(slot_clear(1) === 1, 3)
            ASSERT(slot_size(1) === DOESNT_EXIST, 4)
            ASSERT(slot_clear(1) == DOESNT_EXIST, 5)
            ASSERT(slot_clear(10) == DOESNT_EXIST, 6)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_clear"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_clear"), fee(XRP(1)));
    }

    void
    test_slot_count(FeatureBitset features)
    {
        testcase("Test slot_count");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var NOT_AN_ARRAY = -22
            var sfMemos = 983049
            var Hook = (arg) => {
            ASSERT(otxn_slot(1) === 1, 1)
            ASSERT(slot_size(1) > 0, 2)
            ASSERT(slot_count(1) === NOT_AN_ARRAY, 3)
            ASSERT(slot_count(0) === DOESNT_EXIST, 4)
            ASSERT(slot_subfield(1, sfMemos, 1) === 1, 5)
            ASSERT(slot_size(1) > 0, 6)
            ASSERT(slot_count(1) === 1, 7)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_count"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_count"), fee(XRP(1)));
    }

    void
    test_slot_float(FeatureBitset features)
    {
        testcase("Test slot_float");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var NOT_AN_AMOUNT = -32
            var sfFee = 393224
            var Hook = (arg) => {
            ASSERT(otxn_slot(1) === 1, 1)
            ASSERT(slot_size(1) > 0, 2)
            ASSERT(slot_subfield(1, sfFee, 2) === 2, 3)
            ASSERT(slot_size(2) > 0, 4)
            ASSERT(slot_float(0) === DOESNT_EXIST, 5)
            ASSERT(slot_float(1) === NOT_AN_AMOUNT, 6)
            const xfl = slot_float(2)
            ASSERT(xfl > 0, 7)
            ASSERT(float_int(xfl, 6, 0) === 1e6, 8)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_float"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_float"), fee(XRP(1)));
    }

    void
    test_slot_set(FeatureBitset features)
    {
        testcase("Test slot_set");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var INVALID_ARGUMENT = -7
            var kl_sk = [
            0, 104, 180, 151, 154, 54, 205, 199, 243, 211, 213, 195, 26, 78, 174, 42, 199,
            215, 32, 157, 218, 135, 117, 136, 185, 175, 198, 103, 153, 105, 42, 176, 214,
            107,
            ]
            var a31 = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1,
            ]
            var a33 = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1,
            ]
            var a35 = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1,
            ]
            var a34 = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1,
            ]
            var Hook = (arg) => {
            const kl_zero = [
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1,
            ]
            ASSERT(slot_set([], 0) === INVALID_ARGUMENT, 1)
            ASSERT(slot_set(a34, void 0) === INVALID_ARGUMENT, 1)
            ASSERT(slot_set(a34, -1) === INVALID_ARGUMENT, 1)
            ASSERT(slot_set(a31, 0) === INVALID_ARGUMENT, 2)
            ASSERT(slot_set(a33, 0) === INVALID_ARGUMENT, 3)
            ASSERT(slot_set(a35, 0) === INVALID_ARGUMENT, 4)
            ASSERT(slot_set(a34, 256) === INVALID_ARGUMENT, 5)
            ASSERT(slot_set(kl_zero, 0) === DOESNT_EXIST, 6)
            kl_zero[0] = 1
            ASSERT(slot_set(kl_zero, 0) === DOESNT_EXIST, 7)
            ASSERT(slot_size(1) === DOESNT_EXIST, 8)
            ASSERT(slot_set(kl_sk, 0) > 0, 9)
            ASSERT(slot_size(1) > 0, 10)
            for (let i = 1; i < 255; ++i) ASSERT(slot_set(kl_sk, 0) > 0, 10)
            ASSERT(slot_set(kl_sk, 0), 11)
            ASSERT(slot_set(kl_sk, 10) === 10, 12)
            const s = slot_size(1)
            for (let i = 2; i < 256; ++i) ASSERT(s === slot_size(i), 13)
            const txn = otxn_id(0)
            ASSERT(txn.length === 32, 14)
            ASSERT(slot_set(txn, 1) === 1, 15)
            const s2 = slot_size(1)
            ASSERT(s != s2 && s2 > 0, 16)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_set"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_set"), fee(XRP(1)));
    }

    void
    test_slot_size(FeatureBitset features)
    {
        testcase("Test slot_size");
        using namespace jtx;
        Env env{*this, features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var kl_sk = [
            0, 104, 180, 151, 154, 54, 205, 199, 243, 211, 213, 195, 26, 78, 174, 42, 199,
            215, 32, 157, 218, 135, 117, 136, 185, 175, 198, 103, 153, 105, 42, 176, 214,
            107,
            ]
            var Hook = (arg) => {
            ASSERT(slot_size(1) === DOESNT_EXIST, 1)
            ASSERT(slot_set(kl_sk, 1) === 1, 2)
            ASSERT(slot_set(kl_sk, 255) === 255, 3)
            ASSERT(slot_size(1) === slot_size(255), 4)
            const s = slot_size(1)
            ASSERT(s > 0, 5)
            const buf = slot(1)
            ASSERT(buf.length === s, 6)
            ASSERT(sto_validate(buf) === 1, 7)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_size"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_size"), fee(XRP(1)));
    }

    void
    test_slot_subarray(FeatureBitset features)
    {
        testcase("Test slot_subarray");
        using namespace jtx;

        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var DOESNT_EXIST = -5
            var NO_FREE_SLOTS = -6
            var NOT_AN_ARRAY = -22
            var sfMemos = 983049
            var sfMemoData = 458765
            var kl_sk = [
            0, 104, 180, 151, 154, 54, 205, 199, 243, 211, 213, 195, 26, 78, 174, 42, 199,
            215, 32, 157, 218, 135, 117, 136, 185, 175, 198, 103, 153, 105, 42, 176, 214,
            107,
            ]
            var Hook = (arg) => {
            ASSERT(slot_subarray(1, 1, 1) === DOESNT_EXIST, 1)
            ASSERT(slot_set(kl_sk, 1) === 1, 2)
            ASSERT(slot_size(1) > 0, 3)
            ASSERT(slot_subarray(1, 1, 1) === NOT_AN_ARRAY, 4)
            ASSERT(otxn_slot(2) === 2, 5)
            ASSERT(slot_subfield(2, sfMemos, 3) === 3, 6)
            ASSERT(slot_count(3) === 9, 7)
            ASSERT(slot_subarray(3, 0, 0) > 0, 8)
            ASSERT(slot_subarray(3, 5, 100) === 100, 9)
            ASSERT(slot_subarray(3, 6, 100) === 100, 10)
            ASSERT(slot_subfield(100, sfMemoData, 100) === 100, 11)
            let buf = slot(100)
            ASSERT(6 === buf.length, 11)
            ASSERT(
                buf[0] === 5 &&
                buf[1] === 192 &&
                buf[2] === 1 &&
                buf[3] === 202 &&
                buf[4] === 254 &&
                buf[5] === 6,
                12
            )
            ASSERT(slot_subarray(3, 0, 100) === 100, 13)
            ASSERT(slot_subfield(100, sfMemoData, 100) === 100, 14)
            buf = slot(100)
            ASSERT(buf.length === 6, 15)
            ASSERT(
                buf[0] === 5 &&
                buf[1] === 192 &&
                buf[2] === 1 &&
                buf[3] === 202 &&
                buf[4] === 254 &&
                buf[5] === 0,
                16
            )
            for (let i = 0; i < 250; ++i) ASSERT(slot_subarray(3, 0, 0) > 0, 17)
            ASSERT(slot_subarray(3, 0, 0) === NO_FREE_SLOTS, 18)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_subarray"),
            HSFEE);
        env.close();

        // generate an array of memos to attach
        Json::Value jv;
        jv[jss::Account] = bob.human();
        jv[jss::TransactionType] = jss::Payment;
        jv[jss::Flags] = 0;
        jv[jss::Amount] = "1";
        jv[jss::Memos] = Json::Value{Json::arrayValue};
        jv[jss::Destination] = alice.human();
        Json::Value iv;
        for (uint32_t i = 0; i < 8; ++i)
        {
            std::string v = "C001CAFE00";
            v.data()[9] = '0' + i;
            iv[jss::MemoData] = v.c_str();
            iv[jss::MemoFormat] = "";
            iv[jss::MemoType] = "";
            jv[jss::Memos][i][jss::Memo] = iv;
        }

        // invoke the hook
        env(jv, M("test slot_subarray"), fee(XRP(1)));
    }

    void
    test_slot_subfield(FeatureBitset features)
    {
        testcase("Test slot_subfield");
        using namespace jtx;

        Env env{*this, features};
        // Env env{*this, envconfig(), features, nullptr,
        //     beast::severities::kTrace
        // };

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x) => {
            if (!x) {
                rollback(x.toString(), 0)
            }
            }
            var DOESNT_EXIST = -5
            var NO_FREE_SLOTS = -6
            var INVALID_ARGUMENT = -7
            var INVALID_FIELD = -17
            var NOT_AN_OBJECT = -23
            var sfMemos = 983049
            var sfMemoData = 458765
            var sfLastLedgerSequence = 131099
            var sfHashes = 1245186
            var kl_sk = [
            0, 104, 180, 151, 154, 54, 205, 199, 243, 211, 213, 195, 26, 78, 174, 42, 199,
            215, 32, 157, 218, 135, 117, 136, 185, 175, 198, 103, 153, 105, 42, 176, 214,
            107,
            ]
            var Hook = (arg) => {
            ASSERT(slot_subfield(void 0, 1, 1) === INVALID_ARGUMENT)
            ASSERT(slot_subfield(1, void 0, 1) === INVALID_ARGUMENT)
            ASSERT(slot_subfield(1, 1, void 0) === INVALID_ARGUMENT)
            ASSERT(slot_subfield(1, 1, 1) === DOESNT_EXIST)
            ASSERT(slot_subfield(1, 1, 1) === DOESNT_EXIST)
            ASSERT(slot_set(kl_sk, 1) === 1)
            ASSERT(slot_size(1) > 0)
            ASSERT(slot_subfield(1, sfLastLedgerSequence, 0) === 2)
            ASSERT(slot_size(2) > 0)
            ASSERT(slot_size(1) > slot_size(2))
            ASSERT(slot_subfield(1, sfHashes, 0) === 3)
            ASSERT(slot_size(3) > 0)
            ASSERT(slot_size(1) > slot_size(3))
            ASSERT(slot_subfield(1, 4294967295, 0) === INVALID_FIELD)
            ASSERT(slot_subfield(1, sfMemos, 0) === DOESNT_EXIST)
            ASSERT(slot_subfield(3, sfMemoData, 0) === NOT_AN_OBJECT)
            ASSERT(slot_subfield(1, sfLastLedgerSequence, 3) === 3)
            ASSERT(slot_size(2) === slot_size(3))
            for (let i = 0; i < 252; ++i)
                ASSERT(slot_subfield(1, sfLastLedgerSequence, 0) > 0)
            ASSERT(slot_subfield(1, sfLastLedgerSequence, 0) === NO_FREE_SLOTS)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_subfield"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_subfield"), fee(XRP(1)));
    }

    void
    test_slot_type(FeatureBitset features)
    {
        testcase("Test slot_type");
        using namespace jtx;

        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        // set up a trustline which we can retrieve later
        env(trust(alice, bob["USD"](600)));

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x) => {
            if (!x) {
                rollback(x.toString(), 0)
            }
            }
            var DOESNT_EXIST = -5
            var NOT_AN_AMOUNT = -32
            var KEYLET_LINE = 9
            var sfLedgerEntry = 655491329
            var sfTransaction = 655425793
            var sfAccount = 524289
            var sfAmount = 393217
            var sfHighLimit = 393223
            var sfLastLedgerSequence = 131099
            var kl_sk = [
            0, 104, 180, 151, 154, 54, 205, 199, 243, 211, 213, 195, 26, 78, 174, 42, 199,
            215, 32, 157, 218, 135, 117, 136, 185, 175, 198, 103, 153, 105, 42, 176, 214,
            107,
            ]
            var Hook = (arg) => {
            ASSERT(slot_type(1, 0) === DOESNT_EXIST)
            ASSERT(slot_set(kl_sk, 1) === 1)
            ASSERT(slot_size(1) > 0)
            ASSERT(slot_type(1, 0) === sfLedgerEntry)
            ASSERT(slot_subfield(1, sfLastLedgerSequence, 0) === 2)
            ASSERT(slot_size(2) > 0)
            ASSERT(slot_size(1) > slot_size(2))
            ASSERT(slot_type(2, 0) === sfLastLedgerSequence)
            ASSERT(otxn_slot(3) === 3)
            ASSERT(slot_type(3, 0) === sfTransaction)
            ASSERT(slot_subfield(3, sfAmount, 4) === 4)
            ASSERT(slot_type(4, 1) === 1)
            ASSERT(slot_type(3, 1) === NOT_AN_AMOUNT)
            const addra = hook_account()
            const addrb = otxn_field(sfAccount)
            ASSERT(addra.length === 20)
            ASSERT(addrb.length === 20)
            const cur = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 83, 68, 0, 0, 0, 0, 0]
            const kl_tr = util_keylet(KEYLET_LINE, addra, addrb, cur)
            ASSERT(kl_tr.length === 34)
            ASSERT(slot_set(kl_tr, 5) === 5)
            ASSERT(slot_subfield(5, sfHighLimit, 6) === 6)
            ASSERT(slot_type(6, 1) === 0)
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_type"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_type"), fee(XRP(1)));
    }

    void
    test_slot_json(FeatureBitset features)
    {
        testcase("Test slot_json");
        using namespace jtx;

        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        // set up a trustline which we can retrieve later
        env(trust(alice, bob["USD"](600)));

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x) => {
            if (!x) {
                rollback(x.toString(), 0)
            }
            }
            var DOESNT_EXIST = -5
            var INVALID_ARGUMENT = -7
            var KEYLET_LINE = 9
            var sfAccount = 524289
            var kl_sk = [
            0, 104, 180, 151, 154, 54, 205, 199, 243, 211, 213, 195, 26, 78, 174, 42, 199,
            215, 32, 157, 218, 135, 117, 136, 185, 175, 198, 103, 153, 105, 42, 176, 214,
            107,
            ]
            var Hook = (arg) => {
            ASSERT(slot_json(void 0) === INVALID_ARGUMENT)
            ASSERT(slot_json(1) === DOESNT_EXIST)
            ASSERT(slot_set(kl_sk, 1) === 1)
            ASSERT(slot_size(1) > 0)
            const ls = slot_json(1)
            trace('ls', ls, false)
            ASSERT(typeof ls === 'object')
            ASSERT(ls.LedgerEntryType === 'LedgerHashes')
            ASSERT(otxn_slot(2) === 2)
            ASSERT(slot_size(2) > 0)
            const txs = slot_json(2)
            trace('txs', txs, false)
            ASSERT(typeof txs === 'object')
            ASSERT(txs.TransactionType === 'Payment')
            ASSERT(txs.Amount === '1000000')
            const addra = hook_account()
            const addrb = otxn_field(sfAccount)
            ASSERT(addra.length === 20)
            ASSERT(addrb.length === 20)
            const cur = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 85, 83, 68, 0, 0, 0, 0, 0]
            const kl_tr = util_keylet(KEYLET_LINE, addra, addrb, cur)
            ASSERT(kl_tr.length === 34)
            ASSERT(slot_set(kl_tr, 3) === 3)
            const lines = slot_json(3)
            trace('lines', lines, false)
            ASSERT(typeof lines === 'object')
            ASSERT(lines.HighLimit.issuer === 'rPMh7Pi9ct699iZUTWaytJUoHcJ7cgyziK')
            ASSERT(lines.HighLimit.currency === 'USD')
            ASSERT(lines.HighLimit.value === '0')
            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set slot_json"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test slot_json"), fee(XRP(1)));
    }

    void
    test_state(FeatureBitset features)
    {
        testcase("Test state");
        using namespace jtx;

        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                const ASSERT = (x, code) => {
                if (!x) {
                    trace('error', 0, false)
                    rollback(x.toString(), code)
                }
                }

                const Hook = (arg) => {
                ASSERT(state_set('636F6E74656E74', '6B6579') == 7, 0)
                ASSERT(state_set('636F6E74656E7432', '6B657932') == 8, 1)

                const bytes1 = state('6B6579')
                trace('bytes1', bytes1, false)
                ASSERT(bytes1.length == 7, 2)

                const bytes2 = state('6B657932')
                trace('bytes2', bytes2, false)
                ASSERT(bytes2.length == 8, 3)

                for (let i = 32; i < bytes1.length; ++i) {
                    ASSERT(bytes1[i] === [99, 111, 110, 116, 101, 110, 116][i], 4)
                }

                for (let i = 32; i < bytes2.length; ++i) {
                    ASSERT(bytes2[i] === [99, 111, 110, 116, 101, 110, 116, 50][i], 5)
                }

                accept('', 0)
                }
            )[test.hook]"];

            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set state"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test state"), fee(XRP(1)));
            env.close();
        }

        // override hook with a second version that just reads those state
        // objects
        {
            TestHook hook = jswasm[R"[test.hook](
                const ASSERT = (x, code) => {
                if (!x) {
                    trace('error', 0, false)
                    rollback(x.toString(), code)
                }
                }

                const Hook = (arg) => {
                const bytes1 = state('6B6579')
                trace('bytes1', bytes1, false)
                ASSERT(bytes1.length == 7, 2)

                const bytes2 = state('6B657932')
                trace('bytes2', bytes2, false)
                ASSERT(bytes2.length == 8, 3)

                for (let i = 32; i < bytes1.length; ++i) {
                    ASSERT(bytes1[i] === [99, 111, 110, 116, 101, 110, 116][i], 4)
                }

                for (let i = 32; i < bytes2.length; ++i) {
                    ASSERT(bytes2[i] === [99, 111, 110, 116, 101, 110, 116, 50][i], 5)
                }

                accept('', 0)
                }
            )[test.hook]"];

            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set state 2"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test state 2"), fee(XRP(1)));
        }
    }

    void
    test_state_foreign(FeatureBitset features)
    {
        testcase("Test state_foreign");
        using namespace jtx;

        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x, code) => {
                if (!x) {
                    trace('error', 0, false)
                    rollback(x.toString(), code)
                }
                }
                var Hook = (arg) => {
                ASSERT(state_set('636F6E74656E74', '6B6579') == 7, 0)
                const ns = [
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xab, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                ]
                const acc = hook_account()
                ASSERT(acc.length === 20, 1)
                ASSERT(state_foreign_set('636F6E74656E7432', '6B657932', ns, acc) === 8, 2)
                accept('', 0)
                }
            )[test.hook]"];

            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set state_foreign"),
                HSFEE);
            env.close();

            // invoke the hook
            env(pay(bob, alice, XRP(1)), M("test state_foreign"), fee(XRP(1)));
        }

        // set a second hook on bob that will read the state objects from alice
        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x, code) => {
                if (!x) {
                    trace('error', 0, false)
                    rollback(x.toString(), code)
                }
                }
                var sfAccount = 524289
                var DOESNT_EXIST = -5
                var Hook = (arg) => {
                const acc = otxn_field(sfAccount)
                ASSERT(acc.length === 20, 1)
                const ns1 = [
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0,
                ]
                const bytes1 = state_foreign('6B6579', ns1, acc)
                trace('bytes1', bytes1, false)
                ASSERT(bytes1.length == 7, 2)
                let bytes2 = state_foreign('6B657932', ns1, acc)
                trace('bytes2', bytes2, false)
                ASSERT(bytes2 == DOESNT_EXIST, 3)
                const ns2 = [
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 171, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0,
                ]
                bytes2 = state_foreign('6B657932', ns2, acc)
                trace('bytes2', bytes2, false)
                ASSERT(bytes2.length == 8, 4)
                for (let i = 32; i < bytes1.length; ++i) {
                    ASSERT(bytes1[i] === [99, 111, 110, 116, 101, 110, 116][i], 5)
                }
                for (let i = 32; i < bytes2.length; ++i) {
                    ASSERT(bytes2[i] === [99, 111, 110, 116, 101, 110, 116, 50][i], 6)
                }
                accept('', 0)
                }
            )[test.hook]"];

            // install the hook on bob
            env(ripple::test::jtx::hook(
                    bob, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set state_foreign 2"),
                HSFEE);
            env.close();

            // invoke the hook

            env(pay(alice, bob, XRP(1)),
                M("test state_foreign 2"),
                fee(XRP(1)));
        }
    }

    void
    test_state_foreign_set(FeatureBitset features)
    {
        testcase("Test state_foreign_set");
        using namespace jtx;

        Env env{*this, features};
        // Env env{*this, envconfig(), features, nullptr,
        //     beast::severities::kTrace
        // };

        auto const david = Account("david");  // grantee generic
        auto const cho = Account{"cho"};      // invoker
        auto const bob = Account{"bob"};      // grantee specific
        auto const alice = Account{"alice"};  // grantor
        auto const eve = Account{"eve"};      // grantor with small balance
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.fund(XRP(10000), cho);
        env.fund(XRP(10000), david);
        env.fund(XRP(2600), eve);

        TestHook grantee_wasm = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var sfInvoiceID = 327697
            var INVALID_ARGUMENT = -7
            var a = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1,
            ]
            var aa = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]
            var ba = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1,
            ]
            var sa = [1]
            var ha = [
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
            1, 1, 1, 1,
            ]
            var Hook = (arg) => {
            ASSERT(state_foreign_set(a, [], a, aa) === INVALID_ARGUMENT, 1)
            ASSERT(state_foreign_set(a, '', a, aa) === INVALID_ARGUMENT, 2)
            // ASSERT(state_foreign_set(a, a, ba, aa) === INVALID_ARGUMENT, 3) // tequ: https://github.com/Xahau/xahaud/issues/445
            ASSERT(state_foreign_set(a, a, sa, aa) === INVALID_ARGUMENT, 4)
            // ASSERT(state_foreign_set(a, a, a, ba) === INVALID_ARGUMENT, 5) // tequ: shoud uncomment state_foreign_set undefined check
            ASSERT(state_foreign_set(a, a, a, sa) === INVALID_ARGUMENT, 6)
            // ASSERT(state_foreign_set(a, a, null, aa) === INVALID_ARGUMENT, 7) // tequ: shoud uncomment state_foreign_set undefined check
            // ASSERT(state_foreign_set(a, a, 0, aa) === INVALID_ARGUMENT, 8) // tequ: shoud uncomment state_foreign_set undefined check
            ASSERT(state_foreign_set(a, a, void 0, hook_account()) === 32, 9)
            // ASSERT(state_foreign_set(a, a, [], aa) === INVALID_ARGUMENT, 10) // tequ: shoud uncomment state_foreign_set undefined check
            // ASSERT(state_foreign_set(a, a, '', aa) === INVALID_ARGUMENT, 11) // tequ: shoud uncomment state_foreign_set undefined check
            // ASSERT(state_foreign_set(a, a, a, null) === INVALID_ARGUMENT, 12) // tequ: shoud uncomment state_foreign_set undefined check
            // ASSERT(state_foreign_set(a, a, a, 0) === INVALID_ARGUMENT, 13) // tequ: shoud uncomment state_foreign_set undefined check
            ASSERT(state_foreign_set(a, a, a, void 0) === 32, 14)
            // ASSERT(state_foreign_set(a, a, a, []) === INVALID_ARGUMENT, 15) // tequ: shoud uncomment state_foreign_set undefined check
            // ASSERT(state_foreign_set(a, a, a, '') === INVALID_ARGUMENT, 16) // tequ: shoud uncomment state_foreign_set undefined check
            ASSERT(state_foreign_set(null, a, a, hook_account()) === 0, 17)
            ASSERT(state_foreign_set(0, a, a, hook_account()) === 0, 18)
            ASSERT(state_foreign_set(void 0, a, a, hook_account()) === 0, 19)
            ASSERT(state_foreign_set('', a, a, hook_account()) === 0, 20)
            ASSERT(state_foreign_set([], a, a, hook_account()) === 0, 21)
            ASSERT(state_foreign_set(ha, a, a, hook_account()) === 0, 22)
            const txn = otxn_id(0)
            ASSERT(txn.length === 32, 23)
            const grantor = otxn_field(sfInvoiceID)
            ASSERT(grantor.length === 32, 24)
            const one = [
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 1,
            ]
            const zero = [
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0,
            ]
            trace('grantor', grantor.slice(12), false)
            trace('#25', state_foreign_set(txn, one, zero, grantor.slice(12)), true)
            ASSERT(state_foreign_set(txn, one, zero, grantor.slice(12)) == 32, 25)
            accept('', 0)
            }
        )[test.hook]"];

        HASH_WASM(grantee);

        // this is the grantor
        TestHook grantor_wasm = jswasm[R"[test.hook](
            var ASSERT = (x, code) => {
            if (!x) {
                trace('error', 0, false)
                rollback(x.toString(), code)
            }
            }
            function arrayEqual(arr1, arr2) {
            if (arr1.length !== arr2.length) return false
            for (let i = 0; i < arr1.length; i++) {
                if (arr1[i] !== arr2[i]) return false
            }
            return true
            }
            var sfAccount = 524289
            var DOESNT_EXIST = -5
            var Hook = (arg) => {
            const otxnacc = otxn_field(sfAccount)
            ASSERT(otxnacc.length === 20, 0)
            const hookacc = hook_account()
            ASSERT(hookacc.length == 20, 1)
            if (arrayEqual(otxnacc, hookacc)) {
                const one = [
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 1,
                ]
                const zero = [
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0,
                ]
                const y = state_foreign_set(undefined, one, zero, hookacc)
                trace('y', y, false)
                ASSERT(y === 0 || y === DOESNT_EXIST, 2)
            }
            accept('', 0)
            }
        )[test.hook]"];

        HASH_WASM(grantor);

        // install the grantor hook on alice
        {
            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = grantee_hash_str;
            grants[0U][jss::HookGrant][jss::Authorize] = bob.human();

            Json::Value json = ripple::test::jtx::hook(
                alice, {{hsov1(grantor_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            json[jss::Hooks][0U][jss::Hook][jss::HookGrants] = grants;

            env(json, M("set state_foreign_set"), HSFEE);
            env.close();
        }

        // install the grantee hook on bob
        {
            // invoice ID contains the grantor account
            Json::Value json = ripple::test::jtx::hook(
                bob, {{hsov1(grantee_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            env(json, M("set state_foreign_set 2"), HSFEE);
            env.close();
        }

        auto const aliceid = Account("alice").id();
        auto const nsdirkl = keylet::hookStateDir(aliceid, beast::zero);

        std::string const invid = std::string(24, '0') + strHex(alice.id());

        auto const one = ripple::uint256(
            "0000000000000000000000000000000000000000000000000000000000000001");

        // ensure there's no way the state or directory exist before we start
        {
            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!nsdir);

            auto const state1 =
                env.le(ripple::keylet::hookState(aliceid, one, beast::zero));
            BEAST_REQUIRE(!state1);

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 2);
        }

        // inovke the grantee hook but supply an account to foreign_set (through
        // invoiceid) should definitely fail
        {
            Json::Value json = pay(cho, bob, XRP(1));
            json[jss::InvoiceID] =
                "00000000000000000000000000000000000000000000000000000000000000"
                "01";
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 6a"),
                ter(tecHOOK_REJECTED));
        }

        // invoke the grantee hook but supply a valid account for which no
        // grants exist
        {
            Json::Value json = pay(cho, bob, XRP(1));
            json[jss::InvoiceID] = std::string(24, '0') + strHex(david.id());
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 6b"),
                ter(tecHOOK_REJECTED));
            {
                auto const nsdir = env.le(nsdirkl);
                BEAST_REQUIRE(!nsdir);

                auto const state1 = env.le(
                    ripple::keylet::hookState(david.id(), one, beast::zero));
                BEAST_REQUIRE(!state1);

                BEAST_EXPECT((*env.le("david"))[sfOwnerCount] == 0);
            }
        }

        // invoke the grantee hook, this will create the state on the grantor
        {
            Json::Value json = pay(cho, bob, XRP(1));
            json[jss::InvoiceID] = invid;
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 6"),
                ter(tesSUCCESS));
        }

        // check state
        {
            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!!nsdir);

            auto const state1 =
                env.le(ripple::keylet::hookState(aliceid, one, beast::zero));
            BEAST_REQUIRE(!!state1);

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 3);

            BEAST_EXPECT(state1->getFieldH256(sfHookStateKey) == one);

            auto const data1 = state1->getFieldVL(sfHookStateData);
            BEAST_EXPECT(data1.size() == 32);
            BEAST_EXPECT(uint256::fromVoid(data1.data()) == env.txid());
        }

        // invoke the grantor hook, this will delete the state
        env(pay(alice, cho, XRP(1)),
            M("test state_foreign_set 4"),
            fee(XRP(1)));

        // check state was removed
        {
            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!nsdir);

            auto const state1 =
                env.le(ripple::keylet::hookState(aliceid, one, beast::zero));
            BEAST_REQUIRE(!state1);

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 2);
        }

        // install grantee hook on david
        {
            // invoice ID contains the grantor account
            Json::Value json = ripple::test::jtx::hook(
                david, {{hsov1(grantee_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            env(json, M("set state_foreign_set 5"), HSFEE);
            env.close();
        }

        // invoke daivd, expect failure
        {
            Json::Value json = pay(cho, david, XRP(1));
            json[jss::InvoiceID] = invid;
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 6"),
                ter(tecHOOK_REJECTED));
        }

        // remove sfAuthorize from alice grants
        {
            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = grantee_hash_str;

            Json::Value json = ripple::test::jtx::hook(
                alice, {{hsov1(grantor_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            json[jss::Hooks][0U][jss::Hook][jss::HookGrants] = grants;

            env(json, M("set state_foreign_set 7"), HSFEE);
            env.close();
        }

        // invoke david again, expect success
        {
            Json::Value json = pay(cho, david, XRP(1));
            json[jss::InvoiceID] = invid;
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 8"),
                ter(tesSUCCESS));
        }

        // check state
        {
            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!!nsdir);

            auto const state1 =
                env.le(ripple::keylet::hookState(aliceid, one, beast::zero));
            BEAST_REQUIRE(!!state1);

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 3);

            BEAST_EXPECT(state1->getFieldH256(sfHookStateKey) == one);

            auto const data1 = state1->getFieldVL(sfHookStateData);
            BEAST_EXPECT(data1.size() == 32);
            BEAST_EXPECT(uint256::fromVoid(data1.data()) == env.txid());
        }

        // change alice's namespace
        {
            Json::Value json = ripple::test::jtx::hook(
                alice, {{hsov1(grantor_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            json[jss::Hooks][0U][jss::Hook][jss::HookNamespace] =
                "77777777777777777777777777777777777777777777777777777777777777"
                "77";
            env(json, M("set state_foreign_set 9"), HSFEE);
            env.close();
        }

        // invoke david again, expect failure
        {
            Json::Value json = pay(cho, david, XRP(1));
            json[jss::InvoiceID] = invid;
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 10"),
                ter(tecHOOK_REJECTED));
        }

        // check reserve exhaustion
        TestHook exhaustion_wasm = jswasm[R"[test.hook](
            var ASSERT = (x, code) => {
            if (!x) {
                trace('error', 0, false)
                rollback(x.toString(), code)
            }
            }
            var sfInvoiceID = 327697
            var Hook = (arg) => {
            let txn = otxn_id(0)
            ASSERT(txn.length === 32, 0)
            trace('txn', txn, true)
            const grantor = otxn_field(sfInvoiceID)
            ASSERT(grantor.length === 32, 1)
            const iterations = grantor[0]
            trace('iterations', iterations, false)
            const zero = [
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0,
            ]
            for (let i = 0; i < iterations; ++i) {
                txn[0] = i
                trace('exhaustion i: ', i, false)
                trace('exhaustion txn: ', txn, false)
                trace('exhaustion txn: ', txn, true)
                ASSERT(state_foreign_set(txn, txn, zero, grantor.slice(12)) == 32, 2)
            }
            accept('', 0)
            }
        )[test.hook]"];

        HASH_WASM(exhaustion);

        // install the grantor hook on eve
        {
            Json::Value grants{Json::arrayValue};
            grants[0U][jss::HookGrant] = Json::Value{};
            grants[0U][jss::HookGrant][jss::HookHash] = exhaustion_hash_str;
            grants[0U][jss::HookGrant][jss::Authorize] = bob.human();

            Json::Value json = ripple::test::jtx::hook(
                eve, {{hsov1(grantor_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            json[jss::Hooks][0U][jss::Hook][jss::HookGrants] = grants;

            env(json, M("set state_foreign_set 11"), HSFEE);
            env.close();
        }

        // install exhaustion grantee on bob
        {
            Json::Value json = ripple::test::jtx::hook(
                bob, {{hsov1(exhaustion_wasm, 1, HSDROPS, overrideFlag)}}, 0);
            env(json, M("set state_foreign_set 12"), HSFEE);
            env.close();
        }

        // now invoke repeatedly until exhaustion is reached
        {
            Json::Value json = pay(cho, bob, XRP(1));
            json[jss::InvoiceID] =
                "01" + std::string(22, '0') + strHex(eve.id());

            // 2500 xrp less 1 account reserve (200) divided by 50xrp per object
            // reserve = 46 objects of these we already have: 1 hook, 1
            // sfAuthorize, so 44 objects can be allocated
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 13"),
                ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 3);

            // now we have allocated 1 state object, so 43 more can be allocated

            // try to set 44 state entries, this will fail
            json[jss::InvoiceID] =
                "2C" + std::string(22, '0') + strHex(eve.id());
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 14"),
                ter(tecHOOK_REJECTED));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 3);

            // try to set 43 state objects, this will succeed
            json[jss::InvoiceID] =
                "2B" + std::string(22, '0') + strHex(eve.id());
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 15"),
                ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 46);

            // try to set one state object, this will fail
            env(json,
                fee(XRP(1)),
                M("test state_foreign_set 16"),
                ter(tecHOOK_REJECTED));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 46);
        }
    }

    void
    test_state_set(FeatureBitset features)
    {
        testcase("Test state_set");
        using namespace jtx;

        Env env{*this, features};
        // Env env{*this, envconfig(), features, nullptr,
        //     beast::severities::kTrace
        // };

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        auto const cho = Account{"cho"};
        auto const david = Account{"david"};
        auto const eve = Account{"eve"};      // small balance
        auto const frank = Account{"frank"};  // big balance
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.fund(XRP(10000), cho);
        env.fund(XRP(1000000), david);
        env.fund(XRP(2600), eve);
        env.fund(XRP(1000000000), frank);

        // install a rollback hook on cho
        env(ripple::test::jtx::hook(
                cho, {{hsov1(rollback_wasm, 1, HSDROPS, overrideFlag)}}, 0),
            M("set state_set rollback"),
            HSFEE);
        env.close();

        auto const aliceid = Account("alice").id();

        auto const nsdirkl = keylet::hookStateDir(aliceid, beast::zero);

        // ensure there's no way the state or directory exist before we start
        {
            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!nsdir);

            auto const state1 = env.le(
                ripple::keylet::hookState(aliceid, beast::zero, beast::zero));
            BEAST_REQUIRE(!state1);

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 0);
        }

        // first hook will set two state objects with different keys and data on
        // alice
        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x, line) => {
                if (!x) {
                    trace('line', line, false)
                    rollback(x.toString(), line)
                }
                }
                var INVALID_ARGUMENT = -7
                var a = [
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1,
                ]
                var ba = [
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1,
                ]
                var ha = [
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
                1, 1, 1, 1,
                ]
                var data2 = [
                35, 19, 150, 104, 120, 220, 171, 196, 64, 38, 7, 43, 163, 210, 12, 105, 64,
                221, 205, 231, 56, 155, 11, 169, 108, 60, 179, 135, 55, 2, 129, 232, 43, 221,
                93, 187, 64, 217, 102, 150, 111, 193, 107, 232, 212, 124, 123, 98, 20, 76,
                209, 75, 170, 153, 54, 117, 233, 34, 173, 15, 95, 148, 29, 134, 235, 168, 19,
                153, 249, 152, 255, 202, 91, 134, 47, 223, 103, 143, 226, 227, 195, 55, 204,
                71, 15, 51, 136, 176, 51, 59, 2, 85, 103, 22, 164, 251, 142, 133, 111, 216,
                132, 22, 163, 84, 24, 52, 6, 14, 246, 101, 52, 5, 38, 126, 5, 116, 218, 9,
                191, 85, 140, 117, 146, 172, 51, 251, 1, 141,
                ]
                var Hook = (arg) => {
                ASSERT(state_set(a, ba) === INVALID_ARGUMENT, 11)
                ASSERT(state_set(a, ha) === INVALID_ARGUMENT, 12)
                const key = [
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0,
                ]
                const data = [202, 254, 186, 190]
                ASSERT(state_set(data, key) === 4, 13)
                const key2 = [1, 2, 3]
                ASSERT(state_set(data2, key2) === 128, 14)
                accept('', 0)
                }
            )[test.hook]"];

            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
                M("set state_set 1"),
                HSFEE);
            env.close();

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 1);

            // invoke the hook with cho (rollback after alice's hooks have
            // executed)
            env(pay(alice, cho, XRP(1)),
                M("test state_set 1 rollback"),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 1);

            auto const nsdir = env.le(nsdirkl);
            BEAST_EXPECT(!nsdir);

            auto const state1 = env.le(
                ripple::keylet::hookState(aliceid, beast::zero, beast::zero));
            BEAST_EXPECT(!state1);

            // invoke the hook from bob to alice, this will work
            env(pay(bob, alice, XRP(1)), M("test state_set 1"), fee(XRP(1)));
            env.close();
        }

        // check that the state object and namespace exists
        {
            // owner count should be 1 hook +  2 state objects == 3
            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 3);

            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!!nsdir);

            BEAST_EXPECT(nsdir->getFieldV256(sfIndexes).size() == 2);

            auto const state1 = env.le(
                ripple::keylet::hookState(aliceid, beast::zero, beast::zero));
            BEAST_REQUIRE(!!state1);

            BEAST_EXPECT(state1->getFieldH256(sfHookStateKey) == beast::zero);

            auto const data1 = state1->getFieldVL(sfHookStateData);
            BEAST_EXPECT(data1.size() == 4);
            BEAST_EXPECT(
                data1[0] == 0xCAU && data1[1] == 0xFEU && data1[2] == 0xBAU &&
                data1[3] == 0xBEU);

            uint8_t key2[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3};

            auto const state2 = env.le(ripple::keylet::hookState(
                aliceid, uint256::fromVoid(key2), beast::zero));

            BEAST_REQUIRE(!!state2);

            auto const lekey2 = state2->getFieldH256(sfHookStateKey);

            BEAST_EXPECT(lekey2 == uint256::fromVoid(key2));

            uint8_t data2[128] = {
                0x23U, 0x13U, 0x96U, 0x68U, 0x78U, 0xDCU, 0xABU, 0xC4U, 0x40U,
                0x26U, 0x07U, 0x2BU, 0xA3U, 0xD2U, 0x0CU, 0x69U, 0x40U, 0xDDU,
                0xCDU, 0xE7U, 0x38U, 0x9BU, 0x0BU, 0xA9U, 0x6CU, 0x3CU, 0xB3U,
                0x87U, 0x37U, 0x02U, 0x81U, 0xE8U, 0x2BU, 0xDDU, 0x5DU, 0xBBU,
                0x40U, 0xD9U, 0x66U, 0x96U, 0x6FU, 0xC1U, 0x6BU, 0xE8U, 0xD4U,
                0x7CU, 0x7BU, 0x62U, 0x14U, 0x4CU, 0xD1U, 0x4BU, 0xAAU, 0x99U,
                0x36U, 0x75U, 0xE9U, 0x22U, 0xADU, 0x0FU, 0x5FU, 0x94U, 0x1DU,
                0x86U, 0xEBU, 0xA8U, 0x13U, 0x99U, 0xF9U, 0x98U, 0xFFU, 0xCAU,
                0x5BU, 0x86U, 0x2FU, 0xDFU, 0x67U, 0x8FU, 0xE2U, 0xE3U, 0xC3U,
                0x37U, 0xCCU, 0x47U, 0x0FU, 0x33U, 0x88U, 0xB0U, 0x33U, 0x3BU,
                0x02U, 0x55U, 0x67U, 0x16U, 0xA4U, 0xFBU, 0x8EU, 0x85U, 0x6FU,
                0xD8U, 0x84U, 0x16U, 0xA3U, 0x54U, 0x18U, 0x34U, 0x06U, 0x0EU,
                0xF6U, 0x65U, 0x34U, 0x05U, 0x26U, 0x7EU, 0x05U, 0x74U, 0xDAU,
                0x09U, 0xBFU, 0x55U, 0x8CU, 0x75U, 0x92U, 0xACU, 0x33U, 0xFBU,
                0x01U, 0x8DU};

            auto const ledata2 = state2->getFieldVL(sfHookStateData);
            BEAST_REQUIRE(ledata2.size() == sizeof(data2));

            for (uint32_t i = 0; i < sizeof(data2); ++i)
                BEAST_EXPECT(data2[i] == ledata2[i]);
        }

        // make amother hook to override an existing state and delete an
        // existing state
        {
            TestHook hook = jswasm[R"[test.hook](
                var ASSERT = (x, line) => {
                if (!x) {
                    trace('line', line, false)
                    rollback(x.toString(), line)
                }
                }
                var Hook = (arg) => {
                const key = [0]
                const data = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2]
                ASSERT(state_set(data, key) === 16, 21)
                const key2 = [
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 1, 2, 3,
                ]
                ASSERT(state_set(void 0, key2) === 0, 22)
                accept('', 0)
                }
            )[test.hook]"];

            TestHook hook2 = jswasm[R"[test.hook](
                var ASSERT = (x, line) => {
                if (!x) {
                    trace('line', line, false)
                    rollback(x.toString(), line)
                }
                }
                var Hook = (arg) => {
                const key = [0]
                const data = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2]
                const data_read = state(key)
                ASSERT(data_read.length === 16, 31)
                for (let i = 0; i < 16; ++i) ASSERT(data[i] === data_read[i], 32)
                accept('', 0)
                }
            )[test.hook]"];
            // install the hook on alice
            env(ripple::test::jtx::hook(
                    alice,
                    {{{hsov1(hook, 1, HSDROPS, overrideFlag)},
                      {},
                      {},
                      {hsov1(hook2, 1, HSDROPS, 0)}}},
                    0),
                M("set state_set 2"),
                HSFEE);
            env.close();

            // two hooks + two state objects = 4
            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 4);

            // this hook will be installed on bob, and it will verify the newly
            // updated state is also available on his side. caution must be
            // taken because bob's hooks will execute first if bob's is the
            // otxn. therefore we will flip to a payment from alice to bob here
            TestHook hook3 = jswasm[R"[test.hook](
                var ASSERT = (x, line) => {
                if (!x) {
                    trace('line', line, false)
                    rollback(x.toString(), line)
                }
                }
                var sfAccount = 524289
                var Hook = (arg) => {
                hook_again()
                const alice = otxn_field(sfAccount)
                trace('alice', alice, true)
                ASSERT(alice.length === 20, 41)
                const key = [
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0,
                ]
                const data = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2]
                const data_read = state_foreign(key, key, alice)
                ASSERT(data_read.length == 16, 42)
                for (let i = 0; i < 16; ++i) ASSERT(data[i] === data_read[i], 43)
                accept('', 0)
                }
            )[test.hook]"];

            // install the hook on bob
            env(ripple::test::jtx::hook(
                    bob, {{hsov1(hook3, 1, HSDROPS, overrideFlag)}}, 0),
                M("set state_set 3"),
                HSFEE);
            env.close();

            // invoke the hook with cho (rollback after alice's hooks have
            // executed)
            env(pay(alice, cho, XRP(1)),
                M("test state_set 3 rollback"),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));

            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 4);

            // invoke the hook
            env(pay(alice, bob, XRP(1)), M("test state_set 3"), fee(XRP(1)));
            env.close();
        }

        // check that the updates have been made
        {
            // two hooks + one state == 3
            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 3);
            BEAST_EXPECT((*env.le("bob"))[sfOwnerCount] == 1);

            auto const nsdir = env.le(nsdirkl);
            BEAST_REQUIRE(!!nsdir);

            BEAST_EXPECT(nsdir->getFieldV256(sfIndexes).size() == 1);

            auto const state1 = env.le(
                ripple::keylet::hookState(aliceid, beast::zero, beast::zero));
            BEAST_REQUIRE(!!state1);

            BEAST_EXPECT(state1->getFieldH256(sfHookStateKey) == beast::zero);

            auto const ledata1 = state1->getFieldVL(sfHookStateData);
            BEAST_EXPECT(ledata1.size() == 16);
            uint8_t data1[16] = {
                1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2};

            for (uint32_t i = 0; i < sizeof(data1); ++i)
                BEAST_EXPECT(data1[i] == ledata1[i]);

            uint8_t key2[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3};

            auto const state2 = env.le(ripple::keylet::hookState(
                aliceid, uint256::fromVoid(key2), beast::zero));

            BEAST_REQUIRE(!state2);
        }

        // create a hook state inside the weak side of an execution, while the
        // strong side is rolled back
        {
            TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var Hook = (arg) => {
            hook_again()
            const key = [255]
            const data = [255, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2]
            ASSERT(state_set(data, key) == 16, 2)
            accept('', 0)
            }
            )[test.hook]"];

            // install the hook on alice, deleting the other hook
            env(ripple::test::jtx::hook(
                    alice,
                    {{{hsov1(hook, 1, HSDROPS, overrideFlag)},
                      {},
                      {},
                      {hso_delete()}}},
                    0),
                M("set state_set 4"),
                HSFEE);
            env.close();

            // invoke from alice to cho, this will cause a rollback, however the
            // hook state should still be updated because the hook specified
            // hook_again, and in the second weak execution the hook is allowed
            // to set state
            env(pay(alice, cho, XRP(1)),
                M("test state_set 4 rollback"),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));

            uint8_t key[32] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0,    0,
                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFFU};

            auto const state = env.le(ripple::keylet::hookState(
                aliceid, uint256::fromVoid(key), beast::zero));

            BEAST_EXPECT(state);

            // one hook + two state objects == 3
            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 3);

            // delete alice's hook
            env(ripple::test::jtx::hook(
                    alice, {{{hso_delete()}, {}, {}, {}}}, 0),
                M("set state_set 5 delete"),
                HSFEE);
            env.close();

            // check the state is still present
            {
                auto const state = env.le(ripple::keylet::hookState(
                    aliceid, uint256::fromVoid(key), beast::zero));
                BEAST_EXPECT(state);
            }

            // zero hooks + two state objects == 2
            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 2);

            // put on a different hook

            env(ripple::test::jtx::hook(
                    alice,
                    {{hsov1(rollback_wasm, 1, HSDROPS, overrideFlag)}},
                    0),
                M("set state_set rollback2"),
                HSFEE);
            env.close();

            // check the state is still present
            {
                auto const state = env.le(ripple::keylet::hookState(
                    aliceid, uint256::fromVoid(key), beast::zero));
                BEAST_EXPECT(state);
            }

            // one hooks + two state objects == 3
            BEAST_EXPECT((*env.le("alice"))[sfOwnerCount] == 3);
        }

        // check reserve exhaustion
        TestHook exhaustion_wasm = jswasm[R"[test.hook](
            var ASSERT = (x, line) => {
            if (!x) {
                trace('line', line, false)
                rollback(x.toString(), line)
            }
            }
            var sfInvoiceID = 327697
            var Hook = (arg) => {
            const txn = otxn_id(0)
            ASSERT(txn.length === 32, 1)
            const grantor = otxn_field(sfInvoiceID)
            ASSERT(grantor.length === 32, 2)
            const iterations = (grantor[0] << 8) + grantor[1]
            const p = hook_pos()
            for (let i = 0; i < iterations; ++i) {
                txn[0] = i & 255
                txn[1] = (i >> 8) & 255
                txn[2] = p
                ASSERT(state_set(txn, txn) === 32, 3)
            }
            accept('', 0)
            }
        )[test.hook]"];

        HASH_WASM(exhaustion);

        // install the exhaustion hook on eve
        {
            Json::Value json = ripple::test::jtx::hook(
                eve, {{hsov1(exhaustion_wasm, 1, HSDROPS, overrideFlag)}}, 0);

            env(json, M("set state_set 6"), HSFEE);
            env.close();
        }

        // now invoke repeatedly until exhaustion is reached
        {
            Json::Value json = pay(david, eve, XRP(1));
            json[jss::InvoiceID] = "0001" + std::string(60, '0');

            // 2500 xrp less 1 account reserve (200) divided by 50xrp per object
            // reserve = 46 objects of these we already have: 1 hook, so 45
            // objects can be allocated
            env(json, fee(XRP(1)), M("test state_set 7"), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 2);

            // now we have allocated 1 state object, so 44 more can be allocated

            // try to set 45 state entries, this will fail
            json[jss::InvoiceID] = "002D" + std::string(60, '0');
            env(json,
                fee(XRP(1)),
                M("test state_set 8"),
                ter(tecHOOK_REJECTED));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 2);

            // try to set 44 state objects, this will succeed
            json[jss::InvoiceID] = "002C" + std::string(60, '0');
            env(json, fee(XRP(1)), M("test state_set 9"), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 46);

            // try to set one state object, this will fail
            env(json,
                fee(XRP(1)),
                M("test state_set 10"),
                ter(tecHOOK_REJECTED));
            env.close();
            BEAST_EXPECT((*env.le("eve"))[sfOwnerCount] == 46);
        }

        // test maximum state modification
        {
            // install the hook into every position on frank
            env(ripple::test::jtx::hook(
                    frank,
                    {{hsov1(exhaustion_wasm, 1, HSDROPS),
                      hsov1(exhaustion_wasm, 1, HSDROPS),
                      hsov1(exhaustion_wasm, 1, HSDROPS),
                      hsov1(exhaustion_wasm, 1, HSDROPS)}},
                    0),
                M("set state_set 11"),
                HSFEE,
                ter(tesSUCCESS));

            Json::Value json = pay(david, frank, XRP(1));

            // we can modify 256 entries at a time with the hook, but first we
            // want to test too many modifications so we will do 65 which times
            // 4 executions is 260
            json[jss::InvoiceID] = "0041" + std::string(60, '0');
            env(json,
                fee(XRP(1)),
                M("test state_set 12"),
                ter(tecHOOK_REJECTED));
            env.close();
            BEAST_EXPECT((*env.le("frank"))[sfOwnerCount] == 4);

            // now we will do 64 which is exactly 256, which should be accepted
            json[jss::InvoiceID] = "0040" + std::string(60, '0');
            env(json, fee(XRP(1)), M("test state_set 13"), ter(tesSUCCESS));
            env.close();
            BEAST_EXPECT((*env.le("frank"))[sfOwnerCount] == 260);
        }

        // RH TODO:
        // check state can be set on emit callback
        // check namespacing provides for non-collision of same key
    }

    void
    test_trace(FeatureBitset features)
    {
        testcase("Test trace");
        using namespace jtx;

        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            const Hook = (arg) => {
            ASSERT(trace('', 0, false) === 0)
            ASSERT(trace('', [18], true) === 0)

            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set trace"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test trace"), fee(XRP(1)));
    }

    void
    test_util_accid(FeatureBitset features)
    {
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            const Hook = (arg) => {
            let addr = 'rMEGJtK2SttrtAfoKaqKUpCrDCi9saNuLg'
            let b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xde &&
                b[1] === 0x15 &&
                b[2] === 0x1e &&
                b[3] === 0x2f &&
                b[4] === 0xb2 &&
                b[5] === 0xaa &&
                b[6] === 0xbd &&
                b[7] === 0x1a &&
                b[8] === 0x5b &&
                b[9] === 0xd0 &&
                b[10] === 0x2f &&
                b[11] === 0x63 &&
                b[12] === 0x68 &&
                b[13] === 0x26 &&
                b[14] === 0xdf &&
                b[15] === 0x43 &&
                b[16] === 0x50 &&
                b[17] === 0xc0 &&
                b[18] === 0x40 &&
                b[19] === 0xde
            )

            addr = 'rNo8xzUAauXENpvsMVJ9Q9w5LtVxCVFN4p'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x97 &&
                b[1] === 0x73 &&
                b[2] === 0x23 &&
                b[3] === 0xaa &&
                b[4] === 0x33 &&
                b[5] === 0x7c &&
                b[6] === 0xb6 &&
                b[7] === 0x82 &&
                b[8] === 0x37 &&
                b[9] === 0x83 &&
                b[10] === 0x58 &&
                b[11] === 0x3a &&
                b[12] === 0x7a &&
                b[13] === 0xdf &&
                b[14] === 0x4e &&
                b[15] === 0xd8 &&
                b[16] === 0x52 &&
                b[17] === 0x2c &&
                b[18] === 0xa8 &&
                b[19] === 0xf0
            )

            addr = 'rUpwuJR1xLH18aHLP5nEm4Hw215tmkq6V7'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x78 &&
                b[1] === 0xe2 &&
                b[2] === 0x10 &&
                b[3] === 0xac &&
                b[4] === 0x98 &&
                b[5] === 0x38 &&
                b[6] === 0xf2 &&
                b[7] === 0x5a &&
                b[8] === 0x3b &&
                b[9] === 0x7e &&
                b[10] === 0xde &&
                b[11] === 0x51 &&
                b[12] === 0x37 &&
                b[13] === 0x13 &&
                b[14] === 0x94 &&
                b[15] === 0xed &&
                b[16] === 0x80 &&
                b[17] === 0x77 &&
                b[18] === 0x89 &&
                b[19] === 0x48
            )

            addr = 'ravUPmVUQ65qeuNSFiN6W2U88smjJYHBJm'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x40 &&
                b[1] === 0xe8 &&
                b[2] === 0x2f &&
                b[3] === 0x55 &&
                b[4] === 0xc7 &&
                b[5] === 0x3a &&
                b[6] === 0xeb &&
                b[7] === 0xcf &&
                b[8] === 0xc9 &&
                b[9] === 0x1d &&
                b[10] === 0x3b &&
                b[11] === 0xf4 &&
                b[12] === 0x77 &&
                b[13] === 0x76 &&
                b[14] === 0x50 &&
                b[15] === 0x2b &&
                b[16] === 0x49 &&
                b[17] === 0x7b &&
                b[18] === 0x12 &&
                b[19] === 0x2c
            )

            addr = 'rPXQ8PW1C382oewiEyJrAWtDQBNsQhAtWA'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xf7 &&
                b[1] === 0x13 &&
                b[2] === 0x19 &&
                b[3] === 0x49 &&
                b[4] === 0x3f &&
                b[5] === 0xa6 &&
                b[6] === 0xa3 &&
                b[7] === 0xdb &&
                b[8] === 0x62 &&
                b[9] === 0xae &&
                b[10] === 0x12 &&
                b[11] === 0x1b &&
                b[12] === 0x12 &&
                b[13] === 0x6c &&
                b[14] === 0xfe &&
                b[15] === 0x81 &&
                b[16] === 0x49 &&
                b[17] === 0x5a &&
                b[18] === 0x49 &&
                b[19] === 0x16
            )

            addr = 'rnZbUT8tpm48KEdfELCxRjJJhNV1JNYcg5'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x32 &&
                b[1] === 0x0a &&
                b[2] === 0x5c &&
                b[3] === 0x53 &&
                b[4] === 0x61 &&
                b[5] === 0x5b &&
                b[6] === 0x4b &&
                b[7] === 0x57 &&
                b[8] === 0x1d &&
                b[9] === 0xc4 &&
                b[10] === 0x6f &&
                b[11] === 0x13 &&
                b[12] === 0xbd &&
                b[13] === 0x4f &&
                b[14] === 0x31 &&
                b[15] === 0x70 &&
                b[16] === 0x84 &&
                b[17] === 0xd1 &&
                b[18] === 0xb1 &&
                b[19] === 0x68
            )

            addr = 'rPghxri3jhBaxBfWGAHrVC4KANoRBe6dcM'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xf8 &&
                b[1] === 0xb6 &&
                b[2] === 0x49 &&
                b[3] === 0x2b &&
                b[4] === 0x5b &&
                b[5] === 0x21 &&
                b[6] === 0xc8 &&
                b[7] === 0xda &&
                b[8] === 0xbd &&
                b[9] === 0x0f &&
                b[10] === 0x1d &&
                b[11] === 0x2f &&
                b[12] === 0xd9 &&
                b[13] === 0xf4 &&
                b[14] === 0x5b &&
                b[15] === 0xde &&
                b[16] === 0xcc &&
                b[17] === 0x6a &&
                b[18] === 0xeb &&
                b[19] === 0x91
            )

            addr = 'r4Tck2QJcfcwBuTgVJXYb4QbrKP6mT1acM'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xeb &&
                b[1] === 0x63 &&
                b[2] === 0x4c &&
                b[3] === 0xd6 &&
                b[4] === 0xf9 &&
                b[5] === 0xbf &&
                b[6] === 0x50 &&
                b[7] === 0xc1 &&
                b[8] === 0xd9 &&
                b[9] === 0x79 &&
                b[10] === 0x30 &&
                b[11] === 0x84 &&
                b[12] === 0x1b &&
                b[13] === 0xfc &&
                b[14] === 0x35 &&
                b[15] === 0x32 &&
                b[16] === 0xbd &&
                b[17] === 0x6d &&
                b[18] === 0xc0 &&
                b[19] === 0x75
            )

            addr = 'rETHUL5T1SzM6AMotnsK5V3J5XMwJ9UhZ2'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x9e &&
                b[1] === 0x8a &&
                b[2] === 0x18 &&
                b[3] === 0x66 &&
                b[4] === 0x92 &&
                b[5] === 0x0e &&
                b[6] === 0xe5 &&
                b[7] === 0xed &&
                b[8] === 0xfa &&
                b[9] === 0xe3 &&
                b[10] === 0x23 &&
                b[11] === 0x15 &&
                b[12] === 0xcb &&
                b[13] === 0x83 &&
                b[14] === 0xef &&
                b[15] === 0x73 &&
                b[16] === 0xe4 &&
                b[17] === 0x91 &&
                b[18] === 0x0b &&
                b[19] === 0xca
            )

            addr = 'rh9CggaWiY6QdD55ZkbbnrFpHJkKSauLfC'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x22 &&
                b[1] === 0x8b &&
                b[2] === 0xff &&
                b[3] === 0x31 &&
                b[4] === 0xb4 &&
                b[5] === 0x93 &&
                b[6] === 0xf6 &&
                b[7] === 0xc1 &&
                b[8] === 0x12 &&
                b[9] === 0xea &&
                b[10] === 0xd6 &&
                b[11] === 0xdf &&
                b[12] === 0xc4 &&
                b[13] === 0x05 &&
                b[14] === 0xb3 &&
                b[15] === 0x7d &&
                b[16] === 0xc0 &&
                b[17] === 0x65 &&
                b[18] === 0x21 &&
                b[19] === 0x34
            )

            addr = 'r9sYGdPCGuJauy8QVG4CHnvp5U4eu3yY2B'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x58 &&
                b[1] === 0x3b &&
                b[2] === 0xf0 &&
                b[3] === 0xcb &&
                b[4] === 0x95 &&
                b[5] === 0x80 &&
                b[6] === 0xde &&
                b[7] === 0xa0 &&
                b[8] === 0xb3 &&
                b[9] === 0x71 &&
                b[10] === 0xd0 &&
                b[11] === 0x18 &&
                b[12] === 0x17 &&
                b[13] === 0x1a &&
                b[14] === 0xbb &&
                b[15] === 0x98 &&
                b[16] === 0x1f &&
                b[17] === 0xcc &&
                b[18] === 0x7c &&
                b[19] === 0x68
            )

            addr = 'r4yJX9eU65WHfmKz6xXmSRf9CZN6bXfpWb'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xf1 &&
                b[1] === 0x00 &&
                b[2] === 0x8f &&
                b[3] === 0x64 &&
                b[4] === 0x0f &&
                b[5] === 0x99 &&
                b[6] === 0x19 &&
                b[7] === 0xda &&
                b[8] === 0xcf &&
                b[9] === 0x48 &&
                b[10] === 0x18 &&
                b[11] === 0x1c &&
                b[12] === 0x35 &&
                b[13] === 0x2e &&
                b[14] === 0xe4 &&
                b[15] === 0x3e &&
                b[16] === 0x37 &&
                b[17] === 0x7c &&
                b[18] === 0x01 &&
                b[19] === 0xf6
            )

            addr = 'rBkXoWoXPHuZy2nHbE7L1zJfqAvb4jHRrK'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x75 &&
                b[1] === 0xec &&
                b[2] === 0xdb &&
                b[3] === 0x3b &&
                b[4] === 0x9a &&
                b[5] === 0x71 &&
                b[6] === 0xd9 &&
                b[7] === 0xef &&
                b[8] === 0xd6 &&
                b[9] === 0x55 &&
                b[10] === 0x15 &&
                b[11] === 0xdd &&
                b[12] === 0xea &&
                b[13] === 0xd2 &&
                b[14] === 0x36 &&
                b[15] === 0x7a &&
                b[16] === 0x05 &&
                b[17] === 0x6f &&
                b[18] === 0x4e &&
                b[19] === 0x5f
            )

            addr = 'rnaUBeEBNuyv57Jk127DsApEQoR8JqWpie'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x2c &&
                b[1] === 0xdb &&
                b[2] === 0xeb &&
                b[3] === 0x1f &&
                b[4] === 0x5e &&
                b[5] === 0xc5 &&
                b[6] === 0xd7 &&
                b[7] === 0x5f &&
                b[8] === 0xac &&
                b[9] === 0xbd &&
                b[10] === 0x19 &&
                b[11] === 0xc8 &&
                b[12] === 0x3f &&
                b[13] === 0x45 &&
                b[14] === 0x3b &&
                b[15] === 0xa8 &&
                b[16] === 0xa0 &&
                b[17] === 0x1c &&
                b[18] === 0xdb &&
                b[19] === 0x0f
            )

            addr = 'rJHmUPMQ6qYdaqMizDZY8FKcCqCJxYYnb3'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xbd &&
                b[1] === 0xa5 &&
                b[2] === 0xaf &&
                b[3] === 0xda &&
                b[4] === 0x5f &&
                b[5] === 0x04 &&
                b[6] === 0xe7 &&
                b[7] === 0xef &&
                b[8] === 0x16 &&
                b[9] === 0x7a &&
                b[10] === 0x35 &&
                b[11] === 0x94 &&
                b[12] === 0x6e &&
                b[13] === 0xef &&
                b[14] === 0x19 &&
                b[15] === 0xfa &&
                b[16] === 0x12 &&
                b[17] === 0xf3 &&
                b[18] === 0x1c &&
                b[19] === 0x64
            )

            addr = 'rpJtt64FNNtaEBgqbJcrrunucUWJSdKJa2'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x0e &&
                b[1] === 0x5a &&
                b[2] === 0x83 &&
                b[3] === 0x89 &&
                b[4] === 0xc0 &&
                b[5] === 0x5e &&
                b[6] === 0x56 &&
                b[7] === 0xd1 &&
                b[8] === 0x50 &&
                b[9] === 0xbc &&
                b[10] === 0x45 &&
                b[11] === 0x7b &&
                b[12] === 0x86 &&
                b[13] === 0x46 &&
                b[14] === 0xf1 &&
                b[15] === 0xcf &&
                b[16] === 0xb7 &&
                b[17] === 0xd0 &&
                b[18] === 0xbf &&
                b[19] === 0xd4
            )

            addr = 'rUC2XjZURBYQ8r6i5sqWnhtDmFFdJFobb9'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x7f &&
                b[1] === 0xf5 &&
                b[2] === 0x2d &&
                b[3] === 0xf4 &&
                b[4] === 0x98 &&
                b[5] === 0x2b &&
                b[6] === 0x7c &&
                b[7] === 0x14 &&
                b[8] === 0x7e &&
                b[9] === 0x9a &&
                b[10] === 0x8b &&
                b[11] === 0xeb &&
                b[12] === 0x1a &&
                b[13] === 0x53 &&
                b[14] === 0x60 &&
                b[15] === 0x34 &&
                b[16] === 0x95 &&
                b[17] === 0x42 &&
                b[18] === 0x4a &&
                b[19] === 0x44
            )

            addr = 'rKEsw1ExpKaukXyyPCxeZdAF5V68kPSAVZ'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0xc8 &&
                b[1] === 0x19 &&
                b[2] === 0xe6 &&
                b[3] === 0x2a &&
                b[4] === 0xdd &&
                b[5] === 0x42 &&
                b[6] === 0x48 &&
                b[7] === 0xd6 &&
                b[8] === 0x7d &&
                b[9] === 0xa5 &&
                b[10] === 0x56 &&
                b[11] === 0x66 &&
                b[12] === 0x55 &&
                b[13] === 0xb4 &&
                b[14] === 0xbf &&
                b[15] === 0xde &&
                b[16] === 0x99 &&
                b[17] === 0xcf &&
                b[18] === 0xed &&
                b[19] === 0x96
            )

            addr = 'rEXhVGVWdte28r1DUzfgKLjNiHi1Tn6R7X'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x9f &&
                b[1] === 0x41 &&
                b[2] === 0x26 &&
                b[3] === 0xa3 &&
                b[4] === 0x6d &&
                b[5] === 0x56 &&
                b[6] === 0x01 &&
                b[7] === 0xc8 &&
                b[8] === 0x09 &&
                b[9] === 0x63 &&
                b[10] === 0x76 &&
                b[11] === 0xed &&
                b[12] === 0x4c &&
                b[13] === 0x45 &&
                b[14] === 0x66 &&
                b[15] === 0x63 &&
                b[16] === 0x16 &&
                b[17] === 0xc9 &&
                b[18] === 0x5c &&
                b[19] === 0x80
            )

            addr = 'r3TcfPNEvidJ2LkNoFojffcCd7RgT53Thg'
            b = util_accid(addr)
            ASSERT(20 === b.length)
            ASSERT(
                b[0] === 0x51 &&
                b[1] === 0xd1 &&
                b[2] === 0x00 &&
                b[3] === 0xff &&
                b[4] === 0x0d &&
                b[5] === 0x92 &&
                b[6] === 0x18 &&
                b[7] === 0x73 &&
                b[8] === 0x80 &&
                b[9] === 0x30 &&
                b[10] === 0xc5 &&
                b[11] === 0x1a &&
                b[12] === 0xf2 &&
                b[13] === 0x9f &&
                b[14] === 0x52 &&
                b[15] === 0x8e &&
                b[16] === 0xb8 &&
                b[17] === 0x63 &&
                b[18] === 0x08 &&
                b[19] === 0x7c
            )

            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set util_accid"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test util_accid"), fee(XRP(1)));
    }

    void
    test_util_keylet(FeatureBitset features)
    {
        testcase("Test util_keylet");
        using namespace jtx;

        Env env{*this, features};
        // Env env{*this, envconfig(), features, nullptr,
        //     beast::severities::kTrace
        // };

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                trace('error', 0, false)
                rollback(x.toString(), code)
            }
            }

            const ASSERT_KL_EQ = (m, n) => {
            ASSERT(n[0] === m[0] && n[1] === m[1] && n[2] === m[2] && n[3] === m[3], 0)
            }

            //C5D0F34B0A1905BC3B29AA1BE139FE04D60C8694D3950A8D80251D10B563A822
            const ns = [
            0xc5, 0xd0, 0xf3, 0x4b, 0x0a, 0x19, 0x05, 0xbc, 0x3b, 0x29, 0xaa, 0x1b, 0xe1,
            0x39, 0xfe, 0x04, 0xd6, 0x0c, 0x86, 0x94, 0xd3, 0x95, 0x0a, 0x8d, 0x80, 0x25,
            0x1d, 0x10, 0xb5, 0x63, 0xa8, 0x22,
            ]

            // 2D0CB3CD60DA33B5AA7FEA321F111663EAED32481C6B700E484550F45AD96223
            const klkey = [
            0x00, 0x00, 0x2d, 0x0c, 0xb3, 0xcd, 0x60, 0xda, 0x33, 0xb5, 0xaa, 0x7f, 0xea,
            0x32, 0x1f, 0x11, 0x16, 0x63, 0xea, 0xed, 0x32, 0x48, 0x1c, 0x6b, 0x70, 0x0e,
            0x48, 0x45, 0x50, 0xf4, 0x5a, 0xd9, 0x62, 0x23,
            ]

            const key = [
            0x2d, 0x0c, 0xb3, 0xcd, 0x60, 0xda, 0x33, 0xb5, 0xaa, 0x7f, 0xea, 0x32, 0x1f,
            0x11, 0x16, 0x63, 0xea, 0xed, 0x32, 0x48, 0x1c, 0x6b, 0x70, 0x0e, 0x48, 0x45,
            0x50, 0xf4, 0x5a, 0xd9, 0x62, 0x23,
            ]

            const cur = [
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55,
            0x53, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00,
            ]

            //rB6v18pQ765Z9DH5RQsTFevoQPFmRtBqhT
            const a = [
            0x75, 0x6e, 0xde, 0x88, 0xa9, 0x07, 0xd4, 0xcc, 0xf3, 0x8d, 0x6a, 0xdb, 0x9f,
            0xc7, 0x94, 0x64, 0x19, 0xf0, 0xc4, 0x1d,
            ]
            //raKM1bZkGmASBqN5v2swrf2uAPJ32Cd8GV
            const b = [
            0x3a, 0x51, 0x8a, 0x22, 0x53, 0x81, 0x60, 0x84, 0x1c, 0x14, 0x32, 0xfe, 0x6f,
            0x3e, 0x6d, 0x6e, 0x76, 0x29, 0xfb, 0xba,
            ]

            const KEYLET_HOOK = 1
            const KEYLET_HOOK_STATE = 2
            const KEYLET_ACCOUNT = 3
            const KEYLET_AMENDMENTS = 4
            const KEYLET_CHILD = 5
            const KEYLET_SKIP = 6
            const KEYLET_FEES = 7
            const KEYLET_NEGATIVE_UNL = 8
            const KEYLET_LINE = 9
            const KEYLET_OFFER = 10
            const KEYLET_QUALITY = 11
            const KEYLET_EMITTED_DIR = 12
            const KEYLET_TICKET = 13
            const KEYLET_SIGNERS = 14
            const KEYLET_CHECK = 15
            const KEYLET_DEPOSIT_PREAUTH = 16
            const KEYLET_UNCHECKED = 17
            const KEYLET_OWNER_DIR = 18
            const KEYLET_PAGE = 19
            const KEYLET_ESCROW = 20
            const KEYLET_PAYCHAN = 21
            const KEYLET_EMITTED = 22
            const KEYLET_NFT_OFFER = 23
            const KEYLET_HOOK_DEFINITION = 24
            const KEYLET_HOOK_STATE_DIR = 25
            const KEYLET_URITOKEN = 26

            const INVALID_ARGUMENT = -7

            const Hook = (arg) => {
            // Test one of each type
            let e, ans

            // KEYLET_HOOK
            e = util_keylet(KEYLET_HOOK, a)
            ASSERT(34 === e.length, 1)
            ans = [
                0x00, 0x48, 0x6c, 0x4b, 0x29, 0xc6, 0x0f, 0x40, 0x5d, 0xb7, 0x6e, 0x87,
                0x65, 0x4a, 0x2f, 0x15, 0x4b, 0xab, 0x99, 0xc7, 0x62, 0x29, 0x80, 0x10,
                0xa1, 0x89, 0x78, 0x52, 0x90, 0x80, 0x2f, 0x78, 0xbd, 0xcc,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_HOOK_STATE
            e = util_keylet(KEYLET_HOOK_STATE, b, key, ns)
            ASSERT(34 === e.length, 2)
            ans = [
                0x00, 0x76, 0x28, 0xaf, 0xcc, 0x25, 0x0a, 0x64, 0x41, 0x8e, 0xb7, 0x83,
                0x68, 0xeb, 0x4e, 0xc5, 0x52, 0x4a, 0xeb, 0x97, 0x54, 0xab, 0xc1, 0x0b,
                0x13, 0x06, 0x7f, 0xfb, 0x9f, 0x4b, 0xd8, 0x38, 0x62, 0xf2,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_ACCOUNT
            e = util_keylet(KEYLET_ACCOUNT, b)
            ASSERT(34 === e.length, 3)
            ans = [
                0x00, 0x61, 0xc6, 0x55, 0xdd, 0x8d, 0x8e, 0xd3, 0xba, 0xb4, 0xa0, 0xf1,
                0xec, 0x2d, 0xa9, 0x99, 0xf4, 0x1b, 0xa6, 0x82, 0xc6, 0x84, 0xf9, 0x99,
                0x66, 0xb9, 0x3c, 0x9a, 0xc3, 0xe3, 0x5c, 0x9a, 0x81, 0x6d,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_AMENDMENTS
            e = util_keylet(KEYLET_AMENDMENTS)
            ASSERT(34 === e.length, 4)
            ans = [
                0x00, 0x66, 0x7d, 0xb0, 0x78, 0x8c, 0x02, 0x0f, 0x02, 0x78, 0x0a, 0x67,
                0x3d, 0xc7, 0x47, 0x57, 0xf2, 0x38, 0x23, 0xfa, 0x30, 0x14, 0xc1, 0x86,
                0x6e, 0x72, 0xcc, 0x4c, 0xd8, 0xb2, 0x26, 0xcd, 0x6e, 0xf4,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_CHILD
            e = util_keylet(KEYLET_CHILD, key)
            ASSERT(34 === e.length, 5)
            ans = [
                0x1c, 0xd2, 0x2d, 0x0c, 0xb3, 0xcd, 0x60, 0xda, 0x33, 0xb5, 0xaa, 0x7f,
                0xea, 0x32, 0x1f, 0x11, 0x16, 0x63, 0xea, 0xed, 0x32, 0x48, 0x1c, 0x6b,
                0x70, 0x0e, 0x48, 0x45, 0x50, 0xf4, 0x5a, 0xd9, 0x62, 0x23,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_SKIP
            e = util_keylet(KEYLET_SKIP)
            ASSERT(34 === e.length, 6)
            ans = [
                0x00, 0x68, 0xb4, 0x97, 0x9a, 0x36, 0xcd, 0xc7, 0xf3, 0xd3, 0xd5, 0xc3,
                0x1a, 0x4e, 0xae, 0x2a, 0xc7, 0xd7, 0x20, 0x9d, 0xda, 0x87, 0x75, 0x88,
                0xb9, 0xaf, 0xc6, 0x67, 0x99, 0x69, 0x2a, 0xb0, 0xd6, 0x6b,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_FEES
            e = util_keylet(KEYLET_FEES)
            ASSERT(34 === e.length, 7)
            ans = [
                0x00, 0x73, 0x4b, 0xc5, 0x0c, 0x9b, 0x0d, 0x85, 0x15, 0xd3, 0xea, 0xae,
                0x1e, 0x74, 0xb2, 0x9a, 0x95, 0x80, 0x43, 0x46, 0xc4, 0x91, 0xee, 0x1a,
                0x95, 0xbf, 0x25, 0xe4, 0xaa, 0xb8, 0x54, 0xa6, 0xa6, 0x51,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_NEGATIVE_UNL
            e = util_keylet(KEYLET_NEGATIVE_UNL)
            ASSERT(34 === e.length, 8)
            ans = [
                0x00, 0x4e, 0x2e, 0x8a, 0x59, 0xaa, 0x9d, 0x3b, 0x5b, 0x18, 0x6b, 0x0b,
                0x9e, 0x0f, 0x62, 0xe6, 0xc0, 0x25, 0x87, 0xca, 0x74, 0xa4, 0xd7, 0x78,
                0x93, 0x8e, 0x95, 0x7b, 0x63, 0x57, 0xd3, 0x64, 0xb2, 0x44,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_LINE
            e = util_keylet(KEYLET_LINE, a, b, cur)
            ASSERT(34 === e.length, 9)
            ans = [
                0x00, 0x72, 0x0e, 0xb8, 0x2a, 0xdd, 0x5e, 0x15, 0x59, 0x1b, 0xf6, 0xe3,
                0x6d, 0xbc, 0x3c, 0x12, 0xd3, 0x07, 0x6d, 0x43, 0xa8, 0x53, 0xf8, 0xf9,
                0xe8, 0xa7, 0xd8, 0x4f, 0xe1, 0xe9, 0x7a, 0x2a, 0xc7, 0x3d,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_OFFER
            e = util_keylet(KEYLET_OFFER, a, 1)
            ASSERT(34 === e.length, 10)
            ans = [
                0x00, 0x6f, 0x60, 0x14, 0x48, 0x80, 0x97, 0x5f, 0x76, 0x6a, 0xb2, 0x2c,
                0x32, 0x2f, 0x10, 0x8e, 0x03, 0x43, 0x51, 0xde, 0x89, 0x6c, 0xf4, 0x9f,
                0x6b, 0x4a, 0xc7, 0x2c, 0x54, 0xf7, 0x27, 0x29, 0x9b, 0xe8,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_OFFER
            e = util_keylet(KEYLET_OFFER, a, ns)
            ASSERT(34 === e.length, 11)
            ans = [
                0x00, 0x6f, 0x23, 0x61, 0x7f, 0x44, 0x91, 0x1c, 0xba, 0x3b, 0x5c, 0xbe,
                0xe9, 0x42, 0x22, 0xac, 0xa4, 0x29, 0xf4, 0xd6, 0x60, 0x01, 0xa8, 0xab,
                0x9b, 0x98, 0x5e, 0xb8, 0xb8, 0x42, 0x9f, 0x1e, 0x91, 0x4b,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_QUALITY
            klkey[0] = 0
            klkey[1] = 0x65
            e = util_keylet(KEYLET_QUALITY, klkey, 0, 1)
            ASSERT(INVALID_ARGUMENT === e, 12)
            // ans = [
            //   0x00, 0x6f, 0x23, 0x61, 0x7f, 0x44, 0x91, 0x1c, 0xba, 0x3b, 0x5c, 0xbe,
            //   0xe9, 0x42, 0x22, 0xac, 0xa4, 0x29, 0xf4, 0xd6, 0x60, 0x01, 0xa8, 0xab,
            //   0x9b, 0x98, 0x5e, 0xb8, 0xb8, 0x42, 0x9f, 0x1e, 0x91, 0x4b,
            // ]
            // ASSERT_KL_EQ(e, ans)

            // KEYLET_QUALITY
            klkey[1] = 0x64
            e = util_keylet(KEYLET_QUALITY, klkey, 0, 1)
            ASSERT(34 === e.length, 13)
            ans = [
                0x00, 0x64, 0x2d, 0x0c, 0xb3, 0xcd, 0x60, 0xda, 0x33, 0xb5, 0xaa, 0x7f,
                0xea, 0x32, 0x1f, 0x11, 0x16, 0x63, 0xea, 0xed, 0x32, 0x48, 0x1c, 0x6b,
                0x70, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_EMITTED_DIR
            e = util_keylet(KEYLET_EMITTED_DIR)
            ASSERT(34 === e.length, 14)
            ans = [
                0x00, 0x64, 0xb4, 0xde, 0x82, 0x30, 0x55, 0xd0, 0x0b, 0xc1, 0x2c, 0xd7,
                0x8f, 0xe1, 0xaa, 0xf7, 0x4e, 0xe6, 0x06, 0x21, 0x95, 0xb2, 0x62, 0x9f,
                0x49, 0xa2, 0x59, 0x15, 0xa3, 0x9c, 0x64, 0xbe, 0x19, 0x00,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_SIGNERS
            e = util_keylet(KEYLET_SIGNERS, a)
            ASSERT(34 === e.length, 15)
            ans = [
                0x00, 0x53, 0xdf, 0x8f, 0xf0, 0xce, 0x41, 0x1a, 0x3b, 0x8f, 0x1b, 0xb5,
                0xbb, 0x32, 0x78, 0x17, 0x15, 0xd6, 0x77, 0x42, 0xf5, 0xb5, 0x63, 0xb8,
                0x77, 0xb3, 0x3b, 0x07, 0x76, 0xf6, 0xf7, 0xbc, 0xda, 0x1d,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_CHECK
            e = util_keylet(KEYLET_CHECK, a, 1)
            ASSERT(34 === e.length, 16)
            ans = [
                0x00, 0x43, 0x08, 0x1f, 0x26, 0xff, 0x79, 0x1a, 0xf7, 0x54, 0x26, 0xed,
                0xf9, 0xeb, 0x08, 0x44, 0x85, 0x28, 0x58, 0x2c, 0xb1, 0xa4, 0xef, 0x4f,
                0xd0, 0xb4, 0x49, 0x9b, 0x76, 0x82, 0xe7, 0x69, 0xa6, 0xb5,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_CHECK
            e = util_keylet(KEYLET_CHECK, a, ns)
            ASSERT(34 === e.length, 17)
            ans = [
                0x00, 0x43, 0x94, 0xe3, 0x6f, 0x0d, 0xd3, 0xed, 0xc0, 0x2c, 0x49, 0xa5,
                0xaa, 0x0e, 0xcc, 0x49, 0x18, 0x39, 0x92, 0xab, 0x57, 0xc3, 0x2d, 0x9e,
                0x45, 0x51, 0x04, 0x78, 0x49, 0x49, 0xd1, 0xe6, 0xd2, 0x01,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_DEPOSIT_PREAUTH
            e = util_keylet(KEYLET_DEPOSIT_PREAUTH, a, b)
            ASSERT(34 === e.length, 18)
            ans = [
                0x00, 0x70, 0x88, 0x90, 0x0f, 0x27, 0x66, 0x57, 0xbc, 0xc0, 0x5d, 0xa1,
                0x67, 0x40, 0xab, 0x9d, 0x33, 0x01, 0x8e, 0x45, 0x71, 0x7b, 0x0e, 0xc4,
                0x2e, 0x4d, 0x11, 0xbd, 0x6d, 0xbd, 0x94, 0x03, 0x48, 0xe0,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_UNCHECKED
            klkey[0] = 0
            klkey[1] = 0
            e = util_keylet(KEYLET_UNCHECKED, key)
            ASSERT(34 === e.length, 19)
            ans = klkey
            ASSERT_KL_EQ(e, ans)

            // KEYLET_OWNER_DIR
            e = util_keylet(KEYLET_OWNER_DIR, a)
            ASSERT(34 === e.length, 20)
            ans = [
                0x00, 0x64, 0xc8, 0x5e, 0x01, 0x29, 0x06, 0x7b, 0x75, 0xad, 0x30, 0xb0,
                0xaa, 0x1c, 0xc2, 0x5b, 0x0a, 0x82, 0xc7, 0xf9, 0xaa, 0xbd, 0xee, 0x05,
                0xff, 0x01, 0x66, 0x69, 0xef, 0x9d, 0x82, 0xdc, 0xec, 0x30,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_PAGE
            e = util_keylet(KEYLET_PAGE, ns, 0, 1)
            ASSERT(34 === e.length, 21)
            ans = [
                0x00, 0x64, 0x61, 0xe6, 0x05, 0x1a, 0xb0, 0x49, 0x89, 0x2e, 0x75, 0xc9,
                0x3d, 0x67, 0xfb, 0x7a, 0x63, 0xf1, 0xef, 0x56, 0xdd, 0xaf, 0x3e, 0x6b,
                0x43, 0x6f, 0x57, 0x6e, 0x8c, 0x01, 0x81, 0x98, 0x2e, 0x48,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_ESCROW
            e = util_keylet(KEYLET_ESCROW, a, 1)
            ASSERT(34 === e.length, 22)
            ans = [
                0x00, 0x75, 0x13, 0xef, 0x04, 0xcd, 0x33, 0x6a, 0xad, 0xf6, 0x3d, 0x0c,
                0x7e, 0x05, 0x6c, 0x84, 0x9a, 0x7c, 0xf6, 0x72, 0x5e, 0x99, 0xbc, 0x93,
                0x80, 0x1e, 0xf5, 0x78, 0xa0, 0x32, 0x72, 0x5b, 0x84, 0xfe,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_ESCROW
            e = util_keylet(KEYLET_ESCROW, a, ns)
            ASSERT(34 === e.length, 23)
            ans = [
                0x00, 0x75, 0xc1, 0xc6, 0xc5, 0x23, 0x74, 0x87, 0x12, 0x56, 0xaa, 0x7a,
                0x1f, 0xb3, 0x29, 0x7a, 0x0a, 0x55, 0x88, 0x7d, 0x16, 0x6a, 0xcf, 0x85,
                0x28, 0x59, 0x88, 0xc2, 0xda, 0x81, 0x7f, 0x03, 0x90, 0x43,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_PAYCHAN
            e = util_keylet(KEYLET_PAYCHAN, a, b, 1)
            ASSERT(34 === e.length, 24)
            ans = [
                0x00, 0x78, 0xed, 0x04, 0xce, 0x27, 0x20, 0x21, 0x55, 0x2b, 0xbf, 0xa1,
                0xe5, 0xff, 0xbb, 0x53, 0xb6, 0x45, 0xa2, 0xff, 0x8a, 0x44, 0x66, 0xd5,
                0x76, 0x24, 0xb5, 0x71, 0xe6, 0x44, 0x9e, 0xeb, 0xfc, 0x5a,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_PAYCHAN
            e = util_keylet(KEYLET_PAYCHAN, a, b, ns)
            ASSERT(34 === e.length, 25)
            ans = [
                0x00, 0x78, 0x7d, 0xe1, 0x01, 0xf6, 0x2b, 0xb0, 0x55, 0x80, 0xb9, 0xd6,
                0xb0, 0x3f, 0x3b, 0xb0, 0x01, 0xbd, 0xe6, 0x9b, 0x89, 0x0f, 0x8a, 0xcd,
                0xbe, 0x71, 0x73, 0x5e, 0xc3, 0x63, 0xf8, 0xc5, 0x4b, 0x9b,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_EMITTED
            e = util_keylet(KEYLET_EMITTED, ns)
            ASSERT(34 === e.length, 26)
            ans = [
                0x00, 0x45, 0xf3, 0x51, 0x2d, 0x1c, 0x80, 0xa3, 0xc0, 0xb1, 0x46, 0x04,
                0xe1, 0xad, 0xdb, 0x90, 0x1c, 0x66, 0x32, 0x10, 0x08, 0xcc, 0xd0, 0xab,
                0xd2, 0xdb, 0xbe, 0xc4, 0x08, 0xa6, 0x0f, 0x6a, 0x62, 0xe9,
            ]
            ASSERT_KL_EQ(e, ans)

            // KEYLET_NFT_OFFER
            e = util_keylet(KEYLET_NFT_OFFER, a, 1)
            ASSERT(34 === e.length, 27)

            // KEYLET_NFT_OFFER
            e = util_keylet(KEYLET_NFT_OFFER, a, ns)
            ASSERT(34 === e.length, 28)

            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set util_keylet"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test util_keylet"), fee(XRP(1)));
    }

    void
    test_util_raddr(FeatureBitset features)
    {
        testcase("Test util_raddr");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                rollback(x.toString(), code)
            }
            }

            function toHex(v) {
            const a = []
            for (let i = 0; i < v.length; i++) {
                a.push(v.charCodeAt(i))
            }
            return a
            }

            const Hook = (arg) => {
            let raw = [
                0x6b, 0x30, 0xe2, 0x94, 0xf3, 0x40, 0x3f, 0xf8, 0x7c, 0xef, 0x9e, 0x72,
                0x21, 0x7f, 0xf7, 0xeb, 0x4a, 0x6a, 0x43, 0xf4,
            ]
            let addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x77 &&
                addr[2] === 0x6d &&
                addr[3] === 0x6d &&
                addr[4] === 0x31 &&
                addr[5] === 0x33 &&
                addr[6] === 0x70 &&
                addr[7] === 0x37 &&
                addr[8] === 0x56 &&
                addr[9] === 0x67 &&
                addr[10] === 0x36 &&
                addr[11] === 0x4b &&
                addr[12] === 0x6d &&
                addr[13] === 0x6e &&
                addr[14] === 0x71 &&
                addr[15] === 0x4b &&
                addr[16] === 0x52 &&
                addr[17] === 0x77 &&
                addr[18] === 0x44 &&
                addr[19] === 0x7a &&
                addr[20] === 0x78 &&
                addr[21] === 0x76 &&
                addr[22] === 0x69 &&
                addr[23] === 0x35 &&
                addr[24] === 0x58 &&
                addr[25] === 0x70 &&
                addr[26] === 0x36 &&
                addr[27] === 0x77 &&
                addr[28] === 0x6e &&
                addr[29] === 0x48 &&
                addr[30] === 0x4d &&
                addr[31] === 0x44 &&
                addr[32] === 0x44 &&
                addr[33] === 0x68
            )

            raw = [
                0xe4, 0x0f, 0xa3, 0x4e, 0x3e, 0x66, 0x15, 0x36, 0x64, 0x89, 0x4f, 0xcb,
                0xfb, 0xfc, 0xfe, 0x2d, 0x2d, 0x19, 0x0d, 0x69,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4d &&
                addr[2] === 0x38 &&
                addr[3] === 0x31 &&
                addr[4] === 0x6f &&
                addr[5] === 0x48 &&
                addr[6] === 0x77 &&
                addr[7] === 0x68 &&
                addr[8] === 0x37 &&
                addr[9] === 0x35 &&
                addr[10] === 0x39 &&
                addr[11] === 0x34 &&
                addr[12] === 0x6a &&
                addr[13] === 0x48 &&
                addr[14] === 0x38 &&
                addr[15] === 0x70 &&
                addr[16] === 0x36 &&
                addr[17] === 0x31 &&
                addr[18] === 0x57 &&
                addr[19] === 0x65 &&
                addr[20] === 0x31 &&
                addr[21] === 0x73 &&
                addr[22] === 0x64 &&
                addr[23] === 0x58 &&
                addr[24] === 0x46 &&
                addr[25] === 0x42 &&
                addr[26] === 0x35 &&
                addr[27] === 0x48 &&
                addr[28] === 0x52 &&
                addr[29] === 0x52 &&
                addr[30] === 0x79 &&
                addr[31] === 0x4b &&
                addr[32] === 0x76 &&
                addr[33] === 0x4a
            )

            raw = [
                0x0c, 0x90, 0x4b, 0x4f, 0xa5, 0x59, 0xbf, 0x10, 0x6a, 0xae, 0xb5, 0x28,
                0x6c, 0x94, 0xba, 0x34, 0x18, 0xfd, 0xf3, 0x53,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x70 &&
                addr[2] === 0x39 &&
                addr[3] === 0x52 &&
                addr[4] === 0x79 &&
                addr[5] === 0x73 &&
                addr[6] === 0x42 &&
                addr[7] === 0x63 &&
                addr[8] === 0x55 &&
                addr[9] === 0x42 &&
                addr[10] === 0x59 &&
                addr[11] === 0x63 &&
                addr[12] === 0x76 &&
                addr[13] === 0x4a &&
                addr[14] === 0x4a &&
                addr[15] === 0x4b &&
                addr[16] === 0x38 &&
                addr[17] === 0x54 &&
                addr[18] === 0x48 &&
                addr[19] === 0x45 &&
                addr[20] === 0x79 &&
                addr[21] === 0x6f &&
                addr[22] === 0x79 &&
                addr[23] === 0x74 &&
                addr[24] === 0x74 &&
                addr[25] === 0x6b &&
                addr[26] === 0x57 &&
                addr[27] === 0x58 &&
                addr[28] === 0x39 &&
                addr[29] === 0x4b &&
                addr[30] === 0x52 &&
                addr[31] === 0x62 &&
                addr[32] === 0x39 &&
                addr[33] === 0x4d
            )

            raw = [
                0x75, 0x82, 0xfb, 0x27, 0x10, 0x8c, 0x0f, 0x9a, 0xf2, 0x67, 0x35, 0xcc,
                0x7b, 0x22, 0x6b, 0xd2, 0x2f, 0xdf, 0x4f, 0x92,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x42 &&
                addr[2] === 0x35 &&
                addr[3] === 0x4c &&
                addr[4] === 0x79 &&
                addr[5] === 0x77 &&
                addr[6] === 0x6b &&
                addr[7] === 0x54 &&
                addr[8] === 0x4c &&
                addr[9] === 0x31 &&
                addr[10] === 0x34 &&
                addr[11] === 0x51 &&
                addr[12] === 0x64 &&
                addr[13] === 0x55 &&
                addr[14] === 0x64 &&
                addr[15] === 0x77 &&
                addr[16] === 0x43 &&
                addr[17] === 0x78 &&
                addr[18] === 0x70 &&
                addr[19] === 0x6e &&
                addr[20] === 0x65 &&
                addr[21] === 0x46 &&
                addr[22] === 0x32 &&
                addr[23] === 0x7a &&
                addr[24] === 0x63 &&
                addr[25] === 0x7a &&
                addr[26] === 0x46 &&
                addr[27] === 0x66 &&
                addr[28] === 0x44 &&
                addr[29] === 0x7a &&
                addr[30] === 0x57 &&
                addr[31] === 0x46 &&
                addr[32] === 0x38 &&
                addr[33] === 0x50
            )

            raw = [
                0x6c, 0xb6, 0x51, 0x1f, 0x20, 0xec, 0xca, 0x1e, 0x98, 0x03, 0xfc, 0xfa,
                0x6f, 0x3e, 0x56, 0x75, 0x72, 0x29, 0x51, 0x97,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x77 &&
                addr[2] === 0x75 &&
                addr[3] === 0x46 &&
                addr[4] === 0x50 &&
                addr[5] === 0x4b &&
                addr[6] === 0x34 &&
                addr[7] === 0x48 &&
                addr[8] === 0x51 &&
                addr[9] === 0x4e &&
                addr[10] === 0x73 &&
                addr[11] === 0x59 &&
                addr[12] === 0x42 &&
                addr[13] === 0x47 &&
                addr[14] === 0x74 &&
                addr[15] === 0x46 &&
                addr[16] === 0x52 &&
                addr[17] === 0x4b &&
                addr[18] === 0x77 &&
                addr[19] === 0x45 &&
                addr[20] === 0x6d &&
                addr[21] === 0x75 &&
                addr[22] === 0x41 &&
                addr[23] === 0x68 &&
                addr[24] === 0x63 &&
                addr[25] === 0x4b &&
                addr[26] === 0x63 &&
                addr[27] === 0x48 &&
                addr[28] === 0x39 &&
                addr[29] === 0x5a &&
                addr[30] === 0x32 &&
                addr[31] === 0x59 &&
                addr[32] === 0x7a &&
                addr[33] === 0x58
            )

            raw = [
                0xa5, 0x31, 0x30, 0x28, 0xf9, 0x62, 0xe4, 0x80, 0x48, 0x94, 0x3b, 0x1a,
                0x59, 0xbb, 0x5e, 0x36, 0x96, 0xb3, 0x44, 0x35,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x47 &&
                addr[2] === 0x68 &&
                addr[3] === 0x54 &&
                addr[4] === 0x52 &&
                addr[5] === 0x4a &&
                addr[6] === 0x5a &&
                addr[7] === 0x31 &&
                addr[8] === 0x56 &&
                addr[9] === 0x4d &&
                addr[10] === 0x51 &&
                addr[11] === 0x74 &&
                addr[12] === 0x36 &&
                addr[13] === 0x6a &&
                addr[14] === 0x44 &&
                addr[15] === 0x72 &&
                addr[16] === 0x66 &&
                addr[17] === 0x4e &&
                addr[18] === 0x63 &&
                addr[19] === 0x6f &&
                addr[20] === 0x4a &&
                addr[21] === 0x34 &&
                addr[22] === 0x39 &&
                addr[23] === 0x6a &&
                addr[24] === 0x34 &&
                addr[25] === 0x43 &&
                addr[26] === 0x67 &&
                addr[27] === 0x71 &&
                addr[28] === 0x4b &&
                addr[29] === 0x6d &&
                addr[30] === 0x52 &&
                addr[31] === 0x32 &&
                addr[32] === 0x6f &&
                addr[33] === 0x36
            )

            raw = [
                0xbf, 0x04, 0x6c, 0x79, 0xa0, 0x96, 0xde, 0x80, 0x66, 0xd3, 0x74, 0xc8,
                0xdf, 0x94, 0x5f, 0x89, 0xf2, 0x3e, 0x9a, 0x27,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4a &&
                addr[2] === 0x52 &&
                addr[3] === 0x72 &&
                addr[4] === 0x34 &&
                addr[5] === 0x72 &&
                addr[6] === 0x4c &&
                addr[7] === 0x32 &&
                addr[8] === 0x43 &&
                addr[9] === 0x4a &&
                addr[10] === 0x48 &&
                addr[11] === 0x67 &&
                addr[12] === 0x46 &&
                addr[13] === 0x47 &&
                addr[14] === 0x56 &&
                addr[15] === 0x67 &&
                addr[16] === 0x31 &&
                addr[17] === 0x6a &&
                addr[18] === 0x61 &&
                addr[19] === 0x66 &&
                addr[20] === 0x39 &&
                addr[21] === 0x4a &&
                addr[22] === 0x48 &&
                addr[23] === 0x51 &&
                addr[24] === 0x70 &&
                addr[25] === 0x56 &&
                addr[26] === 0x6d &&
                addr[27] === 0x68 &&
                addr[28] === 0x76 &&
                addr[29] === 0x45 &&
                addr[30] === 0x37 &&
                addr[31] === 0x68 &&
                addr[32] === 0x61 &&
                addr[33] === 0x62
            )

            raw = [
                0xe2, 0x07, 0xab, 0xd3, 0x7d, 0xc2, 0xcd, 0xd4, 0x6d, 0x15, 0x7b, 0x67,
                0x5a, 0xc8, 0x3e, 0x0e, 0x05, 0x9b, 0x08, 0x62,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4d &&
                addr[2] === 0x63 &&
                addr[3] === 0x33 &&
                addr[4] === 0x75 &&
                addr[5] === 0x4d &&
                addr[6] === 0x6b &&
                addr[7] === 0x4b &&
                addr[8] === 0x31 &&
                addr[9] === 0x62 &&
                addr[10] === 0x62 &&
                addr[11] === 0x32 &&
                addr[12] === 0x64 &&
                addr[13] === 0x4b &&
                addr[14] === 0x7a &&
                addr[15] === 0x5a &&
                addr[16] === 0x64 &&
                addr[17] === 0x56 &&
                addr[18] === 0x71 &&
                addr[19] === 0x35 &&
                addr[20] === 0x75 &&
                addr[21] === 0x59 &&
                addr[22] === 0x54 &&
                addr[23] === 0x55 &&
                addr[24] === 0x37 &&
                addr[25] === 0x5a &&
                addr[26] === 0x76 &&
                addr[27] === 0x4e &&
                addr[28] === 0x45 &&
                addr[29] === 0x41 &&
                addr[30] === 0x32 &&
                addr[31] === 0x33 &&
                addr[32] === 0x67 &&
                addr[33] === 0x44
            )

            raw = [
                0x2a, 0x56, 0x74, 0x25, 0x84, 0x8d, 0x41, 0x6d, 0xf1, 0x06, 0x01, 0x6c,
                0x2a, 0xb1, 0x13, 0xc3, 0x1e, 0x65, 0x63, 0x80,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x68 &&
                addr[2] === 0x69 &&
                addr[3] === 0x69 &&
                addr[4] === 0x41 &&
                addr[5] === 0x78 &&
                addr[6] === 0x79 &&
                addr[7] === 0x59 &&
                addr[8] === 0x41 &&
                addr[9] === 0x43 &&
                addr[10] === 0x67 &&
                addr[11] === 0x45 &&
                addr[12] === 0x52 &&
                addr[13] === 0x4b &&
                addr[14] === 0x47 &&
                addr[15] === 0x51 &&
                addr[16] === 0x4d &&
                addr[17] === 0x72 &&
                addr[18] === 0x53 &&
                addr[19] === 0x5a &&
                addr[20] === 0x57 &&
                addr[21] === 0x43 &&
                addr[22] === 0x74 &&
                addr[23] === 0x6b &&
                addr[24] === 0x4d &&
                addr[25] === 0x6f &&
                addr[26] === 0x69 &&
                addr[27] === 0x58 &&
                addr[28] === 0x48 &&
                addr[29] === 0x34 &&
                addr[30] === 0x64 &&
                addr[31] === 0x48 &&
                addr[32] === 0x6e &&
                addr[33] === 0x6f
            )

            raw = [
                0x24, 0xbb, 0xa9, 0xc3, 0x95, 0x74, 0x9a, 0x88, 0x04, 0x12, 0xc0, 0x91,
                0xe7, 0x13, 0x41, 0x7f, 0x9a, 0xd5, 0x74, 0x43,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x68 &&
                addr[2] === 0x4d &&
                addr[3] === 0x4e &&
                addr[4] === 0x33 &&
                addr[5] === 0x79 &&
                addr[6] === 0x4e &&
                addr[7] === 0x50 &&
                addr[8] === 0x4e &&
                addr[9] === 0x74 &&
                addr[10] === 0x4b &&
                addr[11] === 0x70 &&
                addr[12] === 0x78 &&
                addr[13] === 0x6b &&
                addr[14] === 0x71 &&
                addr[15] === 0x4c &&
                addr[16] === 0x78 &&
                addr[17] === 0x51 &&
                addr[18] === 0x32 &&
                addr[19] === 0x63 &&
                addr[20] === 0x33 &&
                addr[21] === 0x55 &&
                addr[22] === 0x68 &&
                addr[23] === 0x6f &&
                addr[24] === 0x41 &&
                addr[25] === 0x7a &&
                addr[26] === 0x66 &&
                addr[27] === 0x75 &&
                addr[28] === 0x59 &&
                addr[29] === 0x35 &&
                addr[30] === 0x75 &&
                addr[31] === 0x35 &&
                addr[32] === 0x4a &&
                addr[33] === 0x7a
            )

            raw = [
                0x49, 0x53, 0x9e, 0x65, 0x21, 0x8a, 0xcf, 0x37, 0x85, 0x2b, 0xff, 0x87,
                0x14, 0x76, 0xda, 0x1a, 0x62, 0x3a, 0xea, 0x80,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x66 &&
                addr[2] === 0x67 &&
                addr[3] === 0x35 &&
                addr[4] === 0x56 &&
                addr[5] === 0x41 &&
                addr[6] === 0x44 &&
                addr[7] === 0x41 &&
                addr[8] === 0x4d &&
                addr[9] === 0x4d &&
                addr[10] === 0x42 &&
                addr[11] === 0x78 &&
                addr[12] === 0x46 &&
                addr[13] === 0x51 &&
                addr[14] === 0x76 &&
                addr[15] === 0x44 &&
                addr[16] === 0x78 &&
                addr[17] === 0x5a &&
                addr[18] === 0x54 &&
                addr[19] === 0x32 &&
                addr[20] === 0x52 &&
                addr[21] === 0x6a &&
                addr[22] === 0x55 &&
                addr[23] === 0x64 &&
                addr[24] === 0x47 &&
                addr[25] === 0x69 &&
                addr[26] === 0x64 &&
                addr[27] === 0x59 &&
                addr[28] === 0x61 &&
                addr[29] === 0x35 &&
                addr[30] === 0x76 &&
                addr[31] === 0x69 &&
                addr[32] === 0x37 &&
                addr[33] === 0x5a
            )

            raw = [
                0xe7, 0xd3, 0x03, 0xbc, 0xae, 0xbd, 0x62, 0x20, 0xae, 0xc2, 0xe1, 0x7e,
                0x0b, 0xff, 0xdc, 0x21, 0x24, 0x34, 0x50, 0x82,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x34 &&
                addr[2] === 0x33 &&
                addr[3] === 0x6d &&
                addr[4] === 0x31 &&
                addr[5] === 0x31 &&
                addr[6] === 0x66 &&
                addr[7] === 0x74 &&
                addr[8] === 0x36 &&
                addr[9] === 0x79 &&
                addr[10] === 0x6f &&
                addr[11] === 0x50 &&
                addr[12] === 0x69 &&
                addr[13] === 0x6d &&
                addr[14] === 0x36 &&
                addr[15] === 0x56 &&
                addr[16] === 0x44 &&
                addr[17] === 0x78 &&
                addr[18] === 0x64 &&
                addr[19] === 0x55 &&
                addr[20] === 0x76 &&
                addr[21] === 0x63 &&
                addr[22] === 0x46 &&
                addr[23] === 0x77 &&
                addr[24] === 0x36 &&
                addr[25] === 0x57 &&
                addr[26] === 0x38 &&
                addr[27] === 0x41 &&
                addr[28] === 0x77 &&
                addr[29] === 0x78 &&
                addr[30] === 0x78 &&
                addr[31] === 0x4b &&
                addr[32] === 0x35 &&
                addr[33] === 0x58
            )

            raw = [
                0xc3, 0xe1, 0x5f, 0xab, 0xc0, 0x0a, 0x79, 0x73, 0x71, 0xd0, 0x55, 0xc0,
                0x80, 0x79, 0xae, 0x45, 0x71, 0x0f, 0xa0, 0x97,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4a &&
                addr[2] === 0x69 &&
                addr[3] === 0x35 &&
                addr[4] === 0x6b &&
                addr[5] === 0x65 &&
                addr[6] === 0x58 &&
                addr[7] === 0x79 &&
                addr[8] === 0x33 &&
                addr[9] === 0x31 &&
                addr[10] === 0x4a &&
                addr[11] === 0x31 &&
                addr[12] === 0x34 &&
                addr[13] === 0x52 &&
                addr[14] === 0x4b &&
                addr[15] === 0x73 &&
                addr[16] === 0x4e &&
                addr[17] === 0x59 &&
                addr[18] === 0x41 &&
                addr[19] === 0x46 &&
                addr[20] === 0x31 &&
                addr[21] === 0x51 &&
                addr[22] === 0x36 &&
                addr[23] === 0x6a &&
                addr[24] === 0x4d &&
                addr[25] === 0x56 &&
                addr[26] === 0x69 &&
                addr[27] === 0x45 &&
                addr[28] === 0x52 &&
                addr[29] === 0x55 &&
                addr[30] === 0x51 &&
                addr[31] === 0x71 &&
                addr[32] === 0x59 &&
                addr[33] === 0x36
            )

            raw = [
                0x95, 0x15, 0x7f, 0x2a, 0xaf, 0xe3, 0x2f, 0x7f, 0x2e, 0xf1, 0xa0, 0xf5,
                0xea, 0xc3, 0x07, 0x06, 0xa1, 0xd3, 0xf5, 0xd9,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4e &&
                addr[2] === 0x62 &&
                addr[3] === 0x48 &&
                addr[4] === 0x53 &&
                addr[5] === 0x55 &&
                addr[6] === 0x6d &&
                addr[7] === 0x66 &&
                addr[8] === 0x4b &&
                addr[9] === 0x61 &&
                addr[10] === 0x34 &&
                addr[11] === 0x71 &&
                addr[12] === 0x31 &&
                addr[13] === 0x51 &&
                addr[14] === 0x78 &&
                addr[15] === 0x44 &&
                addr[16] === 0x45 &&
                addr[17] === 0x5a &&
                addr[18] === 0x4c &&
                addr[19] === 0x6e &&
                addr[20] === 0x54 &&
                addr[21] === 0x67 &&
                addr[22] === 0x46 &&
                addr[23] === 0x56 &&
                addr[24] === 0x45 &&
                addr[25] === 0x4c &&
                addr[26] === 0x78 &&
                addr[27] === 0x39 &&
                addr[28] === 0x6d &&
                addr[29] === 0x57 &&
                addr[30] === 0x45 &&
                addr[31] === 0x43 &&
                addr[32] === 0x6b &&
                addr[33] === 0x41
            )

            raw = [
                0xf0, 0xec, 0x0f, 0x86, 0x31, 0xbb, 0x2c, 0xbf, 0x8f, 0xb7, 0xe3, 0x1c,
                0x82, 0xa0, 0xa3, 0x50, 0xd5, 0xe0, 0xfe, 0x6b,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x34 &&
                addr[2] === 0x78 &&
                addr[3] === 0x31 &&
                addr[4] === 0x78 &&
                addr[5] === 0x46 &&
                addr[6] === 0x32 &&
                addr[7] === 0x42 &&
                addr[8] === 0x47 &&
                addr[9] === 0x73 &&
                addr[10] === 0x42 &&
                addr[11] === 0x41 &&
                addr[12] === 0x7a &&
                addr[13] === 0x77 &&
                addr[14] === 0x77 &&
                addr[15] === 0x61 &&
                addr[16] === 0x4b &&
                addr[17] === 0x61 &&
                addr[18] === 0x70 &&
                addr[19] === 0x4b &&
                addr[20] === 0x6f &&
                addr[21] === 0x6f &&
                addr[22] === 0x35 &&
                addr[23] === 0x57 &&
                addr[24] === 0x65 &&
                addr[25] === 0x31 &&
                addr[26] === 0x59 &&
                addr[27] === 0x53 &&
                addr[28] === 0x6e &&
                addr[29] === 0x52 &&
                addr[30] === 0x50 &&
                addr[31] === 0x57 &&
                addr[32] === 0x75 &&
                addr[33] === 0x39
            )

            raw = [
                0x8d, 0xa4, 0x7d, 0xab, 0xd1, 0x19, 0xdc, 0xc4, 0x45, 0x5f, 0xaa, 0xe2,
                0x1c, 0x39, 0xca, 0x19, 0x34, 0xf1, 0x86, 0x16,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x44 &&
                addr[2] === 0x75 &&
                addr[3] === 0x41 &&
                addr[4] === 0x4c &&
                addr[5] === 0x6e &&
                addr[6] === 0x52 &&
                addr[7] === 0x79 &&
                addr[8] === 0x76 &&
                addr[9] === 0x77 &&
                addr[10] === 0x38 &&
                addr[11] === 0x36 &&
                addr[12] === 0x43 &&
                addr[13] === 0x63 &&
                addr[14] === 0x55 &&
                addr[15] === 0x5a &&
                addr[16] === 0x39 &&
                addr[17] === 0x74 &&
                addr[18] === 0x52 &&
                addr[19] === 0x55 &&
                addr[20] === 0x45 &&
                addr[21] === 0x6d &&
                addr[22] === 0x35 &&
                addr[23] === 0x43 &&
                addr[24] === 0x61 &&
                addr[25] === 0x65 &&
                addr[26] === 0x50 &&
                addr[27] === 0x46 &&
                addr[28] === 0x66 &&
                addr[29] === 0x33 &&
                addr[30] === 0x74 &&
                addr[31] === 0x36 &&
                addr[32] === 0x61 &&
                addr[33] === 0x31
            )

            raw = [
                0xa9, 0x94, 0x5a, 0xe3, 0x5a, 0x43, 0xad, 0xbe, 0xba, 0xa4, 0x13, 0x94,
                0xf5, 0xdc, 0x8f, 0x3b, 0x01, 0x14, 0xff, 0xfe,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x47 &&
                addr[2] === 0x54 &&
                addr[3] === 0x65 &&
                addr[4] === 0x76 &&
                addr[5] === 0x4a &&
                addr[6] === 0x71 &&
                addr[7] === 0x76 &&
                addr[8] === 0x6b &&
                addr[9] === 0x5a &&
                addr[10] === 0x76 &&
                addr[11] === 0x48 &&
                addr[12] === 0x73 &&
                addr[13] === 0x58 &&
                addr[14] === 0x5a &&
                addr[15] === 0x71 &&
                addr[16] === 0x55 &&
                addr[17] === 0x78 &&
                addr[18] === 0x43 &&
                addr[19] === 0x48 &&
                addr[20] === 0x4c &&
                addr[21] === 0x68 &&
                addr[22] === 0x73 &&
                addr[23] === 0x43 &&
                addr[24] === 0x53 &&
                addr[25] === 0x38 &&
                addr[26] === 0x57 &&
                addr[27] === 0x68 &&
                addr[28] === 0x79 &&
                addr[29] === 0x4d &&
                addr[30] === 0x74 &&
                addr[31] === 0x7a &&
                addr[32] === 0x6e &&
                addr[33] === 0x5a
            )

            raw = [
                0xc1, 0xe6, 0x7f, 0x17, 0xd3, 0x00, 0x9b, 0x80, 0x6c, 0x85, 0x74, 0x9c,
                0x80, 0x40, 0xaf, 0x64, 0xce, 0x09, 0x7e, 0x2e,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4a &&
                addr[2] === 0x67 &&
                addr[3] === 0x45 &&
                addr[4] === 0x59 &&
                addr[5] === 0x55 &&
                addr[6] === 0x37 &&
                addr[7] === 0x36 &&
                addr[8] === 0x45 &&
                addr[9] === 0x55 &&
                addr[10] === 0x34 &&
                addr[11] === 0x59 &&
                addr[12] === 0x41 &&
                addr[13] === 0x5a &&
                addr[14] === 0x41 &&
                addr[15] === 0x44 &&
                addr[16] === 0x79 &&
                addr[17] === 0x61 &&
                addr[18] === 0x37 &&
                addr[19] === 0x6b &&
                addr[20] === 0x37 &&
                addr[21] === 0x62 &&
                addr[22] === 0x71 &&
                addr[23] === 0x38 &&
                addr[24] === 0x4e &&
                addr[25] === 0x76 &&
                addr[26] === 0x64 &&
                addr[27] === 0x65 &&
                addr[28] === 0x4b &&
                addr[29] === 0x41 &&
                addr[30] === 0x48 &&
                addr[31] === 0x69 &&
                addr[32] === 0x32 &&
                addr[33] === 0x50
            )

            raw = [
                0xd8, 0x74, 0xcf, 0x61, 0x0d, 0x97, 0xe4, 0xab, 0x76, 0xa0, 0x70, 0x60,
                0xb7, 0xc5, 0x9c, 0x9a, 0x88, 0x86, 0x62, 0xaa,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4c &&
                addr[2] === 0x6a &&
                addr[3] === 0x57 &&
                addr[4] === 0x74 &&
                addr[5] === 0x59 &&
                addr[6] === 0x52 &&
                addr[7] === 0x61 &&
                addr[8] === 0x6e &&
                addr[9] === 0x61 &&
                addr[10] === 0x6b &&
                addr[11] === 0x33 &&
                addr[12] === 0x74 &&
                addr[13] === 0x52 &&
                addr[14] === 0x43 &&
                addr[15] === 0x5a &&
                addr[16] === 0x42 &&
                addr[17] === 0x69 &&
                addr[18] === 0x61 &&
                addr[19] === 0x38 &&
                addr[20] === 0x64 &&
                addr[21] === 0x33 &&
                addr[22] === 0x70 &&
                addr[23] === 0x7a &&
                addr[24] === 0x78 &&
                addr[25] === 0x6b &&
                addr[26] === 0x6e &&
                addr[27] === 0x63 &&
                addr[28] === 0x73 &&
                addr[29] === 0x7a &&
                addr[30] === 0x6f &&
                addr[31] === 0x33 &&
                addr[32] === 0x33 &&
                addr[33] === 0x38
            )

            raw = [
                0x8e, 0xad, 0xb4, 0xbb, 0x71, 0x2a, 0x29, 0x1b, 0x53, 0x43, 0xe0, 0x03,
                0x1f, 0x97, 0x6b, 0x0d, 0xa9, 0xed, 0x39, 0xc2,
            ]
            addr = util_raddr(raw)
            ASSERT(34 === addr.length)
            addr = toHex(addr)
            ASSERT(
                addr[0] === 0x72 &&
                addr[1] === 0x4e &&
                addr[2] === 0x72 &&
                addr[3] === 0x52 &&
                addr[4] === 0x73 &&
                addr[5] === 0x59 &&
                addr[6] === 0x57 &&
                addr[7] === 0x69 &&
                addr[8] === 0x4a &&
                addr[9] === 0x53 &&
                addr[10] === 0x64 &&
                addr[11] === 0x39 &&
                addr[12] === 0x47 &&
                addr[13] === 0x4a &&
                addr[14] === 0x50 &&
                addr[15] === 0x50 &&
                addr[16] === 0x36 &&
                addr[17] === 0x51 &&
                addr[18] === 0x71 &&
                addr[19] === 0x33 &&
                addr[20] === 0x4a &&
                addr[21] === 0x61 &&
                addr[22] === 0x44 &&
                addr[23] === 0x43 &&
                addr[24] === 0x37 &&
                addr[25] === 0x53 &&
                addr[26] === 0x48 &&
                addr[27] === 0x61 &&
                addr[28] === 0x57 &&
                addr[29] === 0x66 &&
                addr[30] === 0x68 &&
                addr[31] === 0x32 &&
                addr[32] === 0x33 &&
                addr[33] === 0x4b
            )

            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set util_raddr"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test util_raddr"), fee(XRP(1)));
    }

    void
    test_util_sha512h(FeatureBitset features)
    {
        testcase("Test util_sha512h");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                trace('error', 0, false)
                rollback(x.toString(), code)
            }
            }

            const Hook = (arg) => {
            let raw = [
                0x72, 0x4e, 0x36, 0x53, 0x59, 0x77, 0x72, 0x32, 0x64, 0x54, 0x56, 0x43,
                0x7a, 0x45, 0x71, 0x39, 0x57, 0x43, 0x77, 0x4a,
            ]
            let hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x42 &&
                hash[1] == 0x5c &&
                hash[2] == 0x4c &&
                hash[3] == 0x01 &&
                hash[4] == 0x84 &&
                hash[5] == 0xa5 &&
                hash[6] == 0x76 &&
                hash[7] == 0x79 &&
                hash[8] == 0xdc &&
                hash[9] == 0x6d &&
                hash[10] == 0xff &&
                hash[11] == 0x40 &&
                hash[12] == 0x8c &&
                hash[13] == 0x29 &&
                hash[14] == 0x06 &&
                hash[15] == 0x6b &&
                hash[16] == 0x0f &&
                hash[17] == 0xb9 &&
                hash[18] == 0xea &&
                hash[19] == 0x34
            )

            raw = [
                0x72, 0x4b, 0x4b, 0x75, 0x52, 0x36, 0x36, 0x46, 0x62, 0x38, 0x33, 0x76,
                0x35, 0x71, 0x79, 0x41, 0x34, 0x48, 0x67, 0x6a,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x36 &&
                hash[1] == 0x2c &&
                hash[2] == 0x32 &&
                hash[3] == 0x1d &&
                hash[4] == 0x8d &&
                hash[5] == 0xdd &&
                hash[6] == 0xaf &&
                hash[7] == 0x2d &&
                hash[8] == 0x3c &&
                hash[9] == 0xe6 &&
                hash[10] == 0x94 &&
                hash[11] == 0x12 &&
                hash[12] == 0x20 &&
                hash[13] == 0xda &&
                hash[14] == 0x62 &&
                hash[15] == 0xa6 &&
                hash[16] == 0x98 &&
                hash[17] == 0x41 &&
                hash[18] == 0x04 &&
                hash[19] == 0x5e
            )

            raw = [
                0x72, 0x42, 0x54, 0x33, 0x58, 0x57, 0x43, 0x76, 0x61, 0x38, 0x48, 0x55,
                0x4e, 0x4e, 0x5a, 0x46, 0x6a, 0x5a, 0x43, 0x55,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xcf &&
                hash[1] == 0xfd &&
                hash[2] == 0x6f &&
                hash[3] == 0x01 &&
                hash[4] == 0x95 &&
                hash[5] == 0x76 &&
                hash[6] == 0x7d &&
                hash[7] == 0xfb &&
                hash[8] == 0xca &&
                hash[9] == 0x41 &&
                hash[10] == 0xfd &&
                hash[11] == 0x24 &&
                hash[12] == 0x23 &&
                hash[13] == 0xd6 &&
                hash[14] == 0x82 &&
                hash[15] == 0x20 &&
                hash[16] == 0x76 &&
                hash[17] == 0xdd &&
                hash[18] == 0xc9 &&
                hash[19] == 0xec
            )

            raw = [
                0x72, 0x4c, 0x52, 0x4c, 0x41, 0x6e, 0x61, 0x62, 0x56, 0x6f, 0x46, 0x62,
                0x37, 0x47, 0x68, 0x79, 0x58, 0x75, 0x42, 0x53,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x02 &&
                hash[1] == 0xeb &&
                hash[2] == 0x2f &&
                hash[3] == 0x30 &&
                hash[4] == 0xfc &&
                hash[5] == 0x73 &&
                hash[6] == 0x34 &&
                hash[7] == 0xe7 &&
                hash[8] == 0x89 &&
                hash[9] == 0xa2 &&
                hash[10] == 0x58 &&
                hash[11] == 0xd6 &&
                hash[12] == 0xb0 &&
                hash[13] == 0x55 &&
                hash[14] == 0x32 &&
                hash[15] == 0x96 &&
                hash[16] == 0xb5 &&
                hash[17] == 0x2e &&
                hash[18] == 0x97 &&
                hash[19] == 0x81
            )

            raw = [
                0x72, 0x4c, 0x37, 0x33, 0x39, 0x47, 0x4b, 0x35, 0x75, 0x36, 0x79, 0x78,
                0x76, 0x43, 0x73, 0x6f, 0x68, 0x43, 0x32, 0x43,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x9f &&
                hash[1] == 0xd4 &&
                hash[2] == 0x7c &&
                hash[3] == 0x25 &&
                hash[4] == 0xde &&
                hash[5] == 0x23 &&
                hash[6] == 0x97 &&
                hash[7] == 0x57 &&
                hash[8] == 0xed &&
                hash[9] == 0x25 &&
                hash[10] == 0xd0 &&
                hash[11] == 0x98 &&
                hash[12] == 0xf7 &&
                hash[13] == 0x83 &&
                hash[14] == 0x70 &&
                hash[15] == 0xf6 &&
                hash[16] == 0x5f &&
                hash[17] == 0x3d &&
                hash[18] == 0xb5 &&
                hash[19] == 0x43
            )

            raw = [
                0x72, 0x4d, 0x4d, 0x45, 0x57, 0x74, 0x75, 0x4b, 0x43, 0x77, 0x54, 0x43,
                0x36, 0x31, 0x78, 0x41, 0x78, 0x35, 0x55, 0x46,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x77 &&
                hash[1] == 0x59 &&
                hash[2] == 0x43 &&
                hash[3] == 0x6b &&
                hash[4] == 0x4d &&
                hash[5] == 0x11 &&
                hash[6] == 0x6b &&
                hash[7] == 0xe5 &&
                hash[8] == 0xf8 &&
                hash[9] == 0x90 &&
                hash[10] == 0x07 &&
                hash[11] == 0x00 &&
                hash[12] == 0xb3 &&
                hash[13] == 0xb2 &&
                hash[14] == 0x6b &&
                hash[15] == 0x8a &&
                hash[16] == 0xc8 &&
                hash[17] == 0xf2 &&
                hash[18] == 0x82 &&
                hash[19] == 0xb7
            )

            raw = [
                0x72, 0x66, 0x48, 0x6a, 0x66, 0x31, 0x6b, 0x70, 0x4b, 0x6a, 0x39, 0x66,
                0x6a, 0x39, 0x35, 0x58, 0x6a, 0x59, 0x69, 0x51,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xbd &&
                hash[1] == 0x1b &&
                hash[2] == 0xdd &&
                hash[3] == 0x9d &&
                hash[4] == 0x10 &&
                hash[5] == 0xde &&
                hash[6] == 0x24 &&
                hash[7] == 0xa1 &&
                hash[8] == 0xb2 &&
                hash[9] == 0x6c &&
                hash[10] == 0x24 &&
                hash[11] == 0xbc &&
                hash[12] == 0xf9 &&
                hash[13] == 0x97 &&
                hash[14] == 0x50 &&
                hash[15] == 0xde &&
                hash[16] == 0x93 &&
                hash[17] == 0x39 &&
                hash[18] == 0x58 &&
                hash[19] == 0x21
            )

            raw = [
                0x72, 0x66, 0x6e, 0x75, 0x57, 0x38, 0x77, 0x6f, 0x4b, 0x62, 0x6e, 0x57,
                0x4b, 0x6b, 0x6b, 0x75, 0x39, 0x6a, 0x79, 0x64,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x3b &&
                hash[1] == 0x89 &&
                hash[2] == 0xed &&
                hash[3] == 0x68 &&
                hash[4] == 0x0d &&
                hash[5] == 0x13 &&
                hash[6] == 0x3b &&
                hash[7] == 0x1d &&
                hash[8] == 0x43 &&
                hash[9] == 0xfe &&
                hash[10] == 0xae &&
                hash[11] == 0x3e &&
                hash[12] == 0xc3 &&
                hash[13] == 0x90 &&
                hash[14] == 0xe8 &&
                hash[15] == 0x0e &&
                hash[16] == 0x17 &&
                hash[17] == 0x14 &&
                hash[18] == 0x23 &&
                hash[19] == 0x71
            )

            raw = [
                0x72, 0x70, 0x79, 0x64, 0x52, 0x39, 0x55, 0x32, 0x67, 0x66, 0x75, 0x6b,
                0x34, 0x5a, 0x72, 0x53, 0x66, 0x48, 0x61, 0x71,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x2b &&
                hash[1] == 0x01 &&
                hash[2] == 0x00 &&
                hash[3] == 0x05 &&
                hash[4] == 0xf1 &&
                hash[5] == 0x60 &&
                hash[6] == 0x71 &&
                hash[7] == 0x62 &&
                hash[8] == 0x7c &&
                hash[9] == 0x4a &&
                hash[10] == 0xcc &&
                hash[11] == 0x03 &&
                hash[12] == 0x2a &&
                hash[13] == 0x89 &&
                hash[14] == 0x40 &&
                hash[15] == 0x5a &&
                hash[16] == 0x03 &&
                hash[17] == 0xdc &&
                hash[18] == 0x83 &&
                hash[19] == 0xc8
            )

            raw = [
                0x72, 0x4c, 0x4c, 0x45, 0x36, 0x34, 0x74, 0x44, 0x4c, 0x78, 0x59, 0x37,
                0x47, 0x6f, 0x41, 0x41, 0x57, 0x66, 0x73, 0x36,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xdf &&
                hash[1] == 0xe3 &&
                hash[2] == 0x14 &&
                hash[3] == 0xf0 &&
                hash[4] == 0x5f &&
                hash[5] == 0x95 &&
                hash[6] == 0x8c &&
                hash[7] == 0x57 &&
                hash[8] == 0x2f &&
                hash[9] == 0x9d &&
                hash[10] == 0x45 &&
                hash[11] == 0xdc &&
                hash[12] == 0x12 &&
                hash[13] == 0x77 &&
                hash[14] == 0x39 &&
                hash[15] == 0xac &&
                hash[16] == 0xea &&
                hash[17] == 0x4a &&
                hash[18] == 0xb0 &&
                hash[19] == 0x8f
            )

            raw = [
                0x72, 0x50, 0x64, 0x50, 0x58, 0x77, 0x76, 0x75, 0x39, 0x4e, 0x4c, 0x59,
                0x46, 0x50, 0x34, 0x69, 0x56, 0x64, 0x56, 0x70,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xd1 &&
                hash[1] == 0x9f &&
                hash[2] == 0x25 &&
                hash[3] == 0x93 &&
                hash[4] == 0xa3 &&
                hash[5] == 0xca &&
                hash[6] == 0xea &&
                hash[7] == 0x10 &&
                hash[8] == 0x06 &&
                hash[9] == 0x78 &&
                hash[10] == 0xfc &&
                hash[11] == 0x58 &&
                hash[12] == 0xa4 &&
                hash[13] == 0x99 &&
                hash[14] == 0x3c &&
                hash[15] == 0x6e &&
                hash[16] == 0xc4 &&
                hash[17] == 0x2d &&
                hash[18] == 0x6d &&
                hash[19] == 0x53
            )

            raw = [
                0x72, 0x44, 0x65, 0x44, 0x32, 0x5a, 0x71, 0x53, 0x48, 0x35, 0x44, 0x70,
                0x51, 0x4d, 0x78, 0x76, 0x36, 0x36, 0x52, 0x6b,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x4d &&
                hash[1] == 0x5b &&
                hash[2] == 0xdd &&
                hash[3] == 0x31 &&
                hash[4] == 0xde &&
                hash[5] == 0xb9 &&
                hash[6] == 0xf5 &&
                hash[7] == 0xb8 &&
                hash[8] == 0xbd &&
                hash[9] == 0x17 &&
                hash[10] == 0xe1 &&
                hash[11] == 0x51 &&
                hash[12] == 0xaa &&
                hash[13] == 0x51 &&
                hash[14] == 0x9c &&
                hash[15] == 0x5b &&
                hash[16] == 0xe0 &&
                hash[17] == 0x15 &&
                hash[18] == 0x61 &&
                hash[19] == 0x2c
            )

            raw = [
                0x72, 0x55, 0x34, 0x78, 0x54, 0x52, 0x75, 0x6f, 0x32, 0x34, 0x62, 0x52,
                0x6f, 0x65, 0x41, 0x48, 0x33, 0x53, 0x55, 0x66,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xbd &&
                hash[1] == 0xa1 &&
                hash[2] == 0x62 &&
                hash[3] == 0x1e &&
                hash[4] == 0x84 &&
                hash[5] == 0x12 &&
                hash[6] == 0xb3 &&
                hash[7] == 0xcc &&
                hash[8] == 0x58 &&
                hash[9] == 0x19 &&
                hash[10] == 0x9a &&
                hash[11] == 0x22 &&
                hash[12] == 0xcf &&
                hash[13] == 0x6a &&
                hash[14] == 0x0a &&
                hash[15] == 0x43 &&
                hash[16] == 0xde &&
                hash[17] == 0xb5 &&
                hash[18] == 0xba &&
                hash[19] == 0x50
            )

            raw = [
                0x72, 0x6e, 0x6d, 0x6f, 0x6a, 0x57, 0x46, 0x6f, 0x41, 0x58, 0x72, 0x76,
                0x71, 0x75, 0x62, 0x6f, 0x45, 0x77, 0x4e, 0x4e,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x5f &&
                hash[1] == 0x26 &&
                hash[2] == 0xf9 &&
                hash[3] == 0x0a &&
                hash[4] == 0xc7 &&
                hash[5] == 0xd5 &&
                hash[6] == 0x40 &&
                hash[7] == 0x2d &&
                hash[8] == 0x1f &&
                hash[9] == 0x9e &&
                hash[10] == 0x46 &&
                hash[11] == 0xaa &&
                hash[12] == 0x6d &&
                hash[13] == 0x9c &&
                hash[14] == 0x64 &&
                hash[15] == 0x88 &&
                hash[16] == 0x87 &&
                hash[17] == 0xf3 &&
                hash[18] == 0x29 &&
                hash[19] == 0x72
            )

            raw = [
                0x72, 0x61, 0x33, 0x57, 0x65, 0x64, 0x69, 0x71, 0x58, 0x37, 0x34, 0x79,
                0x42, 0x42, 0x68, 0x48, 0x4c, 0x44, 0x51, 0x4d,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x25 &&
                hash[1] == 0x70 &&
                hash[2] == 0x5f &&
                hash[3] == 0x6d &&
                hash[4] == 0xa8 &&
                hash[5] == 0x60 &&
                hash[6] == 0x54 &&
                hash[7] == 0xba &&
                hash[8] == 0xd8 &&
                hash[9] == 0x33 &&
                hash[10] == 0x41 &&
                hash[11] == 0x48 &&
                hash[12] == 0x95 &&
                hash[13] == 0x52 &&
                hash[14] == 0xa6 &&
                hash[15] == 0x22 &&
                hash[16] == 0x9d &&
                hash[17] == 0x82 &&
                hash[18] == 0xa0 &&
                hash[19] == 0x87
            )

            raw = [
                0x72, 0x45, 0x47, 0x57, 0x33, 0x6b, 0x6f, 0x34, 0x41, 0x31, 0x69, 0x50,
                0x43, 0x5a, 0x54, 0x78, 0x6d, 0x77, 0x6a, 0x44,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xd4 &&
                hash[1] == 0xda &&
                hash[2] == 0xe0 &&
                hash[3] == 0xc7 &&
                hash[4] == 0x40 &&
                hash[5] == 0xc4 &&
                hash[6] == 0x28 &&
                hash[7] == 0x59 &&
                hash[8] == 0xa9 &&
                hash[9] == 0x6d &&
                hash[10] == 0x91 &&
                hash[11] == 0xdc &&
                hash[12] == 0x34 &&
                hash[13] == 0x0d &&
                hash[14] == 0xb9 &&
                hash[15] == 0xe6 &&
                hash[16] == 0xe9 &&
                hash[17] == 0x9d &&
                hash[18] == 0x04 &&
                hash[19] == 0x0b
            )

            raw = [
                0x72, 0x68, 0x52, 0x46, 0x71, 0x54, 0x35, 0x45, 0x39, 0x7a, 0x63, 0x69,
                0x70, 0x68, 0x4c, 0x54, 0x39, 0x78, 0x6a, 0x52,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x61 &&
                hash[1] == 0x5b &&
                hash[2] == 0xfe &&
                hash[3] == 0x17 &&
                hash[4] == 0x6e &&
                hash[5] == 0x81 &&
                hash[6] == 0x42 &&
                hash[7] == 0xff &&
                hash[8] == 0xee &&
                hash[9] == 0xd7 &&
                hash[10] == 0x1a &&
                hash[11] == 0x6d &&
                hash[12] == 0x14 &&
                hash[13] == 0x5d &&
                hash[14] == 0x64 &&
                hash[15] == 0xa8 &&
                hash[16] == 0x20 &&
                hash[17] == 0x1a &&
                hash[18] == 0x33 &&
                hash[19] == 0xc3
            )

            raw = [
                0x72, 0x70, 0x61, 0x4a, 0x69, 0x34, 0x4c, 0x62, 0x55, 0x36, 0x55, 0x63,
                0x4a, 0x45, 0x78, 0x62, 0x38, 0x39, 0x35, 0x5a,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x01 &&
                hash[1] == 0x61 &&
                hash[2] == 0xa4 &&
                hash[3] == 0x8e &&
                hash[4] == 0x6d &&
                hash[5] == 0x20 &&
                hash[6] == 0xba &&
                hash[7] == 0x20 &&
                hash[8] == 0x72 &&
                hash[9] == 0x72 &&
                hash[10] == 0x8f &&
                hash[11] == 0x4f &&
                hash[12] == 0x3f &&
                hash[13] == 0xe1 &&
                hash[14] == 0xe1 &&
                hash[15] == 0xe7 &&
                hash[16] == 0xeb &&
                hash[17] == 0x15 &&
                hash[18] == 0xa8 &&
                hash[19] == 0x4c
            )

            raw = [
                0x72, 0x34, 0x59, 0x78, 0x47, 0x46, 0x71, 0x51, 0x64, 0x47, 0x70, 0x71,
                0x6e, 0x4c, 0x59, 0x65, 0x4d, 0x38, 0x56, 0x52,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0x42 &&
                hash[1] == 0xc5 &&
                hash[2] == 0x2f &&
                hash[3] == 0x3b &&
                hash[4] == 0xb7 &&
                hash[5] == 0xd4 &&
                hash[6] == 0x54 &&
                hash[7] == 0xb4 &&
                hash[8] == 0x97 &&
                hash[9] == 0xb4 &&
                hash[10] == 0xfc &&
                hash[11] == 0xb0 &&
                hash[12] == 0x46 &&
                hash[13] == 0xba &&
                hash[14] == 0xb6 &&
                hash[15] == 0xad &&
                hash[16] == 0x93 &&
                hash[17] == 0x8d &&
                hash[18] == 0xeb &&
                hash[19] == 0x7d
            )

            raw = [
                0x72, 0x33, 0x76, 0x71, 0x75, 0x79, 0x72, 0x45, 0x39, 0x55, 0x53, 0x70,
                0x68, 0x62, 0x43, 0x55, 0x6d, 0x65, 0x4b, 0x55,
            ]
            hash = util_sha512h(raw)
            ASSERT(32 == hash.length)
            ASSERT(
                hash[0] == 0xd5 &&
                hash[1] == 0x6b &&
                hash[2] == 0x6b &&
                hash[3] == 0x45 &&
                hash[4] == 0x30 &&
                hash[5] == 0xf0 &&
                hash[6] == 0x34 &&
                hash[7] == 0x76 &&
                hash[8] == 0x31 &&
                hash[9] == 0x56 &&
                hash[10] == 0x8c &&
                hash[11] == 0x38 &&
                hash[12] == 0x0c &&
                hash[13] == 0x1a &&
                hash[14] == 0xaf &&
                hash[15] == 0xab &&
                hash[16] == 0x42 &&
                hash[17] == 0x16 &&
                hash[18] == 0x21 &&
                hash[19] == 0x42
            )

            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set util_sha512h"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test util_sha512h"), fee(XRP(1)));
    }

    void
    test_util_verify(FeatureBitset features)
    {
        testcase("Test util_verify");
        using namespace jtx;
        Env env{*this, features};

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x, code) => {
            if (!x) {
                trace('error', 0, false)
                rollback(x.toString(), code)
            }
            }

            // secp256k1
            const pubkey_sec = [
            0x02, 0xc7, 0x38, 0x7f, 0xfc, 0x25, 0xc1, 0x56, 0xca, 0x7f, 0x8a, 0x6d, 0x76,
            0x0c, 0x8d, 0x01, 0xef, 0x64, 0x2c, 0xee, 0x9c, 0xe4, 0x68, 0x0c, 0x33, 0xff,
            0xb3, 0xff, 0x39, 0xaf, 0xec, 0xfe, 0x70,
            ]

            const sig_sec = [
            0x30, 0x45, 0x02, 0x21, 0x00, 0x95, 0x6e, 0x7d, 0x1f, 0x01, 0x16, 0xf1, 0x65,
            0x00, 0xd2, 0xcc, 0xd8, 0x8d, 0x2a, 0x2f, 0xef, 0xf6, 0x52, 0x16, 0x85, 0x42,
            0xf4, 0x4e, 0x43, 0xdb, 0xe6, 0xf4, 0x53, 0xe8, 0x03, 0xb8, 0x4f, 0x02, 0x20,
            0x0a, 0xb6, 0xc3, 0x4b, 0x5f, 0x0c, 0xc6, 0x6b, 0x4f, 0x1f, 0x83, 0xe9, 0x89,
            0x74, 0xb8, 0x80, 0xa2, 0x2f, 0xae, 0x52, 0x91, 0x6b, 0xa2, 0xce, 0x96, 0xa3,
            0x61, 0x05, 0x3f, 0xff, 0x81, 0xe9,
            ]

            // ed25519
            const pubkey_ed = [
            0xed, 0xd9, 0xb3, 0x59, 0x98, 0x02, 0xb2, 0x14, 0xa9, 0x9d, 0x75, 0x77, 0x12,
            0xd6, 0xab, 0xdf, 0x72, 0xf8, 0x3c, 0x63, 0xbb, 0xd5, 0x38, 0x61, 0x41, 0x17,
            0x90, 0xb1, 0x3d, 0x04, 0xb2, 0xc5, 0xc9,
            ]

            const sig_ed = [
            0x56, 0x68, 0x80, 0x76, 0x70, 0xfe, 0xce, 0x60, 0x34, 0xaf, 0xd6, 0xcd, 0x1b,
            0xb4, 0xc6, 0x60, 0xae, 0x08, 0x39, 0x6d, 0x6d, 0x8b, 0x7d, 0x22, 0x71, 0x3b,
            0xda, 0x26, 0x43, 0xc1, 0xe1, 0x91, 0xc4, 0xe4, 0x4d, 0x8e, 0x02, 0xe8, 0x57,
            0x8b, 0x20, 0x45, 0xda, 0xd4, 0x8f, 0x97, 0xfc, 0x16, 0xf8, 0x92, 0x5b, 0x6b,
            0x51, 0xfb, 0x3b, 0xe5, 0x0f, 0xb0, 0x4b, 0x3a, 0x20, 0x4c, 0x53, 0x04,
            ]

            const msg = [0xde, 0xad, 0xbe, 0xef]
            const bmsg = [0xad, 0xbe, 0xef]

            const Hook = (arg) => {
            // test secp256k1 verification
            ASSERT(util_verify(msg, sig_sec, pubkey_sec) === 1)
            ASSERT(util_verify(bmsg, sig_sec, pubkey_sec) === 0)

            // test ed25519 verification
            ASSERT(util_verify(msg, sig_ed, pubkey_ed) === 1)
            ASSERT(util_verify(bmsg, sig_ed, pubkey_ed) === 0)

            accept('', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set util_verify"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test util_verify"), fee(XRP(1)));
    }

    void
    test_sto_emplace(FeatureBitset features)
    {
        testcase("Test sto_emplace");
        using namespace jtx;

        Env env{*this, features};

        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            var ASSERT = (x) => {
              if (!x) {
                rollback(x.toString(), 0)
              }
            }
            var sto = [
              17, 0, 97, 34, 0, 0, 0, 0, 36, 4, 31, 148, 217, 37, 4, 94, 132, 183, 45, 0, 0,
              0, 0, 85, 19, 64, 179, 37, 134, 49, 150, 181, 111, 65, 245, 137, 235, 125, 47,
              217, 76, 13, 125, 184, 14, 75, 44, 103, 167, 120, 42, 214, 194, 176, 119, 80,
              98, 64, 0, 0, 0, 0, 164, 121, 148, 129, 20, 55, 223, 68, 7, 231, 170, 7, 241,
              213, 201, 145, 242, 211, 111, 158, 184, 199, 52, 175, 108,
            ]
            var ins = [
              86, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
              17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
            ]
            var ans = [
              17, 0, 97, 34, 0, 0, 0, 0, 36, 4, 31, 148, 217, 37, 4, 94, 132, 183, 45, 0, 0,
              0, 0, 85, 19, 64, 179, 37, 134, 49, 150, 181, 111, 65, 245, 137, 235, 125, 47,
              217, 76, 13, 125, 184, 14, 75, 44, 103, 167, 120, 42, 214, 194, 176, 119, 80,
              86, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
              17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 98, 64, 0, 0, 0, 0,
              164, 121, 148, 129, 20, 55, 223, 68, 7, 231, 170, 7, 241, 213, 201, 145, 242,
              211, 111, 158, 184, 199, 52, 175, 108,
            ]
            var ans2 = [
              17, 0, 97, 34, 0, 0, 0, 0, 36, 4, 31, 148, 217, 37, 4, 94, 132, 183, 45, 0, 0,
              0, 0, 84, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17,
              17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 17, 85, 19, 64, 179,
              37, 134, 49, 150, 181, 111, 65, 245, 137, 235, 125, 47, 217, 76, 13, 125, 184,
              14, 75, 44, 103, 167, 120, 42, 214, 194, 176, 119, 80, 98, 64, 0, 0, 0, 0,
              164, 121, 148, 129, 20, 55, 223, 68, 7, 231, 170, 7, 241, 213, 201, 145, 242,
              211, 111, 158, 184, 199, 52, 175, 108,
            ]
            var INVALID_ARGUMENT = -7
            var TOO_SMALL = -4
            var PARSE_ERROR = -18
            var Hook = (arg) => {
              ASSERT(sto_emplace(void 0, 1, 1) == INVALID_ARGUMENT)
              ASSERT(sto_emplace(1, 1, void 0) == INVALID_ARGUMENT)
              ASSERT(sto_emplace(1, 1, -1) == INVALID_ARGUMENT)
              ASSERT(sto_emplace(1, 1, 4294967215) == INVALID_ARGUMENT)
              ASSERT(sto_emplace(1, void 0, 1) == INVALID_ARGUMENT)
              ASSERT(sto_emplace(1, false, 1) == INVALID_ARGUMENT)
              ASSERT(sto_emplace([0, 1], [0, 1], 327686) == PARSE_ERROR)
              {
                ASSERT(sto_emplace([0], [0, 1], 327686) == TOO_SMALL)
                ASSERT(sto_emplace([0, 1], [0], 327686) == TOO_SMALL)
                ASSERT(
                  sto_emplace([0, 1], new Array(4097).fill(0), 327686) == INVALID_ARGUMENT
                )
                ASSERT(
                  sto_emplace(new Array(16385).fill(0), [0, 1], 327686) == INVALID_ARGUMENT
                )
              }
              let buf = []
              {
                buf = sto_emplace(sto, ins, 327686)
                ASSERT(buf.length === sto.length + ins.length)
                for (let i = 0; i < ans.length; ++i) ASSERT(ans[i] == buf[i])
                ins[0] = 84
                buf = sto_emplace(sto, ins, 327684)
                ASSERT(buf.length == sto.length + ins.length)
                for (let i = 0; i < ans2.length; ++i) ASSERT(ans2[i] == buf[i])
              }
              {
                const sto2 = [34, 0, 0, 0, 0]
                const ins2 = [17, 17, 17]
                const buf2 = sto_emplace(sto2, ins2, 65537)
                ASSERT(buf2.length == sto2.length + ins2.length)
                const ans3 = [17, 17, 17, 34, 0, 0, 0, 0]
                for (let i = 0; i < ans3.length; ++i) ASSERT(ans3[i] == buf2[i])
              }
              {
                const sto2 = [34, 0, 0, 0, 0]
                const ins2 = [49, 17, 17, 17, 17, 18, 34, 34, 34]
                buf = sto_emplace(sto2, ins2, 196609)
                ASSERT(buf.length == sto2.length + ins2.length)
                const ans3 = [34, 0, 0, 0, 0, 49, 17, 17, 17, 17, 18, 34, 34, 34]
                for (let i = 0; i < ans3.length; ++i) ASSERT(ans3[i] == buf[i])
              }
              {
                const rep = [34, 16, 32, 48, 64]
                buf = sto_emplace(sto, rep, 131074)
                ASSERT(buf.length == sto.length)
                ASSERT(buf[0] == sto[0] && buf[1] == sto[1] && buf[2] == sto[2])
                for (let i = 3; i < rep.length + 3; ++i) ASSERT(buf[i] == rep[i - 3])
                for (let i = rep.length + 3; i < sto.length; ++i) ASSERT(sto[i] == buf[i])
              }
              return accept(0, 0)
            }

        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_emplace"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_emplace"), fee(XRP(1)));
    }

    void
    test_sto_erase(FeatureBitset features)
    {
        testcase("Test sto_erase");
        using namespace jtx;

        Env env{*this, features};
        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const ASSERT = (x) => {
            if (!x) rollback(x.toString(), 0)
            }

            const DOESNT_EXIST = -5

            const sto = [
                0x11, 0x00, 0x61, 0x22, 0x00, 0x00, 0x00, 0x00, 0x24, 0x04, 0x1f, 0x94, 0xd9,
                0x25, 0x04, 0x5e, 0x84, 0xb7, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x55, 0x13, 0x40,
                0xb3, 0x25, 0x86, 0x31, 0x96, 0xb5, 0x6f, 0x41, 0xf5, 0x89, 0xeb, 0x7d, 0x2f,
                0xd9, 0x4c, 0x0d, 0x7d, 0xb8, 0x0e, 0x4b, 0x2c, 0x67, 0xa7, 0x78, 0x2a, 0xd6,
                0xc2, 0xb0, 0x77, 0x50, 0x62, 0x40, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x79, 0x94,
                0x81, 0x14, 0x37, 0xdf, 0x44, 0x07, 0xe7, 0xaa, 0x07, 0xf1, 0xd5, 0xc9, 0x91,
                0xf2, 0xd3, 0x6f, 0x9e, 0xb8, 0xc7, 0x34, 0xaf, 0x6c,
            ]

            // test_sto_erase
            const Hook = (arg) => {
                // erase field 22
                {
                    const buf = sto_erase(sto, 0x20002)
                    ASSERT(buf.length === sto.length - 5)

                    ASSERT(buf[0] === sto[0] && buf[1] === sto[1] && buf[2] === sto[2])
                    for (let i = 3; i < sto.length - 5; i++) {
                    ASSERT(sto[i + 5] === buf[i])
                    }
                }

                // test front erasure
                {
                    const buf = sto_erase(sto, 0x10001)
                    ASSERT(buf.length === sto.length - 3)

                    for (let i = 3; i < sto.length - 3; i++) {
                    ASSERT(sto[i] === buf[i - 3])
                    }
                }

                // test back erasure
                {
                    const buf = sto_erase(sto, 0x80001)
                    ASSERT(buf.length === sto.length - 22)

                    for (let i = 0; i < sto.length - 22; i++) {
                    ASSERT(sto[i] === buf[i])
                    }
                }

                // test not found
                {
                    const buf = sto_erase(sto, 0x80002)
                    // TODO: https://github.com/Xahau/xahaud/issues/459
                    // ASSERT(buf === DOESNT_EXIST)
                }

                // test total erasure
                {
                    const rep = [0x22, 0x10, 0x20, 0x30, 0x40]
                    const buf = sto_erase(rep, 0x20002)
                    ASSERT(buf.length === 0)
                }

                accept('success', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_erase"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_erase"), fee(XRP(1)));
    }

    void
    test_sto_subarray(FeatureBitset features)
    {
        testcase("Test sto_subarray");
        using namespace jtx;

        Env env{*this, features};
        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const DOESNT_EXIST = -5
            const INVALID_ARGUMENT = -7

            const ASSERT = (x) => {
                if (!x) rollback(x.toString(), 0)
            }

            const sto = [
                0xf4, 0xeb, 0x13, 0x00, 0x01, 0x81, 0x14, 0x20, 0x42, 0x88, 0xd2, 0xe4, 0x7f,
                0x8e, 0xf6, 0xc9, 0x9b, 0xcc, 0x45, 0x79, 0x66, 0x32, 0x0d, 0x12, 0x40, 0x97,
                0x11, 0xe1, 0xeb, 0x13, 0x00, 0x01, 0x81, 0x14, 0x3e, 0x9d, 0x4a, 0x2b, 0x8a,
                0xa0, 0x78, 0x0f, 0x68, 0x2d, 0x13, 0x6f, 0x7a, 0x56, 0xd6, 0x72, 0x4e, 0xf5,
                0x37, 0x54, 0xe1, 0xf1,
            ]

            const Hook = (reserved) => {
                // Test invalid arg
                ASSERT(sto_subarray(undefined, 0) === INVALID_ARGUMENT)
                ASSERT(sto_subarray(sto, undefined) === INVALID_ARGUMENT)
                ASSERT(sto_subarray(sto, -1) === INVALID_ARGUMENT)
                ASSERT(sto_subarray(sto, 0xffffffff + 1) === INVALID_ARGUMENT)

                // Test index 0, should be position 1 length 27
                // ASSERT(sto_subarray(sto, 0) === (1 << 32) + 27)
                ASSERT(sto_subarray(sto, 0) === 2 ** 32 + 27)

                // Test index 1, should be position 28 length 27
                // ASSERT(sto_subarray(sto, 1) === (28 << 32) + 27)
                ASSERT(sto_subarray(sto, 1) === 28 * 2 ** 32 + 27)

                // Test index 2, doesn't exist
                ASSERT(sto_subarray(sto, 2) === DOESNT_EXIST)

                accept('success', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_subarray"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_subarray"), fee(XRP(1)));
    }

    void
    test_sto_subfield(FeatureBitset features)
    {
        testcase("Test sto_subfield");
        using namespace jtx;

        Env env{*this, features};
        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const DOESNT_EXIST = -5
            const INVALID_ARGUMENT = -7
            const ASSERT = (x) => {
                if (!x) rollback(x.toString(), 0)
            }

            const sto = [
                0x11, 0x00, 0x53, 0x22, 0x00, 0x00, 0x00, 0x00, 0x25, 0x01, 0x52, 0x70, 0x1a,
                0x20, 0x23, 0x00, 0x00, 0x00, 0x02, 0x20, 0x26, 0x00, 0x00, 0x00, 0x00, 0x34,
                0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0x09, 0xa9, 0xc8, 0x6b,
                0xf2, 0x06, 0x95, 0x73, 0x5a, 0xb0, 0x36, 0x20, 0xeb, 0x1c, 0x32, 0x60, 0x66,
                0x35, 0xac, 0x3d, 0xa0, 0xb7, 0x02, 0x82, 0xf3, 0x7c, 0x67, 0x4f, 0xc8, 0x89,
                0xef, 0xe7,
            ]

            const Hook = (reserved) => {
                // Test invalid arg
                ASSERT(sto_subfield(undefined, 0x10001) === INVALID_ARGUMENT)
                ASSERT(sto_subfield(sto, undefined) === INVALID_ARGUMENT)
                ASSERT(sto_subfield(sto, -1) === INVALID_ARGUMENT)
                ASSERT(sto_subfield(sto, 0xffffffff + 1) === INVALID_ARGUMENT)

                // ASSERT(sto_subfield(sto, 0x10001) === (1 << 32) + 2)
                ASSERT(sto_subfield(sto, 0x10001) === 2 ** 32 + 2)

                // Test subfield 0x11, should be position 0 length 3, payload pos 1, len 2
                // ASSERT(sto_subfield(sto, 0x10001) === (1 << 32) + 2)
                ASSERT(sto_subfield(sto, 0x10001) === 2 ** 32 + 2)

                // Test subfield 0x22, should be position 3 length 5, payload pos 4, len 4
                // ASSERT(sto_subfield(sto, 0x20002) === (4 << 32) + 4)
                ASSERT(sto_subfield(sto, 0x20002) === 4 * 2 ** 32 + 4)

                // Test subfield 0x34, should be at position 25, length = 9, payload pos 26, len 8
                // ASSERT(sto_subfield(sto, 0x30004) === (26 << 32) + 8)
                ASSERT(sto_subfield(sto, 0x30004) === 26 * 2 ** 32 + 8)

                // Test final subfield, position 34, length 33, payload pos 35, len 32
                // ASSERT(sto_subfield(sto, 0x50005) === (35 << 32) + 32)
                ASSERT(sto_subfield(sto, 0x50005) === 35 * 2 ** 32 + 32)

                // Test not found
                ASSERT(sto_subfield(sto, 0x90009) === DOESNT_EXIST)

                accept('success', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_subfield"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_subfield"), fee(XRP(1)));
    }

    void
    test_sto_validate(FeatureBitset features)
    {
        testcase("Test sto_validate");
        using namespace jtx;

        Env env{*this, features};
        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const INVALID_ARGUMENT = -7
            const ASSERT = (x) => {
              if (!x) rollback(x.toString(), 0)
            }

            const sto = [
                0x11, 0x00, 0x61, 0x22, 0x00, 0x00, 0x00, 0x00, 0x24, 0x04, 0x1f, 0x94, 0xd9,
                0x25, 0x04, 0x5e, 0x84, 0xb7, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x55, 0x13, 0x40,
                0xb3, 0x25, 0x86, 0x31, 0x96, 0xb5, 0x6f, 0x41, 0xf5, 0x89, 0xeb, 0x7d, 0x2f,
                0xd9, 0x4c, 0x0d, 0x7d, 0xb8, 0x0e, 0x4b, 0x2c, 0x67, 0xa7, 0x78, 0x2a, 0xd6,
                0xc2, 0xb0, 0x77, 0x50, 0x62, 0x40, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x79, 0x94,
                0x81, 0x14, 0x37, 0xdf, 0x44, 0x07, 0xe7, 0xaa, 0x07, 0xf1, 0xd5, 0xc9, 0x91,
                0xf2, 0xd3, 0x6f, 0x9e, 0xb8, 0xc7, 0x34, 0xaf, 0x6c,
            ]

            const Hook = (reserved) => {
                // Test arg check
                ASSERT(sto_validate(undefined) === INVALID_ARGUMENT)

                // Test validation
                ASSERT(sto_validate(sto) === 1)

                // Invalidate
                sto[0] = 0x22
                ASSERT(sto_validate(sto) === 0)

                // Fix
                sto[0] = 0x11

                // Invalidate somewhere else
                sto[3] = 0x40
                ASSERT(sto_validate(sto) === 0)

                // test small validation
                {
                    const sto = [0x22, 0x00, 0x00, 0x00, 0x00]
                    ASSERT(sto_validate(sto) === 1)
                }

                accept('success', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_validate"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_validate"), fee(XRP(1)));
    }

    void
    test_sto_to_json(FeatureBitset features)
    {
        testcase("Test sto_to_json");
        using namespace jtx;

        Env env{*this, features};
        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const INVALID_ARGUMENT = -7
            const ASSERT = (x) => {
                if (!x) rollback(x.toString(), 0)
            }

            const sto = [
                0x11, 0x00, 0x61, 0x22, 0x00, 0x00, 0x00, 0x00, 0x24, 0x04, 0x1f, 0x94, 0xd9,
                0x25, 0x04, 0x5e, 0x84, 0xb7, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x55, 0x13, 0x40,
                0xb3, 0x25, 0x86, 0x31, 0x96, 0xb5, 0x6f, 0x41, 0xf5, 0x89, 0xeb, 0x7d, 0x2f,
                0xd9, 0x4c, 0x0d, 0x7d, 0xb8, 0x0e, 0x4b, 0x2c, 0x67, 0xa7, 0x78, 0x2a, 0xd6,
                0xc2, 0xb0, 0x77, 0x50, 0x62, 0x40, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x79, 0x94,
                0x81, 0x14, 0x37, 0xdf, 0x44, 0x07, 0xe7, 0xaa, 0x07, 0xf1, 0xd5, 0xc9, 0x91,
                0xf2, 0xd3, 0x6f, 0x9e, 0xb8, 0xc7, 0x34, 0xaf, 0x6c,
            ]

            const sto_json = {
                LedgerEntryType: 'AccountRoot',
                Flags: 0,
                Sequence: 69178585,
                PreviousTxnLgrSeq: 73303223,
                OwnerCount: 0,
                PreviousTxnID:
                    '1340B325863196B56F41F589EB7D2FD94C0D7DB80E4B2C67A7782AD6C2B07750',
                Balance: '10779028',
                Account: 'raaRdKAobgyv8VqpRV2UZRjT74FL9PHuei',
            }

            const Hook = (reserved) => {
                // Test arg check
                ASSERT(sto_to_json(undefined) === INVALID_ARGUMENT)

                // Test ledger entry
                const json = sto_to_json(sto)
                ASSERT(typeof json === 'object')
                trace('2', '', false)
                trace('JSON.stringify(json)', json, false)
                ASSERT(Object.keys(json).length === Object.keys(sto_json).length)
                for (const key of Object.keys(json)) {
                    ASSERT(sto_json[key] === json[key])
                }

                // test transaction
                ASSERT(otxn_slot(1) === 1)
                const otxn_sto = slot(1)
                ASSERT(typeof sto_to_json(otxn_sto) === 'object')

                // Invalid
                sto[0] = 0x22
                ASSERT(sto_to_json(sto) === INVALID_ARGUMENT)

                // Fix
                sto[0] = 0x11

                // Invalid length
                sto.push(0x00)
                ASSERT(sto_to_json(sto) === INVALID_ARGUMENT)

                accept('success', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_to_json"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_to_json"), fee(XRP(1)));
    }

    void
    test_sto_from_json(FeatureBitset features)
    {
        testcase("Test sto_from_json");
        using namespace jtx;

        Env env{*this, features};
        auto const bob = Account{"bob"};
        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);

        TestHook hook = jswasm[R"[test.hook](
            const INVALID_ARGUMENT = -7
            const ASSERT = (x) => {
                if (!x) rollback(x.toString(), 0)
            }

            const accRoot = {
                LedgerEntryType: 'AccountRoot',
                Flags: 0,
                Sequence: 69178585,
                PreviousTxnLgrSeq: 73303223,
                OwnerCount: 0,
                PreviousTxnID:
                    '1340B325863196B56F41F589EB7D2FD94C0D7DB80E4B2C67A7782AD6C2B07750',
                Balance: '10779028',
                Account: 'raaRdKAobgyv8VqpRV2UZRjT74FL9PHuei',
            }

            const sto = [
                0x11, 0x00, 0x61, 0x22, 0x00, 0x00, 0x00, 0x00, 0x24, 0x04, 0x1f, 0x94, 0xd9,
                0x25, 0x04, 0x5e, 0x84, 0xb7, 0x2d, 0x00, 0x00, 0x00, 0x00, 0x55, 0x13, 0x40,
                0xb3, 0x25, 0x86, 0x31, 0x96, 0xb5, 0x6f, 0x41, 0xf5, 0x89, 0xeb, 0x7d, 0x2f,
                0xd9, 0x4c, 0x0d, 0x7d, 0xb8, 0x0e, 0x4b, 0x2c, 0x67, 0xa7, 0x78, 0x2a, 0xd6,
                0xc2, 0xb0, 0x77, 0x50, 0x62, 0x40, 0x00, 0x00, 0x00, 0x00, 0xa4, 0x79, 0x94,
                0x81, 0x14, 0x37, 0xdf, 0x44, 0x07, 0xe7, 0xaa, 0x07, 0xf1, 0xd5, 0xc9, 0x91,
                0xf2, 0xd3, 0x6f, 0x9e, 0xb8, 0xc7, 0x34, 0xaf, 0x6c,
            ]

            const Hook = (reserved) => {
                // Test arg check
                ASSERT(sto_from_json(undefined) === INVALID_ARGUMENT)

                // Test ledger entry
                const accRootSto = sto_from_json(accRoot)
                ASSERT(Array.isArray(accRootSto))
                ASSERT(accRootSto.length === sto.length)
                ASSERT(accRootSto.every((v, i) => v === sto[i]))

                // test transaction
                const txn_json = otxn_json()
                ASSERT(otxn_slot(1) === 1)
                const otxn_sto = slot(1)
                ASSERT(sto_from_json(txn_json).length === otxn_sto.length)
                ASSERT(sto_from_json(txn_json).every((v, i) => v === otxn_sto[i]))

                // Invalid field
                ASSERT(
                    sto_from_json({ ...accRoot, SomeInvalidField: '123' }) === INVALID_ARGUMENT
                )

                // Invalid value type
                // TODO: Should be invalid https://github.com/Xahau/xahaud/issues/458
                // ASSERT(sto_from_json({ ...accRoot, Balance: 123 }) === INVALID_ARGUMENT)

                accept('success', 0)
            }
        )[test.hook]"];

        // install the hook on alice
        env(ripple::test::jtx::hook(
                alice, {{hsov1(hook, 1, HSDROPS, overrideFlag)}}, 0),
            M("set sto_from_json"),
            HSFEE);
        env.close();

        // invoke the hook
        env(pay(bob, alice, XRP(1)), M("test sto_from_json"), fee(XRP(1)));
    }

    void
    testWithFeatures(FeatureBitset features)
    {
        testHooksOwnerDir(features);
        testHooksDisabled(features);
        testTxStructure(features);
        // // testInferHookSetOperation(); // Not Version Specific
        // // testParams(features); // Not Version Specific
        // // testGrants(features); // Not Version Specific

        testInstall(features);
        testDelete(features);
        testNSDelete(features);
        testCreate(features);
        testUpdate(features);
        testWithTickets(features);

        // // DA TODO: illegalfunc_wasm
        // // testWasm(features);
        test_accept(features);
        test_rollback(features);

        // testGuards(features); // Not Used in JSHooks

        // test_emit(features);  //
        // test_prepare(features);  // JS ONLY
        // test_etxn_burden(features);       // tested above
        // test_etxn_generation(features);   // tested above
        // test_otxn_burden(features);       // tested above
        // test_otxn_generation(features);   // tested above
        // test_etxn_details(features);   // C ONLY
        // test_etxn_fee_base(features);  //
        // test_etxn_nonce(features);     // C ONLY
        // test_etxn_reserve(features);   //

        // test_fee_base(features);       //
        test_otxn_field(features);  //
        // test_ledger_keylet(features);  //

        test_float_compare(features);   //
        test_float_divide(features);    //
        test_float_int(features);       //
        test_float_invert(features);    //
        test_float_log(features);       //
        test_float_mantissa(features);  //
        test_float_mulratio(features);  //
        test_float_multiply(features);  //
        test_float_negate(features);    //
        test_float_one(features);       //
        test_float_root(features);      //
        test_float_set(features);       //
        test_float_sign(features);      //
        test_float_sto(features);       //
        test_float_sto_set(features);   //
        test_float_sum(features);       //

        test_hook_account(features);    //
        test_hook_again(features);      //
        test_hook_hash(features);       //
        test_hook_param(features);      //
        test_hook_param_set(features);  //
        test_hook_pos(features);        //
        test_hook_skip(features);       //

        test_ledger_last_hash(features);  //
        test_ledger_last_time(features);  //
        test_ledger_nonce(features);      //
        test_ledger_seq(features);        //

        // test_meta_slot(features);  //
        // test_xpop_slot(features);  //

        test_otxn_id(features);    //
        test_otxn_slot(features);  //
        // test_otxn_type(features);   // tequ: Assertion failed:
        // (list_empty(&rt->gc_obj_list))
        test_otxn_param(features);  //
        // test_otxn_json(features);  // JS ONLY // tequ: Assertion failed:
        // (list_empty(&rt->gc_obj_list))

        // test_slot(features);           // tequ: Assertion failed:
        // (list_empty(&rt->gc_obj_list))
        test_slot_clear(features);     //
        test_slot_count(features);     //
        test_slot_float(features);     //
        test_slot_set(features);       //
        test_slot_size(features);      //
        test_slot_subarray(features);  //
        test_slot_subfield(features);  //
        test_slot_type(features);      //
        test_slot_json(features);      // JS ONLY

        test_state(features);              //
        test_state_foreign(features);      //
        test_state_foreign_set(features);  //
        // test_state_foreign_set_max(features);  // Not Version Specific
        test_state_set(features);  //

        test_sto_emplace(features);    //
        test_sto_erase(features);      //
        test_sto_subarray(features);   //
        test_sto_subfield(features);   //
        test_sto_validate(features);   //
        test_sto_to_json(features);    // JS ONLY
        test_sto_from_json(features);  // JS ONLY

        test_trace(features);  //
        // test_trace_float(features);  // C ONLY
        // test_trace_num(features);    // C ONLY

        test_util_accid(features);    //
        test_util_keylet(features);   //
        test_util_raddr(features);    //
        test_util_sha512h(features);  //
        test_util_verify(features);   //
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeatures(sa);
    }

private:
    TestHook accept_wasm =  // WASM: 0
        jswasm[
            R"[test.hook](
            const Hook = (arg) => {
            return accept("0", 0);
            }
        )[test.hook]"];

    HASH_WASM(accept);

    TestHook rollback_wasm =  // WASM: 1
        jswasm[
            R"[test.hook](
            const Hook = (arg) => {
            return rollback("0", 0);
            }
        )[test.hook]"];

    HASH_WASM(rollback);

    TestHook illegalfunc_wasm =  // WASM: 3
        jswasm[
            R"[test.hook](
            const Hook = (arg) => {
            console.log("HERE");
            return accept(ret, 0);
            }
        )[test.hook]"];

    TestHook long_wasm =  // WASM: 4
        jswasm[
            R"[test.hook](
            const M_REPEAT_10 = (X) => X.repeat(10);
            const M_REPEAT_100 = (X) => M_REPEAT_10(X).repeat(10);
            const M_REPEAT_1000 = (X) => M_REPEAT_100(X).repeat(10);
            const Hook = (arg) => {
            const ret = M_REPEAT_1000("abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz01234567890123");
            return accept(ret, 0);
            }
        )[test.hook]"];

    TestHook makestate_wasm =  // WASM: 5
        jswasm[
            R"[test.hook](
            const Hook = (arg) => {
            const test_key = "0000000000000000000000000000000000000000000000006b657900";
            const test_value = "76616C756500";
            return accept("0", state_set(test_value, test_key));
            }
        )[test.hook]"];

    HASH_WASM(makestate);

    // this is just used as a second small hook with a unique hash
    TestHook accept2_wasm =  // WASM: 6
        jswasm[
            R"[test.hook](
            const Hook = (arg) => {
            return accept("0", 2);
            }
        )[test.hook]"];

    HASH_WASM(accept2);
};
BEAST_DEFINE_TESTSUITE(SetJSHook, app, ripple);
}  // namespace test
}  // namespace ripple
#undef M
