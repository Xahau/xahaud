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

#include <ripple/protocol/Feature.h>
#include <ripple/protocol/PayChan.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <vector>

namespace ripple {
namespace test {

struct SetHookTSH_test : public beast::unit_test::suite
{
private:

    const uint64_t tshSTRONG = 0;
    const uint64_t tshWEAK = 1;
    const uint64_t tshNONE = 2;

    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }
    void static collectFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE | hsfCOLLECT;
    }

    const std::vector<uint8_t> TshHook = {
        0x00U, 0x61U, 0x73U, 0x6dU, 0x01U, 0x00U, 0x00U, 0x00U, 0x01U, 0x1cU,
        0x04U, 0x60U, 0x05U, 0x7fU, 0x7fU, 0x7fU, 0x7fU, 0x7fU, 0x01U, 0x7eU,
        0x60U, 0x03U, 0x7fU, 0x7fU, 0x7eU, 0x01U, 0x7eU, 0x60U, 0x02U, 0x7fU,
        0x7fU, 0x01U, 0x7fU, 0x60U, 0x01U, 0x7fU, 0x01U, 0x7eU, 0x02U, 0x23U,
        0x03U, 0x03U, 0x65U, 0x6eU, 0x76U, 0x05U, 0x74U, 0x72U, 0x61U, 0x63U,
        0x65U, 0x00U, 0x00U, 0x03U, 0x65U, 0x6eU, 0x76U, 0x06U, 0x61U, 0x63U,
        0x63U, 0x65U, 0x70U, 0x74U, 0x00U, 0x01U, 0x03U, 0x65U, 0x6eU, 0x76U,
        0x02U, 0x5fU, 0x67U, 0x00U, 0x02U, 0x03U, 0x02U, 0x01U, 0x03U, 0x05U,
        0x03U, 0x01U, 0x00U, 0x02U, 0x06U, 0x2bU, 0x07U, 0x7fU, 0x01U, 0x41U,
        0xc0U, 0x8bU, 0x04U, 0x0bU, 0x7fU, 0x00U, 0x41U, 0x80U, 0x08U, 0x0bU,
        0x7fU, 0x00U, 0x41U, 0xb8U, 0x0bU, 0x0bU, 0x7fU, 0x00U, 0x41U, 0x80U,
        0x08U, 0x0bU, 0x7fU, 0x00U, 0x41U, 0xc0U, 0x8bU, 0x04U, 0x0bU, 0x7fU,
        0x00U, 0x41U, 0x00U, 0x0bU, 0x7fU, 0x00U, 0x41U, 0x01U, 0x0bU, 0x07U,
        0x08U, 0x01U, 0x04U, 0x68U, 0x6fU, 0x6fU, 0x6bU, 0x00U, 0x03U, 0x0aU,
        0xf3U, 0x81U, 0x00U, 0x01U, 0xefU, 0x81U, 0x00U, 0x01U, 0x01U, 0x7fU,
        0x23U, 0x00U, 0x41U, 0x10U, 0x6bU, 0x22U, 0x01U, 0x24U, 0x00U, 0x20U,
        0x01U, 0x20U, 0x00U, 0x36U, 0x02U, 0x0cU, 0x41U, 0x9aU, 0x0bU, 0x41U,
        0x0fU, 0x41U, 0xbdU, 0x09U, 0x41U, 0x0eU, 0x41U, 0x00U, 0x10U, 0x00U,
        0x1aU, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x02U, 0x40U, 0x20U,
        0x01U, 0x28U, 0x02U, 0x0cU, 0x0eU, 0x03U, 0x00U, 0x01U, 0x02U, 0x03U,
        0x0bU, 0x41U, 0xd9U, 0x0aU, 0x41U, 0xc0U, 0x00U, 0x41U, 0xfeU, 0x08U,
        0x41U, 0x3fU, 0x41U, 0x00U, 0x10U, 0x00U, 0x1aU, 0x0cU, 0x02U, 0x0bU,
        0x41U, 0x9bU, 0x0aU, 0x41U, 0x3dU, 0x41U, 0xc2U, 0x08U, 0x41U, 0x3cU,
        0x41U, 0x00U, 0x10U, 0x00U, 0x1aU, 0x0cU, 0x01U, 0x0bU, 0x41U, 0xd7U,
        0x09U, 0x41U, 0xc3U, 0x00U, 0x41U, 0x80U, 0x08U, 0x41U, 0xc2U, 0x00U,
        0x41U, 0x00U, 0x10U, 0x00U, 0x1aU, 0x0bU, 0x41U, 0xaaU, 0x0bU, 0x41U,
        0x0dU, 0x41U, 0xcbU, 0x09U, 0x41U, 0x0cU, 0x41U, 0x00U, 0x10U, 0x00U,
        0x1aU, 0x20U, 0x01U, 0x20U, 0x01U, 0x41U, 0x08U, 0x6aU, 0x22U, 0x00U,
        0x36U, 0x02U, 0x04U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0x18U, 0x88U, 0x42U, 0xffU, 0x01U, 0x83U,
        0x3cU, 0x00U, 0x00U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0x10U, 0x88U, 0x42U, 0xffU, 0x01U, 0x83U,
        0x3cU, 0x00U, 0x01U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0x08U, 0x88U, 0x42U, 0xffU, 0x01U, 0x83U,
        0x3cU, 0x00U, 0x02U, 0x20U, 0x01U, 0x28U, 0x02U, 0x04U, 0x20U, 0x01U,
        0x35U, 0x02U, 0x0cU, 0x42U, 0xffU, 0x01U, 0x83U, 0x3cU, 0x00U, 0x03U,
        0x20U, 0x00U, 0x41U, 0x04U, 0x42U, 0x18U, 0x10U, 0x01U, 0x1aU, 0x41U,
        0x01U, 0x41U, 0x01U, 0x10U, 0x02U, 0x1aU, 0x20U, 0x01U, 0x41U, 0x10U,
        0x6aU, 0x24U, 0x00U, 0x42U, 0x00U, 0x0bU, 0x0bU, 0xbfU, 0x03U, 0x01U,
        0x00U, 0x41U, 0x80U, 0x08U, 0x0bU, 0xb7U, 0x03U, 0x74U, 0x73U, 0x68U,
        0x2eU, 0x63U, 0x3aU, 0x20U, 0x57U, 0x65U, 0x61U, 0x6bU, 0x20U, 0x41U,
        0x67U, 0x61U, 0x69U, 0x6eU, 0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U,
        0x75U, 0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U,
        0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU,
        0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U,
        0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U,
        0x65U, 0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x57U, 0x65U, 0x61U, 0x6bU, 0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U,
        0x75U, 0x74U, 0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U,
        0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU,
        0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U,
        0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U,
        0x65U, 0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x53U, 0x74U, 0x72U, 0x6fU, 0x6eU, 0x67U, 0x2eU, 0x20U, 0x45U, 0x78U,
        0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x42U, 0x45U, 0x46U, 0x4fU,
        0x52U, 0x45U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U,
        0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U,
        0x70U, 0x6cU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU,
        0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x00U, 0x74U, 0x73U, 0x68U, 0x2eU,
        0x63U, 0x3aU, 0x20U, 0x53U, 0x74U, 0x61U, 0x72U, 0x74U, 0x2eU, 0x00U,
        0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U, 0x45U, 0x6eU, 0x64U,
        0x2eU, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x57U, 0x65U, 0x61U, 0x6bU, 0x20U, 0x41U, 0x67U, 0x61U, 0x69U, 0x6eU,
        0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U,
        0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6eU,
        0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U, 0x69U, 0x73U,
        0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U,
        0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x22U, 0x00U,
        0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U, 0x57U, 0x65U,
        0x61U, 0x6bU, 0x2eU, 0x20U, 0x45U, 0x78U, 0x65U, 0x63U, 0x75U, 0x74U,
        0x65U, 0x20U, 0x41U, 0x46U, 0x54U, 0x45U, 0x52U, 0x20U, 0x74U, 0x72U,
        0x61U, 0x6eU, 0x73U, 0x61U, 0x63U, 0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U,
        0x69U, 0x73U, 0x20U, 0x61U, 0x70U, 0x70U, 0x6cU, 0x69U, 0x65U, 0x64U,
        0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU, 0x65U, 0x64U, 0x67U, 0x65U, 0x72U,
        0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U,
        0x53U, 0x74U, 0x72U, 0x6fU, 0x6eU, 0x67U, 0x2eU, 0x20U, 0x45U, 0x78U,
        0x65U, 0x63U, 0x75U, 0x74U, 0x65U, 0x20U, 0x42U, 0x45U, 0x46U, 0x4fU,
        0x52U, 0x45U, 0x20U, 0x74U, 0x72U, 0x61U, 0x6eU, 0x73U, 0x61U, 0x63U,
        0x74U, 0x69U, 0x6fU, 0x6eU, 0x20U, 0x69U, 0x73U, 0x20U, 0x61U, 0x70U,
        0x70U, 0x6cU, 0x69U, 0x65U, 0x64U, 0x20U, 0x74U, 0x6fU, 0x20U, 0x6cU,
        0x65U, 0x64U, 0x67U, 0x65U, 0x72U, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U,
        0x68U, 0x2eU, 0x63U, 0x3aU, 0x20U, 0x53U, 0x74U, 0x61U, 0x72U, 0x74U,
        0x2eU, 0x22U, 0x00U, 0x22U, 0x74U, 0x73U, 0x68U, 0x2eU, 0x63U, 0x3aU,
        0x20U, 0x45U, 0x6eU, 0x64U, 0x2eU, 0x22U};

    void
    addWeakTSH(jtx::Env& env, jtx::Account const& account)
    {
        using namespace test::jtx;
        env(fset(account, asfTshCollect));
        env.close();
    }

    void
    setTSHHook(
        jtx::Env& env,
        jtx::Account const& account,
        bool const& testStrong)
    {
        using namespace test::jtx;
        auto const tshFlag = testStrong ? overrideFlag : collectFlag;
        env(hook(account, {{hso(TshHook, tshFlag)}}, 0),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();
    }

    void
    validateTSHStrongWeak(
        Json::Value meta,
        uint64_t const& expected,
        uint64_t const& lineno)
    {
        switch (expected)
        {
            // tshSTRONG
            case 0: {
                auto const executions = meta[sfHookExecutions.jsonName];
                auto const execution = executions[0u][sfHookExecution.jsonName];
                BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
                BEAST_EXPECT(
                    execution[sfHookReturnString.jsonName] == "00000000");
                if (execution[sfHookReturnString.jsonName] != "00000000")
                {
                    std::cout << "testTSHStrongWeak Line: " << lineno << "\n";
                    std::cout
                        << "testTSHStrongWeak Expected: " << expected
                        << " Result: " << execution[sfHookReturnString.jsonName]
                        << "\n";
                    std::cout << "testTSHStrongWeak Meta: " << meta << "\n";
                }
                break;
            }
            // tshWEAK
            case 1: {
                auto const executions = meta[sfHookExecutions.jsonName];
                auto const execution = executions[0u][sfHookExecution.jsonName];
                BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
                BEAST_EXPECT(
                    execution[sfHookReturnString.jsonName] == "00000001");
                if (execution[sfHookReturnString.jsonName] != "00000001")
                {
                    std::cout << "testTSHStrongWeak Line: " << lineno << "\n";
                    std::cout
                        << "testTSHStrongWeak Expected: " << expected
                        << " Result: " << execution[sfHookReturnString.jsonName]
                        << "\n";
                    std::cout << "testTSHStrongWeak Meta: " << meta << "\n";
                }
                break;
            }
            // tshNONE
            case 2: {
                auto const executions = meta[sfHookExecutions.jsonName];
                BEAST_EXPECT(executions.size() == 0);
                if (executions.size() != 0)
                {
                    std::cout << "testTSHStrongWeak Line: " << lineno << "\n";
                    std::cout << "testTSHStrongWeak Expected: " << expected
                              << " Result: " << executions.size() << "\n";
                    std::cout << "testTSHStrongWeak Meta: " << meta << "\n";
                }
                break;
            }

            default:
                break;
        }
    }

    void
    testTSHStrongWeak(
        jtx::Env& env,
        int const& expected,
        uint64_t const& lineno)
    {
        Json::Value params;
        params[jss::transaction] =
            env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        auto const meta = jrr[jss::result][jss::meta];
        validateTSHStrongWeak(meta, expected, lineno);
    }

    // AccountSet
    // | otxn | tsh | set |
    // |   A  |  A  |  S  |
    void
    testAccountSetTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("account set TSH");

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // account set
            env(fset(account, asfDefaultRipple), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // AccountDelete
    // | otxn | tsh |  delete  |
    // |   A  |  A  |   N/A    |
    // |   A  |  B  |    S     |
    // Account cannot delete with a hook installed
    void
    testAccountDeleteTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("account delete TSH");

        // otxn: account
        // tsh bene
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, bene);

            // set tsh hook
            setTSHHook(env, bene, testStrong);

            // AccountDelete
            incLgrSeqForAccDel(env, account);
            env(acctdelete(account, bene), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // Check
    // | otxn | tsh | cancel |  create  | cash  |
    // |   A  |  A  |   S    |    S     |  N/A  |
    // |   A  |  D  |   N    |    S     |  N/A  |
    // |   D  |  D  |   S    |   N/A    |   S   |
    // |   D  |  A  |   S    |   N/A    |   S   |
    static uint256
    getCheckIndex(AccountID const& account, std::uint32_t uSequence)
    {
        return keylet::check(account, uSequence).key;
    }

    void
    testCheckCancelTSH(FeatureBitset features)
    {
        testcase("check cancel tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh destination
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testCheckCreateTSH(FeatureBitset features)
    {
        testcase("check create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create check
            env(check::create(account, dest, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh destination
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // create check
            env(check::create(account, dest, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testCheckCashTSH(FeatureBitset features)
    {
        testcase("check cash tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cash check
            env(check::cash(dest, checkId, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cash check
            env(check::cash(dest, checkId, XRP(100)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // ClaimReward
    // | otxn | tsh | claim |
    // |   A  |  A  |   S   |
    // |   A  |  I  |   S   |

    void
    testClaimRewardTSH(FeatureBitset features)
    {
        testcase("claim reward tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // claim reward
            env(reward::claim(account),
                reward::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // claim reward
            env(reward::claim(account),
                reward::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // DepositPreauth
    // | otxn  | tsh | preauth |
    // |   A   |  A  |    S    |
    // |   A   |  Au |    S    |

    void
    testDepositPreauthTSH(FeatureBitset features)
    {
        testcase("deposit preauth tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const authed = Account("alice");
            auto const account = Account("bob");
            env.fund(XRP(1000), authed, account);
            env.close();

            // require authorization for deposits.
            env(fset(account, asfDepositAuth));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh authorize
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const authed = Account("alice");
            auto const account = Account("bob");
            env.fund(XRP(1000), authed, account);
            env.close();

            // require authorization for deposits.
            env(fset(account, asfDepositAuth));

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, authed);

            // set tsh hook
            setTSHHook(env, authed, testStrong);

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // Escrow
    // | otxn  | tsh | cancel | cancel(id) |  create  | finish | finish(id)
    // |   A   |  A  |    S   |     S      |     S    |    S   |     S
    // |   A   |  D  |    N   |     N      |     S    |    S   |     S
    // |   D   |  D  |    S   |     S      |    N/A   |    S   |     S
    // |   D   |  A  |    S   |     S      |    N/A   |    S   |     S

    static uint256
    getEscrowIndex(AccountID const& account, std::uint32_t uSequence)
    {
        return keylet::escrow(account, uSequence).key;
    }

    void
    testEscrowCancelTSH(FeatureBitset features)
    {
        testcase("escrow cancel tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            env(escrow::cancel(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            env(escrow::cancel(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            env(escrow::cancel(dest, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            env(escrow::cancel(dest, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testEscrowIDCancelTSH(FeatureBitset features)
    {
        testcase("escrow id cancel tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = escrow::cancel(account, account, 0);
            }
            else
            {
                tx = escrow::cancel(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = escrow::cancel(account, account, 0);
            }
            else
            {
                tx = escrow::cancel(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // cancel escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = escrow::cancel(dest, account, 0);
            }
            else
            {
                tx = escrow::cancel(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = escrow::cancel(dest, account, 0);
            }
            else
            {
                tx = escrow::cancel(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    void
    testEscrowCreateTSH(FeatureBitset features)
    {
        testcase("escrow create tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // create escrow
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testEscrowFinishTSH(FeatureBitset features)
    {
        testcase("escrow finish tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            env(escrow::finish(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            env(escrow::finish(account, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            env(escrow::finish(dest, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            env(escrow::finish(dest, account, seq1),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testEscrowIDFinishTSH(FeatureBitset features)
    {
        testcase("escrow id finish tsh");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = escrow::finish(account, account, 0);
            }
            else
            {
                tx = escrow::finish(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = escrow::finish(account, account, 0);
            }
            else
            {
                tx = escrow::finish(account, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // finish escrow
            Json::Value tx;
            if (!env.current()->rules().enabled(fixXahauV1))
            {
                tx = escrow::finish(dest, account, 0);
            }
            else
            {
                tx = escrow::finish(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // finish escrow
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            Json::Value tx;
            if (!fixV1)
            {
                tx = escrow::finish(dest, account, 0);
            }
            else
            {
                tx = escrow::finish(dest, account);
            }
            env(tx, escrow::escrow_id(escrowId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected =
                (fixV1 ? (testStrong ? tshSTRONG : tshSTRONG)
                       : (testStrong ? tshNONE : tshNONE));
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // GenesisMint
    // | otxn  | tsh |  mint |
    // |   A   |  A  |   S   |
    // |   A   |  D  |   S   |
    // |   A   |  B  |   W   |

    void
    testGenesisMintTSH(FeatureBitset features)
    {
        testcase("genesis mint tsh");

        // trigger the emitted txn
        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = env.master;
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(issuer), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set mint hook on master
            env(hook(issuer, {{hso(genesis::MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    genesis::makeBlob({
                        {bene.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = env.master;
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(issuer), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook && set mint hook on master
            env(hook(
                    issuer,
                    {{hso(TshHook, overrideFlag),
                      hso(genesis::MintTestHook, overrideFlag)}},
                    0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    genesis::makeBlob({
                        {bene.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh bene
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = env.master;
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(issuer), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, bene);

            // set tsh hook
            setTSHHook(env, bene, testStrong);

            // set mint hook on master
            env(hook(issuer, {{hso(genesis::MintTestHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(invoke::invoke(
                    account,
                    issuer,
                    genesis::makeBlob({
                        {bene.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const emissions = meta[sfHookEmissions.jsonName];
            auto const emission = emissions[0u][sfHookEmission.jsonName];
            auto const txId = emission[sfEmittedTxnID.jsonName];
            env.close();

            // verify tsh hook triggered
            Json::Value params1;
            params1[jss::transaction] = txId;
            auto const jrr1 = env.rpc("json", "tx", to_string(params1));
            auto const meta1 = jrr1[jss::result][jss::meta];
            auto const expected = testStrong ? tshNONE : tshWEAK;
            validateTSHStrongWeak(meta1, expected, __LINE__);
        }
    }

    // Import
    // | otxn  | tsh | import |
    // |   A   |  A  |   S    |
    // |   A   |  I  |   S    |
    // * Issuer cannot import on itself

    void
    testImportTSH(FeatureBitset features)
    {
        testcase("import tsh");

        using namespace test::jtx;
        using namespace std::literals;

        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC"
            "1"};

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, keys, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(env.master), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // import
            env(import::import(
                    account, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkVLConfig(
                    21337, keys, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("bob");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // burn down the total ledger coins so that genesis mints don't mint
            // above 100B tripping invariant
            env(noop(env.master), fee(XRP(10'000'000ULL)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // import
            env(import::import(
                    account, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // Invoke
    // | otxn  | tsh | invoke |
    // |   A   |  A  |    S   |
    // |   A   |  D  |    S   |

    void
    testInvokeTSH(FeatureBitset features)
    {
        testcase("invoke tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // ttINVOKE
            env(invoke::invoke(account),
                invoke::dest(dest),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // ttINVOKE
            env(invoke::invoke(account),
                invoke::dest(dest),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // Offer
    // | otxn  | tsh |  cancel | create |
    // |   A   |  A  |    S    |    S   |
    // |   A   |  C  |   N/A   |    N   |

    void
    testOfferCancelTSH(FeatureBitset features)
    {
        testcase("offer cancel tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, gw);
            env.close();

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // create offer
            auto const offerSeq = env.seq(account);
            env(offer(account, USD(1000), XRP(1000)), ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // cancel offer
            env(offer_cancel(account, offerSeq), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testOfferCreateTSH(FeatureBitset features)
    {
        testcase("offer create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, gw);
            env.close();

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh cross
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const cross = Account("bob");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, cross, gw);
            env.close();

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, cross);

            // set tsh hook
            setTSHHook(env, cross, testStrong);

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    // Payment
    // | otxn  | tsh | payment |
    // |   A   |  A  |    S    |
    // |   A   |  D  |    S    |
    // |   A   |  C  |    W    |

    void
    testPaymentTSH(FeatureBitset features)
    {
        testcase("payment tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh cross
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const cross = Account("bob");
            auto const dest = Account("carol");
            auto const gw = Account{"gateway"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), account, cross, dest, gw);
            env.close();

            // setup rippling
            auto const USDA = account["USD"];
            auto const USDB = cross["USD"];
            auto const USDC = dest["USD"];
            env.trust(USDA(10), cross);
            env.trust(USDB(10), dest);

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, cross);

            // set tsh hook
            setTSHHook(env, cross, testStrong);

            // payment
            env(pay(account, dest, USDB(10)), paths(USDA), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // PaymentChannel
    // | otxn  | tsh | claim |  create  |  fund  |
    // |   A   |  A  |   S   |    S     |    S   |
    // |   A   |  D  |   W   |    S     |    W   |
    // |   D   |  D  |   S   |   N/A    |   N/A  |
    // |   D   |  A  |   S   |   N/A    |   N/A  |

    static uint256
    channel(
        jtx::Account const& account,
        jtx::Account const& dst,
        std::uint32_t seqProxyValue)
    {
        auto const k = keylet::payChan(account, dst, seqProxyValue);
        return k.key;
    }

    static Buffer
    signClaimAuth(
        PublicKey const& pk,
        SecretKey const& sk,
        uint256 const& channel,
        STAmount const& authAmt)
    {
        Serializer msg;
        serializePayChanAuthorization(msg, channel, authAmt.xrp());
        return sign(pk, sk, msg.slice());
    }

    void
    testPaymentChannelClaimTSH(FeatureBitset features)
    {
        testcase("payment channel claim tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(account.pk(), account.sk(), chan, authAmt);
            env(paychan::claim(
                    dest, chan, reqBal, authAmt, Slice(sig), account.pk()),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: dest
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(account.pk(), account.sk(), chan, authAmt);
            env(paychan::claim(
                    dest, chan, reqBal, authAmt, Slice(sig), account.pk()),
                txflags(tfClose),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testPaymentChannelCreateTSH(FeatureBitset features)
    {
        testcase("payment channel create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testPaymentChannelFundTSH(FeatureBitset features)
    {
        testcase("payment channel fund tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // fund paychannel
            env(paychan::fund(account, chan, XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // fund paychannel
            env(paychan::fund(account, chan, XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // SetHook
    // | otxn  | tsh | set |
    // |   A   |  A  |  S  |
    void
    testSetHookTSH(FeatureBitset features)
    {
        testcase("set hook tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            auto hook1 = hso(TshHook, overrideFlag);
            hook1[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000004000"
                "00";
            env(hook(account, {{hook1}}, 0), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // SetRegularKey
    // | otxn  | tsh | srk |
    // |   A   |  A  |  S  |
    // |   A   |  D  |  S  |
    void
    testSetRegularKeyTSH(FeatureBitset features)
    {
        testcase("set regular key tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // set regular key
            env(regkey(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, dest);

            // set tsh hook
            setTSHHook(env, dest, testStrong);

            // set regular key
            env(regkey(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // SignersListSet
    // | otxn  | tsh | sls |
    // |   A   |  A  |  S  |
    // |   A   |  S  |  S  |
    void
    testSignersListSetTSH(FeatureBitset features)
    {
        testcase("signers list set tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const signer1 = Account{"bob"};
            auto const signer2 = Account{"carol"};
            env.fund(XRP(1000), account, signer1, signer2);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // signers list set
            env(signers(account, 2, {{signer1, 1}, {signer2, 1}}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh signer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const signer1 = Account{"bob"};
            auto const signer2 = Account{"carol"};
            env.fund(XRP(1000), account, signer1, signer2);
            env.close();

            // set tsh hook
            env(hook(signer1, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, signer2);

            // set tsh hook
            setTSHHook(env, signer2, testStrong);

            // signers list set
            env(signers(account, 2, {{signer1, 1}, {signer2, 1}}),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
            auto const execution2 = executions[1u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution2[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution2[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Ticket
    // | otxn  | tsh | create |
    // |   A   |  A  |   S    |
    void
    testTicketCreateTSH(FeatureBitset features)
    {
        testcase("ticket create tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // ticket create
            env(ticket::create(account, 2), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    // TrustSet
    // | otxn  | tsh | trustset |
    // |   A   |  A  |     S    |
    // |   A   |  I  |     W    |
    void
    testTrustSetTSH(FeatureBitset features)
    {
        testcase("trust set tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, account);

            // set tsh hook
            setTSHHook(env, account, testStrong);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account{"gw"};
            auto const USD = issuer["USD"];
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }
    }

    // | otxn | tfBurnable | tsh |   mint |  burn  |  buy  |  sell  | cancel
    // |   O  |    false   |  O  |   N/A  |   S    |  N/A  |   S    |   S
    // |   O  |    false   |  I  |   N/A  |   N    |  N/A  |   W    |   N/A
    // |   O  |    false   |  B  |   N/A  |   N/A  |  N/A  |   N    |   N/A
    // |   O  |    true    |  B  |   N/A  |   N/A  |  N/A  |   N    |   N/A
    // |   O  |    true    |  O  |   N/A  |   S    |  N/A  |   S    |   S
    // |   O  |    true    |  I  |   N/A  |   N    |  N/A  |   S    |   N/A
    // |   I  |    false   |  O  |   N/A  |   N/A  |  N/A  |   N/A  |   N/A
    // |   I  |    false   |  I  |   S    |   N/A  |  N/A  |   N/A  |   N/A
    // |   I  |    false   |  B  |   N    |   N/A  |  N/A  |   N/A  |   N/A
    // |   I  |    true    |  O  |   N/A  |   N    |  N/A  |   N/A  |   N/A
    // |   I  |    true    |  I  |   S    |   S    |  N/A  |   N/A  |   N/A
    // |   I  |    true    |  B  |   N    |   N/A  |  N/A  |   N/A  |   N/A
    // |   B  |    true    |  O  |   N/A  |   N/A  |  S    |   N/A  |   N/A
    // |   B  |    true    |  B  |   N/A  |   N/A  |  S    |   N/A  |   N/A
    // |   B  |    false   |  I  |   N/A  |   N/A  |  W    |   N/A  |   N/A
    // |   B  |    true    |  I  |   N/A  |   N/A  |  S    |   N/A  |   N/A

    void
    testURITokenMintTSH(FeatureBitset features)
    {
        testcase("uritoken mint tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: issuer
        // flag: not burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: issuer
        // flag: not burnable
        // tsh buyer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh buyer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }
    }

    void
    testURITokenBurnTSH(FeatureBitset features)
    {
        testcase("uritoken burn tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            auto const expected =
                (fixV1 ? (testStrong ? tshNONE : tshNONE)
                       : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            auto const expected =
                (fixV1 ? (testStrong ? tshNONE : tshNONE)
                       : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh owner
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            bool const fixV1 = env.current()->rules().enabled(fixXahauV1);
            auto const expected =
                (fixV1 ? (testStrong ? tshNONE : tshNONE)
                       : (testStrong ? tshSTRONG : tshSTRONG));
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testURITokenBuyTSH(FeatureBitset features)
    {
        testcase("uritoken buy tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: buyer
        // flag: not burnable
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: buyer
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: buyer
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: buyer
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testURITokenCancelSellOfferTSH(FeatureBitset features)
    {
        testcase("uritoken cancel sell offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh buyer
        // w/s: none
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshNONE, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testURITokenCreateSellOfferTSH(FeatureBitset features)
    {
        testcase("uritoken create sell offer tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            auto const expected = testStrong ? tshNONE : tshWEAK;
            testTSHStrongWeak(env, expected, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, buyer);

            // set tsh hook
            setTSHHook(env, buyer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, owner);

            // set tsh hook
            setTSHHook(env, owner, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
        for (bool const testStrong : {true, false})
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, owner, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set tsh collect
            if (!testStrong)
                addWeakTSH(env, issuer);

            // set tsh hook
            setTSHHook(env, issuer, testStrong);

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            testTSHStrongWeak(env, tshSTRONG, __LINE__);
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testAccountSetTSH(features);
        testAccountDeleteTSH(features);
        testCheckCancelTSH(features);
        testCheckCashTSH(features);
        testCheckCreateTSH(features);
        testClaimRewardTSH(features);
        testDepositPreauthTSH(features);
        testEscrowCancelTSH(features);
        testEscrowIDCancelTSH(features);
        testEscrowCreateTSH(features);
        testEscrowFinishTSH(features);
        testEscrowIDFinishTSH(features);
        testGenesisMintTSH(features);
        testImportTSH(features);
        testInvokeTSH(features);
        testOfferCancelTSH(features);
        testOfferCreateTSH(features);
        testPaymentTSH(features);
        testPaymentChannelClaimTSH(features);
        testPaymentChannelCreateTSH(features);
        testPaymentChannelFundTSH(features);
        testSetHookTSH(features);
        testSetRegularKeyTSH(features);
        testSignersListSetTSH(features);
        testTicketCreateTSH(features);
        testTrustSetTSH(features);
        testURITokenMintTSH(features);
        testURITokenBurnTSH(features);
        testURITokenBuyTSH(features);
        testURITokenCancelSellOfferTSH(features);
        testURITokenCreateSellOfferTSH(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa - fixXahauV1);
        testWithFeats(sa);
    }
};

BEAST_DEFINE_TESTSUITE(SetHookTSH, app, ripple);

}  // namespace test
}  // namespace ripple