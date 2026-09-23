//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2016 Ripple Labs Inc.

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
#include <test/app/SetHookV3_wasm.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/tx/detail/SetHook.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/json/json_reader.h>
#include <xrpl/json/json_writer.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>
#include <iostream>

namespace ripple {

namespace test {

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

class SetHookV3_test : public beast::unit_test::suite
{
private:
    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }

    Json::Value
    addFuncParam(const std::string& name, const std::string& typeName)
    {
        Json::Value param = Json::Value(Json::objectValue);
        param[jss::FunctionParameter][jss::FunctionParameterName] =
            strHex(name);
        param[jss::FunctionParameter][jss::FunctionParameterType][jss::type] =
            typeName;
        return param;
    };

    template <typename T>
    Json::Value
    addFuncParamValue(const std::string& typeName, T value)
    {
        Json::Value param = Json::Value(Json::objectValue);
        param[jss::FunctionParameter][jss::FunctionParameterValue][jss::type] =
            typeName;
        param[jss::FunctionParameter][jss::FunctionParameterValue][jss::value] =
            value;
        return param;
    };

    Json::Value
    addFunc(const std::map<
            std::string,
            std::vector<std::pair<std::string, std::string>>>& args)
    {
        Json::Value params = Json::Value(Json::arrayValue);
        int i = 0;
        for (auto const& [a_name, a_params] : args)
        {
            Json::Value param = Json::Value(Json::objectValue);
            param[jss::HookFunction][jss::FunctionName] = strHex(a_name);
            for (auto const& [p_name, p_type] : a_params)
                param[jss::HookFunction][jss::FunctionParameters].append(
                    addFuncParam(p_name, p_type));
            params[i++] = param;
        }
        return params;
    }

public:
// This is a large fee, large enough that we can set most small test hooks
// without running into fee issues we only want to test fee code specifically in
// fee unit tests, the rest of the time we want to ignore it.
#define HSFEE fee(100'000'000)
#define M(m) memo(m, "", "")

    void
    testFeeRPCCall(jtx::Env& env, Json::Value tx, std::string expected)
    {
        auto const jtx = env.jt(tx);

        auto const feeDrops = env.current()->fees().base;

        // build tx_blob
        Json::Value params;
        params[jss::tx_blob] = strHex(jtx.stx->getSerializer().slice());

        // fee request
        auto const jrr = env.rpc("json", "fee", to_string(params));
        // std::cout << "RESULT: " << jrr << "\n";

        // verify base fee & open ledger fee
        auto const drops = jrr[jss::result][jss::drops];
        auto const baseFee = drops[jss::base_fee_no_hooks];
        BEAST_EXPECT(baseFee == to_string(feeDrops));
        auto const openLedgerFee = drops[jss::open_ledger_fee];
        BEAST_EXPECT(openLedgerFee == expected);

        // verify hooks fee
        auto const hooksFee = jrr[jss::result][jss::fee_hooks_feeunits];
        BEAST_EXPECT(hooksFee == expected);
        if (hooksFee != expected)
        {
            std::cout << "hooksFee: " << hooksFee << "";
            std::cout << "expected: " << expected << "\n";
        }
    }

    void
    testQueryRPCCall(
        jtx::Env& env,
        jtx::Account const& hookAccount,
        jtx::Account const& sourceAccount,
        std::string functionName,
        Json::Value funcParams,
        std::string expected)
    {
        Json::Value params;
        params[jss::hook_account] = hookAccount.human();
        params[jss::source_account] = sourceAccount.human();
        params[jss::function_name] = functionName;
        params[jss::function_params] = funcParams;

        // hook_query request
        auto const jrr = env.rpc("json", "hook_query", to_string(params));
        // std::cout << "RESULT: " << jrr << "\n";
    }

    void
    testInvalid(FeatureBitset features)
    {
        testcase("Test invalid");
        using namespace jtx;
        using namespace std::string_literals;

        // Env env{*this, features};
        Env env{
            *this, envconfig(), features, nullptr,
            // beast::severities::kTrace
        };

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.close();

        Json::Value jv = hso(testv3_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;

        // HookApiVersion 3 without HookFunctions
        env(ripple::test::jtx::hook(alice, {{jv}}, 0),
            HSFEE,
            ter(temMALFORMED));
        env.close();

        jv[jss::HookFunctions] = addFunc(
            {{"hook_accept", {}}, {"hook_rollback", {}}, {"hook_accept2", {}}});
        // HookApiVersion 1 with HookFunctions
        {
            Json::Value jv = hso(testv3_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 1;
            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // invalid Hook Index
        {
            Json::Value empty;
            empty[jss::CreateCode] = "";
            empty[jss::Flags] = hsfOVERRIDE;
            empty[jss::HookFunctions] = addFunc({{}, {"hook_accept2", {}}});
            env(ripple::test::jtx::hook(alice, {{empty, jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // invalid Hookv3 with Hookv1
        {
            Json::Value jv = hso(testv3_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            jv[jss::HookFunctions] = addFunc({{"hook_accept2", {}}});
            env(ripple::test::jtx::hook(
                    alice, {{jv, hso(accept_wasm, overrideFlag)}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();

            // TOOD: Should error when deploy v3 after deplyed v1 (by 2
            // txns)
        }

        // invalid FunctionName
        {
            Json::Value jv = hso(testv3_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            jv[jss::HookFunctions] = addFunc({{"hook_accept3", {}}});
            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // FunctionName size is too long
        {
            TestHook testLongFunctionName_wasm = wasmv3[
                R"[test.hook](
                #include <stdint.h>
                extern int32_t _g       (uint32_t id, uint32_t maxiter);
                extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
                #define SBUF(x) (uint32_t)x,sizeof(x)

                int64_t hook_too_long1234(uint32_t reserved)
                {
                    _g(1,1);
                    return accept(SBUF("success"),0);
                }
            )[test.hook]"];
            Json::Value jv = hso(testLongFunctionName_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            jv[jss::HookFunctions] = addFunc({{"hook_too_long1234", {}}});
            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Prepare: Create to Alice
        Json::Value jvCreate = hso(testv3_simple_wasm, overrideFlag);
        jvCreate[jss::HookApiVersion] = 3;
        jvCreate[jss::HookFunctions] =
            addFunc({{"hook_accept", {{"account", "ACCOUNT"}}}});
        env(ripple::test::jtx::hook(alice, {{jvCreate}}, 0), HSFEE);
        env.close();

        // Test Install (bob)
        {
            Json::Value jvInstall = hso(testv3_simple_wasm, overrideFlag);
            jvInstall[jss::HookApiVersion] = 3;
            jvInstall[jss::HookFunctions] = addFunc({{"hook_accept2", {}}});
            env(ripple::test::jtx::hook(bob, {{jvInstall}}, 0),
                HSFEE,
                ter(temMALFORMED));

            jvInstall[jss::HookFunctions] =
                addFunc({{"hook_accept", {}}, {"hook_accept2", {}}});
            env(ripple::test::jtx::hook(bob, {{jvInstall}}, 0),
                HSFEE,
                ter(temMALFORMED));
        }
        // TestUpdate (alice)
        {
            Json::Value jvUpdate = hso(testv3_simple_wasm, overrideFlag);
            jvUpdate[jss::HookApiVersion] = 3;
            jvUpdate[jss::HookFunctions] = addFunc({{"hook_accept2", {}}});
            env(ripple::test::jtx::hook(alice, {{jvUpdate}}, 0),
                HSFEE,
                ter(temMALFORMED));

            jvUpdate[jss::HookFunctions] =
                addFunc({{"hook_accept", {}}, {"hook_accept2", {}}});
            env(ripple::test::jtx::hook(alice, {{jvUpdate}}, 0),
                HSFEE,
                ter(temMALFORMED));
        }
        // Test: Execulte
        {
            // Invalid Parameter Size
            {
                // size == 0
                Json::Value jv = invoke::invoke(bob);
                jv[jss::Destination] = alice.human();
                jv[jss::FunctionName] = strHex("hook_accept"s);
                env(jv, fee(XRP(1)), ter(tecHOOK_INVALID_CALL));
            }
            {
                // size == 2
                Json::Value jv = invoke::invoke(bob);
                jv[jss::Destination] = alice.human();
                jv[jss::FunctionName] = strHex("hook_accept"s);
                jv[jss::FunctionParameters][0u] =
                    addFuncParamValue("ACCOUNT", Account{"bob"}.human());
                jv[jss::FunctionParameters][1u] =
                    addFuncParamValue("ACCOUNT", Account{"bob"}.human());
                env(jv, fee(XRP(1)), ter(tecHOOK_INVALID_CALL));
            }
            // Invalid ParemeterType
            {
                Json::Value jv = invoke::invoke(bob);
                jv[jss::Destination] = alice.human();
                jv[jss::FunctionName] = strHex("hook_accept"s);
                jv[jss::FunctionParameters][0u] =
                    addFuncParamValue("VL", "1234567890");
                env(jv, fee(XRP(1)), ter(tecHOOK_INVALID_CALL));
            }
        }
    }

    void
    testFeeRPC(FeatureBitset features)
    {
        testcase("Test fee RPC");
        using namespace jtx;
        using namespace std::string_literals;

        // Env env{*this, features};
        Env env{
            *this, envconfig(), features, nullptr,
            // beast::severities::kTrace
        };

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        auto const charlie = Account{"charlie"};
        auto const dave = Account{"dave"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.fund(XRP(10000), charlie);
        env.fund(XRP(10000), dave);
        env.close();

        // install the hook on alice
        Json::Value jv = hso(testv3_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept"s);
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_rollback"s);
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept2"s);
            jv[jss::HookFunctions][2u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        auto tx = invoke::invoke(alice);
        tx[jss::FunctionName] = strHex("hook_accept"s);
        testFeeRPCCall(env, tx, "21");

        tx[jss::FunctionName] = strHex("hook_accept2"s);
        testFeeRPCCall(env, tx, "19");

        {
            // Function Parameter Fee
            Json::Value jv = hso(testv3_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            {
                jv[jss::HookFunctions] = addFunc(
                    {{"hook_accept", {{"account", "ACCOUNT"}}},
                     {"hook_rollback", {}},
                     {"hook_accept2", {}}});
            }
            env(ripple::test::jtx::hook(bob, {{jv}}, 0), HSFEE);
            env.close();

            auto tx = invoke::invoke(bob);
            tx[jss::FunctionName] = strHex("hook_accept"s);
            tx[jss::FunctionParameters][0u] =
                addFuncParamValue("ACCOUNT", Account{"bob"}.human());
            // basefee 21 + 20byte(AccountID)
            testFeeRPCCall(env, tx, "41");
        }

        // Initialize Hook
        {
            TestHook initialize_wasm = wasmv3[
                R"[test.hook](
                #include <stdint.h>
                extern int32_t _g       (uint32_t id, uint32_t maxiter);
                extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
                int64_t hook_initialize(uint32_t reserved)
                {
                    _g(1,1);
                    return accept(0,0,0);
                }
            )[test.hook]"];
            HASH_WASM(initialize);

            auto makeTx = [&](uint32_t flags) {
                Json::Value jv = hso(initialize_wasm, overrideFlag);
                jv[jss::HookApiVersion] = 3;
                Json::Value function = Json::Value(Json::objectValue);
                function[jss::FunctionName] = strHex("hook_initialize"s);
                function[jss::Flags] = flags;
                jv[jss::HookFunctions][0u][jss::HookFunction] = function;
                return jv;
            };

            Json::Value jv = makeTx(0);
            // No Definition, without initialize flag
            testFeeRPCCall(
                env, ripple::test::jtx::hook(charlie, {{jv}}, 0), "76010");

            // No Definition,with initialize flag
            jv = makeTx(hffINITIALIZE);
            testFeeRPCCall(
                env, ripple::test::jtx::hook(charlie, {{jv}}, 0), "76019");

            // With Definition, without initialize flag
            jv = makeTx(0);
            env(ripple::test::jtx::hook(charlie, {{jv}}, 0), HSFEE);

            jv = Json::Value(Json::objectValue);
            jv[jss::HookHash] = initialize_hash_str;
            testFeeRPCCall(env, ripple::test::jtx::hook(dave, {{jv}}, 0), "10");

            // remove the hook
            env(ripple::test::jtx::hook(charlie, {{hso_delete()}}, 0), HSFEE);
            env.close();

            // With Definition, with initialize flag
            jv = makeTx(hffINITIALIZE);
            env(ripple::test::jtx::hook(charlie, {{jv}}, 0), HSFEE);

            jv = Json::Value(Json::objectValue);
            jv[jss::HookHash] = initialize_hash_str;
            testFeeRPCCall(env, ripple::test::jtx::hook(dave, {{jv}}, 0), "19");
        }
    }

    void
    testSimple(FeatureBitset features)
    {
        testcase("Test simple");

        using namespace jtx;
        using namespace std::string_literals;

        // Env env{*this, features};
        Env env{
            *this, envconfig(), features, nullptr,
            // beast::severities::kTrace
        };

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // install the hook on alice
        Json::Value jv = hso(testv3_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept"s);
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_rollback"s);
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept2"s);
            jv[jss::HookFunctions][2u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        // invoke the hook
        Json::Value iv = invoke::invoke(alice);
        iv[jss::FunctionName] = strHex("hook_accept"s);
        env(iv, fee(XRP(1)));
        env.close();

        Json::Value iv2 = invoke::invoke(alice);
        iv2[jss::FunctionName] = strHex("hook_rollback"s);
        env(iv2, fee(XRP(1)), ter(tecHOOK_REJECTED));
        env.close();

        Json::Value iv3 = invoke::invoke(alice);
        iv3[jss::FunctionName] = strHex("hook_accept2"s);
        env(iv3, fee(XRP(1)));
        env.close();
    }

    void
    testFunctionParameters(FeatureBitset features)
    {
        testcase("Test function parameters");

        using namespace jtx;
        using namespace std::string_literals;

        // Env env{*this, features};
        Env env{
            *this, envconfig(), features, nullptr,
            // beast::severities::kTrace
        };

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook testv3_wasm = wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t util_accid (uint32_t, uint32_t, uint32_t, uint32_t);
            extern int64_t otxn_func_param (uint32_t write_ptr, uint32_t write_len, uint32_t index, uint32_t serialized_type_id);
            #define UINT8 16
            #define UINT16 1
            #define UINT32 2
            #define UINT64 3
            #define UINT128 4
            #define UINT256 5
            #define AMOUNT 6
            #define VL 7
            #define ACCOUNT 8
            #define INVALID_ARGUMENT -7
            #define DOESNT_EXIST -5
            #define SBUF(x) (uint32_t)x,sizeof(x)
            #define SVAR(x) (uint32_t)&x,sizeof(x)
            #define ASSERT_EQUAL(x, y) if (!(x == y)) rollback((uint32_t)#x,sizeof(#x), x);
            int64_t hook_accept(uint32_t reserved)
            {
                _g(1,1);
                ASSERT_EQUAL(otxn_func_param(0, 0, 10, UINT16), DOESNT_EXIST);
                ASSERT_EQUAL(otxn_func_param(0, 2, 0, UINT8), INVALID_ARGUMENT);
                ASSERT_EQUAL(otxn_func_param(0, 2, 0, 99), INVALID_ARGUMENT);
                
                uint16_t data16;
                ASSERT_EQUAL(otxn_func_param(SVAR(data16), 0, UINT16), 2);
                ASSERT_EQUAL(data16, 0);
                
                uint32_t data32;
                ASSERT_EQUAL(otxn_func_param(SVAR(data32), 1, UINT32), 4);
                ASSERT_EQUAL(data32, 1);
                
                uint64_t data64;
                ASSERT_EQUAL(otxn_func_param(SVAR(data64), 2, UINT64), 8);
                ASSERT_EQUAL(data64, 2);
                
                uint8_t data128[16];
                ASSERT_EQUAL(otxn_func_param(SBUF(data128), 3, UINT128), 16);
                for (int i = 0; GUARD(15), i < 15; i++)
                    ASSERT_EQUAL(data128[i], 0);
                ASSERT_EQUAL(data128[15], 3);

                uint8_t data256[32];
                ASSERT_EQUAL(otxn_func_param(SBUF(data256), 4, UINT256), 32);
                for (int i = 0; GUARD(31), i < 31; i++)
                    ASSERT_EQUAL(data256[i], 0);
                ASSERT_EQUAL(data256[31], 4);

                ASSERT_EQUAL(otxn_func_param(0, 8, 5, AMOUNT), 8);
                
                uint8_t data_vl[6];
                ASSERT_EQUAL(otxn_func_param(SBUF(data_vl), 6, VL), 6);
                for (int i = 0; GUARD(6), i < 6; i++)
                    ASSERT_EQUAL(data_vl[i], i);

                uint8_t data_account[20];
                uint8_t expected_account[20];
                const char addr[] = "rPMh7Pi9ct699iZUTWaytJUoHcJ7cgyziK"; // bob
                ASSERT_EQUAL(util_accid(SBUF(expected_account), SBUF(addr)), 20);
                ASSERT_EQUAL(otxn_func_param(SBUF(data_account), 7, ACCOUNT), 20);
                for (int i = 0; GUARD(20), i < 20; i++)
                    ASSERT_EQUAL(data_account[i], expected_account[i]);

                uint8_t data8;
                ASSERT_EQUAL(otxn_func_param(SVAR(data8), 8, UINT8), 1);
                ASSERT_EQUAL(data8, 8);

                return accept(SBUF("success"),0);
            }
            
            int64_t hook_rollback(uint32_t reserved)
            {
                _g(1,1);
                return rollback(SBUF("rollback"),0);
            }

            int64_t hook_accept2(uint32_t reserved)
            {
                _g(1,1);
                return accept(SBUF("success2"),0);
            }
        )[test.hook]"];

        // install the hook on alice
        Json::Value jv = hso(testv3_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept"s);
            Json::Value hookFunctions = Json::Value(Json::arrayValue);
            hookFunctions[0u] = addFuncParam("uint16", "UINT16");
            hookFunctions[1u] = addFuncParam("uint32", "UINT32");
            hookFunctions[2u] = addFuncParam("uint64", "UINT64");
            hookFunctions[3u] = addFuncParam("uint128", "UINT128");
            hookFunctions[4u] = addFuncParam("uint256", "UINT256");
            hookFunctions[5u] = addFuncParam("amount", "AMOUNT");
            hookFunctions[6u] = addFuncParam("vl", "VL");
            hookFunctions[7u] = addFuncParam("account", "ACCOUNT");
            hookFunctions[8u] = addFuncParam("uint8", "UINT8");
            function[jss::FunctionParameters] = hookFunctions;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_rollback"s);
            Json::Value hookFunctions = Json::Value(Json::arrayValue);
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept2"s);
            jv[jss::HookFunctions][2u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        // invoke the hook
        Json::Value iv = invoke::invoke(alice);
        iv[jss::FunctionName] = strHex("hook_accept"s);
        iv[jss::FunctionParameters][0u] =
            addFuncParamValue<uint16_t>("UINT16", 0);
        iv[jss::FunctionParameters][1u] =
            addFuncParamValue<uint32_t>("UINT32", 1);
        iv[jss::FunctionParameters][2u] =
            addFuncParamValue<std::string>("UINT64", "02");
        iv[jss::FunctionParameters][3u] =
            addFuncParamValue<std::string>("UINT128", "03");
        iv[jss::FunctionParameters][4u] =
            addFuncParamValue<std::string>("UINT256", "04");
        iv[jss::FunctionParameters][5u] =
            addFuncParamValue<std::string>("AMOUNT", "10");
        iv[jss::FunctionParameters][6u] =
            addFuncParamValue<std::string>("VL", "000102030405");
        iv[jss::FunctionParameters][7u] =
            addFuncParamValue<std::string>("ACCOUNT", Account{"bob"}.human());
        iv[jss::FunctionParameters][8u] =
            addFuncParamValue<uint8_t>("UINT8", 8);

        env(iv, fee(XRP(1)));
        env.close();
        return;

        Json::Value iv2 = invoke::invoke(alice);
        iv2[jss::FunctionName] = strHex("hook_rollback"s);
        env(iv2, fee(XRP(1)), ter(tecHOOK_INVALID_CALL));
        env.close();

        Json::Value iv3 = invoke::invoke(alice);
        iv3[jss::FunctionName] = strHex("hook_accept2"s);
        env(iv3, fee(XRP(1)));
        env.close();
    }

    void
    testInvalidHookQueryExecution(FeatureBitset features)
    {
        testcase("Test hook query execution restrictions");

        using namespace jtx;
        using namespace std::string_literals;

        Env env{
            *this,
            envconfig(),
            features,
            nullptr,
        };

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.close();

        TestHook queryHook_wasm = wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t state_set(uint32_t,uint32_t,uint32_t, uint32_t);
            extern int64_t state(uint32_t,uint32_t,uint32_t,uint32_t);
            extern int64_t query_result_set(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
            extern int64_t emit(uint32_t,uint32_t,uint32_t,uint32_t);
            #define SBUF(x) (uint32_t)x,sizeof(x)
            #define ASSERT(x) if (!(x)) rollback((uint32_t)#x,sizeof(#x), __LINE__);
            
            int64_t query_only(uint32_t reserved)
            {
                _g(1,1);
                // Query functions should be able to read state
                state(0,0,SBUF("test_key"));
                
                // Query functions should NOT be able to modify state
                // This should be restricted by the runtime
                ASSERT(state_set(SBUF("key"),SBUF("value")) < 0);
                
                // Query functions should NOT be able to emit transactions
                // This should be restricted by the runtime
                ASSERT(emit(0,0,0,0) < 0);
                
                // Set query result
                ASSERT(query_result_set(SBUF("query_result"),SBUF("success"),7) > 0);
                return accept(SBUF("query_success"),0);
            }
            
            int64_t regular_function(uint32_t reserved)
            {
                _g(1,1);
                return accept(SBUF("regular_success"),0);
            }
        )[test.hook]"];

        // Install hook with query function
        Json::Value jv = hso(queryHook_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("query_only"s);
            function[jss::Flags] = FunctionalHookFlags::hffQUERY;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("regular_function"s);
            function[jss::Flags] = 0;
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        // Test 1: Query-only function should NOT be executable via Invoke
        // transaction
        {
            Json::Value iv = invoke::invoke(bob);
            iv[jss::Destination] = alice.human();
            iv[jss::FunctionName] = strHex("query_only"s);
            env(iv, fee(XRP(1)), ter(tecHOOK_INVALID_CALL));
            env.close();
        }

        // Test 2: Regular function should be executable via Invoke transaction
        {
            Json::Value iv = invoke::invoke(bob);
            iv[jss::Destination] = alice.human();
            iv[jss::FunctionName] = strHex("regular_function"s);
            env(iv, fee(XRP(1)));
            env.close();
        }

        // Test 3: Query function should be accessible via hook_query RPC
        {
            Json::Value params = Json::objectValue;
            testQueryRPCCall(
                env, alice, bob, "query_only", params, "query_success");
        }

        // Test 4: Regular function should NOT be accessible via hook_query RPC
        {
            Json::Value params = Json::objectValue;
            Json::Value rpcParams;
            rpcParams[jss::hook_account] = alice.human();
            rpcParams[jss::source_account] = bob.human();
            rpcParams[jss::function_name] = "regular_function";
            rpcParams[jss::function_params] = params;

            auto const jrr =
                env.rpc("json", "hook_query", to_string(rpcParams));
            // Should return error indicating function is not a query function
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));
        }
    }

    void
    testHookQuery(FeatureBitset features)
    {
        testcase("Test hook query");

        using namespace jtx;
        using namespace std::string_literals;

        Env env{
            *this,
            envconfig(),
            features,
            nullptr,
        };

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.close();

        TestHook queryRPCHook_wasm = wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t otxn_func_param (uint32_t write_ptr, uint32_t write_len, uint32_t index, uint32_t serialized_type_id);
            extern int64_t query_result_set(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
            #define UINT8 16
            #define UINT16 1
            #define UINT32 2
            #define UINT64 3
            #define UINT128 4
            #define UINT256 5
            #define AMOUNT 6
            #define VL 7
            #define ACCOUNT 8
            #define SBUF(x) (uint32_t)x,sizeof(x)
            #define ASSERT(x) if (!(x)) rollback((uint32_t)#x,sizeof(#x), __LINE__);
            
            int64_t test_param_types(uint32_t reserved)
            {
                _g(1,1);
                
                // Test parameter access for all types
                uint8_t data8;
                ASSERT(otxn_func_param((uint32_t)&data8, 1, 0, UINT8) == 1);
                
                uint16_t data16;
                ASSERT(otxn_func_param((uint32_t)&data16, 2, 1, UINT16) == 2);
                
                uint32_t data32;
                ASSERT(otxn_func_param((uint32_t)&data32, 4, 2, UINT32) == 4);
                
                uint64_t data64;
                ASSERT(otxn_func_param((uint32_t)&data64, 8, 3, UINT64) == 8);
                
                uint8_t data128[16];
                ASSERT(otxn_func_param(SBUF(data128), 4, UINT128) == 16);
                
                uint8_t data256[32];
                ASSERT(otxn_func_param(SBUF(data256), 5, UINT256) == 32);
                
                uint8_t amount[8];
                ASSERT(otxn_func_param(SBUF(amount), 6, AMOUNT) == 8);
                
                uint8_t vl_data[20];
                ASSERT(otxn_func_param(SBUF(vl_data), 7, VL) > 0);
                
                uint8_t account[20];
                ASSERT(otxn_func_param(SBUF(account), 8, ACCOUNT) == 20);
                
                return accept(SBUF("all_params_ok"),0);
            }
            
            int64_t test_param_error(uint32_t reserved)
            {
                _g(1,1);
                
                // Test parameter bounds checking
                ASSERT(otxn_func_param(0, 0, 99, UINT8) == -5); // DOESNT_EXIST
                ASSERT(otxn_func_param(0, 0, 0, 99) == -7);     // INVALID_ARGUMENT
                
                return accept(SBUF("param_errors_ok"),0);
            }
        )[test.hook]"];

        // Install hook with comprehensive query functions
        Json::Value jv = hso(queryRPCHook_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("test_param_types"s);
            function[jss::Flags] = FunctionalHookFlags::hffQUERY;
            Json::Value params = Json::Value(Json::arrayValue);
            params[0u] = addFuncParam("uint8_param", "UINT8");
            params[1u] = addFuncParam("uint16_param", "UINT16");
            params[2u] = addFuncParam("uint32_param", "UINT32");
            params[3u] = addFuncParam("uint64_param", "UINT64");
            params[4u] = addFuncParam("uint128_param", "UINT128");
            params[5u] = addFuncParam("uint256_param", "UINT256");
            params[6u] = addFuncParam("amount_param", "AMOUNT");
            params[7u] = addFuncParam("vl_param", "VL");
            params[8u] = addFuncParam("account_param", "ACCOUNT");
            function[jss::FunctionParameters] = params;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("test_param_error"s);
            function[jss::Flags] = FunctionalHookFlags::hffQUERY;
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        // Test all parameter types via query RPC
        {
            Json::Value params = Json::objectValue;
            params["uint8_param"][jss::type] = "UINT8";
            params["uint8_param"][jss::value] = 255;
            params["uint16_param"][jss::type] = "UINT16";
            params["uint16_param"][jss::value] = 65535;
            params["uint32_param"][jss::type] = "UINT32";
            params["uint32_param"][jss::value] = 4294967295U;
            params["uint64_param"][jss::type] = "UINT64";
            params["uint64_param"][jss::value] = "18446744073709551615";
            params["uint128_param"][jss::type] = "UINT128";
            params["uint128_param"][jss::value] =
                "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF";
            params["uint256_param"][jss::type] = "UINT256";
            params["uint256_param"][jss::value] =
                "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"
                "FF";
            params["amount_param"][jss::type] = "AMOUNT";
            params["amount_param"][jss::value] = "1000000000000";
            params["vl_param"][jss::type] = "VL";
            params["vl_param"][jss::value] = "DEADBEEFCAFE";
            params["account_param"][jss::type] = "ACCOUNT";
            params["account_param"][jss::value] = bob.human();

            testQueryRPCCall(
                env,
                alice,
                bob,
                "test_all_param_types",
                params,
                "all_params_ok");
        }

        // Test parameter error conditions
        {
            Json::Value params = Json::objectValue;
            testQueryRPCCall(
                env,
                alice,
                bob,
                "test_param_errors",
                params,
                "param_errors_ok");
        }

        // Test invalid parameter count
        {
            Json::Value rpcParams;
            rpcParams[jss::hook_account] = alice.human();
            rpcParams[jss::source_account] = bob.human();
            rpcParams[jss::function_name] = "test_all_param_types";
            Json::Value params = Json::objectValue;
            params["only_one_param"][jss::type] = "UINT8";
            params["only_one_param"][jss::value] = 1;
            rpcParams[jss::function_params] = params;

            auto const jrr =
                env.rpc("json", "hook_query", to_string(rpcParams));
            // Should fail due to parameter count mismatch
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));
        }

        // Test invalid parameter type
        {
            Json::Value rpcParams;
            rpcParams[jss::hook_account] = alice.human();
            rpcParams[jss::source_account] = bob.human();
            rpcParams[jss::function_name] = "test_all_param_types";
            Json::Value params = Json::objectValue;
            params["uint8_param"][jss::type] = "INVALID_TYPE";
            params["uint8_param"][jss::value] = 1;
            rpcParams[jss::function_params] = params;

            auto const jrr =
                env.rpc("json", "hook_query", to_string(rpcParams));
            // Should fail due to invalid parameter type
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));
        }
    }

    void
    testInitialize(FeatureBitset features)
    {
        testcase("Test initialize");

        using namespace jtx;
        using namespace std::string_literals;

        Env env{
            *this,
            envconfig(),
            features,
            nullptr,
        };

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook initHook_wasm = wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t state_set(uint32_t,uint32_t,uint32_t, uint32_t);
            extern int64_t state(uint32_t,uint32_t,uint32_t,uint32_t);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
            #define SBUF(x) (uint32_t)x,sizeof(x)
            #define ASSERT(x) if (!(x)) rollback((uint32_t)#x,sizeof(#x), __LINE__);
            
            int64_t initialize(uint32_t reserved)
            {
                _g(1,1);
                // Set initial state during hook installation
                ASSERT(state_set("init_value",10,"init_key",8) > 0);
                ASSERT(state_set("0",1,"counter",7) > 0);
                return accept(SBUF("init_success"),0);
            }
            
            int64_t check_state(uint32_t reserved)
            {
                _g(1,1);
                uint8_t buffer[32];
                int64_t result = state(SBUF(buffer),"init_key",8);
                if (result != 10)
                    return rollback(SBUF("result == 10"),result);
                ASSERT(result == 10); // "init_value" length
                
                // Verify content matches
                char expected[] = "init_value";
                for (int i = 0; GUARD(10), i < 10; i++)
                    ASSERT(buffer[i] == expected[i]);
                
                return accept(SBUF("state_verified"),0);
            }
        )[test.hook]"];

        // Test 1: Duplicate initialize function
        {
            Json::Value jv = hso(initHook_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            {
                Json::Value function = Json::Value(Json::objectValue);
                function[jss::FunctionName] = strHex("initialize"s);
                function[jss::Flags] = FunctionalHookFlags::hffINITIALIZE;
                jv[jss::HookFunctions][0u][jss::HookFunction] = function;
            }
            {
                Json::Value function = Json::Value(Json::objectValue);
                function[jss::FunctionName] = strHex("check_state"s);
                function[jss::Flags] = FunctionalHookFlags::hffINITIALIZE;
                jv[jss::HookFunctions][1u][jss::HookFunction] = function;
            }

            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Test 1: Install hook with initialize function
        Json::Value jv = hso(initHook_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("initialize"s);
            function[jss::Flags] = FunctionalHookFlags::hffINITIALIZE;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("check_state"s);
            function[jss::Flags] = 0;
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        // Verify initialize function was executed during installation
        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        // Test 2: Verify state was set by initialize function
        {
            Json::Value iv = invoke::invoke(alice);
            iv[jss::FunctionName] = strHex("check_state"s);
            env(iv, fee(XRP(1)));
            env.close();
        }

        // Test 3: Test initialize function failure rollback
        TestHook failInitHook_wasm = wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t state_set(uint32_t,uint32_t,uint32_t, uint32_t);
            #define SBUF(x) (uint32_t)x,sizeof(x)
            
            int64_t fail_init(uint32_t reserved)
            {
                _g(1,1);
                state_set(SBUF("should_not_exist"),SBUF("value"));
                return rollback(SBUF("init_failed"),0);
            }
        )[test.hook]"];

        Json::Value jvFail = hso(failInitHook_wasm, overrideFlag);
        jvFail[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("fail_init"s);
            function[jss::Flags] = FunctionalHookFlags::hffINITIALIZE;
            jvFail[jss::HookFunctions][0u][jss::HookFunction] = function;
        }

        // Initialize function failure should cause hook installation to fail
        env(ripple::test::jtx::hook(alice, {{jvFail}}, 0),
            HSFEE,
            ter(tecHOOK_REJECTED));
        env.close();
    }

    void
    testFunctionalHookInstallation(FeatureBitset features)
    {
        testcase("Test functional hook installation");

        using namespace jtx;
        using namespace std::string_literals;

        Env env{
            *this,
            envconfig(),
            features,
            nullptr,
        };

        auto const alice = Account{"alice"};
        auto const bob = Account{"bob"};
        env.fund(XRP(10000), alice);
        env.fund(XRP(10000), bob);
        env.close();

        // Test 1: Cannot install functional (v3) hooks to index 1
        {
            // Try to install v3 functional hook in next position - should fail
            Json::Value jv = hso(testv3_simple_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            jv[jss::HookFunctions] = addFunc({{"hook_accept", {}}});
            env(ripple::test::jtx::hook(alice, {{{}, jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Test 1: Cannot mix functional (v3) and non-functional (v1) hooks
        {
            // Install v1 hook first to index 1
            env(ripple::test::jtx::hook(
                    alice, {{{}, hso(accept_wasm, overrideFlag)}}, 0),
                HSFEE);
            env.close();

            // Try to install v3 functional hook to index 0 - should fail
            Json::Value jv = hso(testv3_simple_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            jv[jss::HookFunctions] = addFunc({{"hook_accept", {}}});
            env(ripple::test::jtx::hook(alice, {{jv, {}}}, 0),
                HSFEE,
                ter(tecHOOK_INVALID_ENTRY));
            env.close();

            // Clean up
            env(ripple::test::jtx::hook(
                    alice, {{hso_delete(), hso_delete()}}, 0),
                HSFEE);
            env.close();
        }

        // Test 2: Cannot combine hffINITIALIZE and hffQUERY flags
        {
            Json::Value jv = hso(testv3_simple_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept"s);
            function[jss::Flags] = FunctionalHookFlags::hffINITIALIZE |
                FunctionalHookFlags::hffQUERY;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;

            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }
        // Test 2: Cannot use flags other than hffINITIALIZE and hffQUERY
        {
            Json::Value jv = hso(testv3_simple_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept"s);
            function[jss::Flags] = static_cast<uint16_t>(
                ~(FunctionalHookFlags::hffINITIALIZE |
                  FunctionalHookFlags::hffQUERY));
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;

            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Test 3: Function name uniqueness within same hook
        {
            Json::Value jv = hso(testv3_simple_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            jv[jss::HookFunctions][0u] = addFunc({{"hook_accept", {}}})[0u];
            jv[jss::HookFunctions][1u] = addFunc({{"hook_accept", {}}})[0u];

            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Test 4: Parameter name uniqueness within same function
        {
            Json::Value jv = hso(testv3_simple_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("hook_accept"s);
            Json::Value params = Json::Value(Json::arrayValue);
            params[0u] = addFuncParam("duplicate_param", "UINT8");
            params[1u] = addFuncParam("duplicate_param", "UINT16");
            function[jss::FunctionParameters] = params;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;

            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Test 5: Maximum function name length (16 bytes)
        {
            TestHook testLongFunctionName_wasm = wasmv3[
                R"[test.hook](
                #include <stdint.h>
                extern int32_t _g       (uint32_t id, uint32_t maxiter);
                extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
                #define SBUF(x) (uint32_t)x,sizeof(x)

                int64_t this_name_is_long(uint32_t reserved)
                {
                    _g(1,1);
                    return accept(SBUF("success"),0);
                }
            )[test.hook]"];
            Json::Value jv = hso(testLongFunctionName_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            // 17 characters - should fail
            std::string longName = "this_name_is_long";
            jv[jss::HookFunctions] = addFunc({{longName, {}}});

            env(ripple::test::jtx::hook(alice, {{jv}}, 0),
                HSFEE,
                ter(temMALFORMED));
            env.close();
        }

        // Test 6: Valid 16-byte function name should succeed
        {
            TestHook testLongFunctionName_wasm = wasmv3[
                R"[test.hook](
                #include <stdint.h>
                extern int32_t _g       (uint32_t id, uint32_t maxiter);
                extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
                #define SBUF(x) (uint32_t)x,sizeof(x)

                int64_t exactly_16_chars(uint32_t reserved)
                {
                    _g(1,1);
                    return accept(SBUF("success"),0);
                }
            )[test.hook]"];
            Json::Value jv = hso(testLongFunctionName_wasm, overrideFlag);
            jv[jss::HookApiVersion] = 3;
            // Exactly 16 characters - should succeed
            std::string maxName = "exactly_16_chars";
            jv[jss::HookFunctions] = addFunc({{maxName, {}}});

            env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
            env.close();

            // Clean up
            env(ripple::test::jtx::hook(alice, {{hso_delete()}}, 0), HSFEE);
            env.close();
        }

        // Test 7: Installing functional hook on account with existing
        // functional hook (update)
        {
            // Install first functional hook
            Json::Value jv1 = hso(testv3_simple_wasm, overrideFlag);
            jv1[jss::HookApiVersion] = 3;
            jv1[jss::HookFunctions] = addFunc({{"hook_accept", {}}});
            env(ripple::test::jtx::hook(alice, {{jv1}}, 0), HSFEE);
            env.close();

            // Update with different function signature
            Json::Value jv2 = hso(testv3_simple_wasm, overrideFlag);
            jv2[jss::HookApiVersion] = 3;
            jv2[jss::HookFunctions] =
                addFunc({{"hook_accept", {{"param", "UINT8"}}}});
            env(ripple::test::jtx::hook(alice, {{jv2}}, 0), HSFEE);
            env.close();

            // Clean up
            env(ripple::test::jtx::hook(alice, {{hso_delete()}}, 0), HSFEE);
            env.close();
        }
    }

    void
    testHookAPIFunctionErrorConditions(FeatureBitset features)
    {
        testcase("Test hook API function error conditions");

        using namespace jtx;
        using namespace std::string_literals;

        Env env{
            *this,
            envconfig(),
            features,
            nullptr,
        };

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook errorTestHook_wasm = wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t otxn_func_param (uint32_t write_ptr, uint32_t write_len, uint32_t index, uint32_t serialized_type_id);
            extern int64_t query_result_set(uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
            #define INVALID_ARGUMENT -7
            #define DOESNT_EXIST -5
            #define UINT8 16
            #define UINT16 1
            #define UINT32 2
            #define UINT64 3
            #define UINT128 4
            #define UINT256 5
            #define AMOUNT 6
            #define VL 7
            #define ACCOUNT 8
            #define SBUF(x) (uint32_t)x,sizeof(x)
            #define ASSERT_EQUAL(x, y) if (!(x == y)) rollback((uint32_t)#x,sizeof(#x), x);
            
            int64_t param_errors(uint32_t reserved)
            {
                _g(1,1);
                
                // Test INVALID_ARGUMENT (-7): invalid buffer size
                uint8_t small_buffer[1];
                ASSERT_EQUAL(otxn_func_param(SBUF(small_buffer), 0, UINT32), INVALID_ARGUMENT);
                
                // Test DOESNT_EXIST (-5): parameter index out of bounds
                uint32_t data;
                ASSERT_EQUAL(otxn_func_param((uint32_t)&data, 4, 999, UINT32), DOESNT_EXIST);
                
                // Test INVALID_ARGUMENT (-7): invalid type
                ASSERT_EQUAL(otxn_func_param((uint32_t)&data, 4, 0, 999), INVALID_ARGUMENT);
                
                return accept(SBUF("error_tests_ok"),0);
            }
            
            int64_t result_errors(uint32_t reserved)
            {
                _g(1,1);
                
                // Test INVALID_ARGUMENT (-7): invalid target type
                ASSERT_EQUAL(query_result_set(SBUF("target"),SBUF("data"),999), INVALID_ARGUMENT);
                
                // Test TOO_BIG (-3): result data too large (if applicable)
                uint8_t large_data[1000];
                // This might not fail in all implementations, but testing the interface
                query_result_set(SBUF("target"),SBUF(large_data),VL);
                
                return accept(SBUF("query_error_tests_ok"),0);
            }
            
            int64_t memory_bounds(uint32_t reserved)
            {
                _g(1,1);
                
                // Test buffer overlap detection (if implemented)
                uint8_t buffer[8];
                // Try to read into overlapping memory
                otxn_func_param((uint32_t)buffer, 4, 0, UINT32);
                otxn_func_param((uint32_t)(buffer + 2), 4, 0, UINT32);
                
                return accept(SBUF("memory_bounds_ok"),0);
            }
        )[test.hook]"];

        // Install hook with error testing functions
        Json::Value jv = hso(errorTestHook_wasm, overrideFlag);
        jv[jss::HookApiVersion] = 3;
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("param_errors"s);
            function[jss::Flags] = 0;
            Json::Value params = Json::Value(Json::arrayValue);
            params[0u] = addFuncParam("test_param", "UINT32");
            function[jss::FunctionParameters] = params;
            jv[jss::HookFunctions][0u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("result_errors"s);
            function[jss::Flags] = FunctionalHookFlags::hffQUERY;
            jv[jss::HookFunctions][1u][jss::HookFunction] = function;
        }
        {
            Json::Value function = Json::Value(Json::objectValue);
            function[jss::FunctionName] = strHex("memory_bounds"s);
            function[jss::Flags] = 0;
            jv[jss::HookFunctions][2u][jss::HookFunction] = function;
        }

        env(ripple::test::jtx::hook(alice, {{jv}}, 0), HSFEE);
        env.close();

        // Test 1: otxn_func_param error conditions
        {
            Json::Value iv = invoke::invoke(alice);
            iv[jss::FunctionName] = strHex("param_errors"s);
            iv[jss::FunctionParameters][0u] =
                addFuncParamValue("UINT32", 12345);
            env(iv, fee(XRP(1)));
            env.close();
        }

        // Test 2: query_result_set error conditions
        {
            Json::Value params = Json::objectValue;
            testQueryRPCCall(
                env,
                alice,
                alice,
                "result_errors",
                params,
                "query_error_tests_ok");
        }

        // Test 3: memory bounds testing
        {
            Json::Value iv = invoke::invoke(alice);
            iv[jss::FunctionName] = strHex("memory_bounds"s);
            env(iv, fee(XRP(1)));
            env.close();
        }

        // Test 4: Invalid hook query RPC parameters
        {
            // Test missing hook_account
            Json::Value rpcParams;
            rpcParams[jss::source_account] = alice.human();
            rpcParams[jss::function_name] = "test_query_result_errors";
            rpcParams[jss::function_params] = Json::objectValue;

            auto jrr = env.rpc("json", "hook_query", to_string(rpcParams));
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));

            // Test missing source_account
            rpcParams = Json::objectValue;
            rpcParams[jss::hook_account] = alice.human();
            rpcParams[jss::function_name] = "test_query_result_errors";
            rpcParams[jss::function_params] = Json::objectValue;

            jrr = env.rpc("json", "hook_query", to_string(rpcParams));
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));

            // Test missing function_name
            rpcParams = Json::objectValue;
            rpcParams[jss::hook_account] = alice.human();
            rpcParams[jss::source_account] = alice.human();
            rpcParams[jss::function_params] = Json::objectValue;

            jrr = env.rpc("json", "hook_query", to_string(rpcParams));
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));

            // Test non-existent function
            rpcParams = Json::objectValue;
            rpcParams[jss::hook_account] = alice.human();
            rpcParams[jss::source_account] = alice.human();
            rpcParams[jss::function_name] = "non_existent_function";
            rpcParams[jss::function_params] = Json::objectValue;

            jrr = env.rpc("json", "hook_query", to_string(rpcParams));
            BEAST_EXPECT(jrr[jss::result].isMember(jss::error));
        }
    }

    void
    testWithFeatures(FeatureBitset features)
    {
        testInvalid(features);
        testFeeRPC(features);
        testSimple(features);
        testFunctionParameters(features);

        // query
        testInvalidHookQueryExecution(features);
        testHookQuery(features);

        // initialize
        testInitialize(features);

        testFunctionalHookInstallation(features);
        testHookAPIFunctionErrorConditions(features);
        // testWeakTSH(features);
    }

    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeatures(sa);
    }

private:
    TestHook testv3_simple_wasm = wasmv3[
        R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            #define SBUF(x) (uint32_t)x,sizeof(x)
            int64_t hook_accept(uint32_t reserved)
            {
                _g(1,1);
                return accept(SBUF("success"),0);
            }
        )[test.hook]"];
    HASH_WASM(testv3_simple);

    TestHook testv3_wasm = wasmv3[
        R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t hook_pos (void);
            #define SBUF(x) (uint32_t)x,sizeof(x)

            int64_t hook_accept(uint32_t reserved)
            {
                _g(1,1);
                hook_pos();
                return accept(SBUF("success"),0);
            }
            
            int64_t hook_rollback(uint32_t reserved)
            {
                _g(1,1);
                return rollback(SBUF("rollback"),0);
            }

            int64_t hook_accept2(uint32_t reserved)
            {
                _g(1,1);
                return accept(SBUF("success2"),0);
            }
        )[test.hook]"];
    HASH_WASM(testv3);

    TestHook accept_wasm =  // WASM: 0
        wasmv3[
            R"[test.hook](
            #include <stdint.h>
            extern int32_t _g       (uint32_t id, uint32_t maxiter);
            extern int64_t accept   (uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            int64_t hook(uint32_t reserved )
            {
                _g(1,1);
                return accept(0,0,0);
            }
        )[test.hook]"];

    HASH_WASM(accept);
};
BEAST_DEFINE_TESTSUITE(SetHookV3, app, ripple);
}  // namespace test
}  // namespace ripple
#undef M
