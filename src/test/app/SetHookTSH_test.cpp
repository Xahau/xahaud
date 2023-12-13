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

    // AccountSet
    // | otxn | tsh | set |
    // |   A  |  A  |  S  |
    void
    testAccountSetTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh hook on bob
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // account set
            env(fset(account, asfTshCollect), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // AccountDelete
    // | otxn | tsh | delete |
    // |   A  |  A  |    N   |
    // |   A  |  B  |    S   |
    void
    testAccountDeleteTSH(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh bene
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const bene = Account("bob");
            env.fund(XRP(1000), account, bene);
            env.close();

            // set tsh hook
            env(hook(bene, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // AccountDelete
            incLgrSeqForAccDel(env, account);
            env(acctdelete(account, bene), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Check
    // | otxn | tsh | cancel | create | cash |
    // |   A  |  A  |   S    |   S    |  N   |
    // |   A  |  D  |   N    |   S    |  N   |
    // |   D  |  D  |   S    |   N    |  S   |
    // |   D  |  A  |   S    |   N    |  S   |
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh destination
        // w/s: none
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
            env(fset(dest, asfTshCollect));
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(account, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: strong
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel check
            env(check::cancel(dest, checkId), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cash check
            env(check::cash(dest, checkId, XRP(100)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: strong
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
            env(fset(account, asfTshCollect));
            env.close();

            // create check
            uint256 const checkId{getCheckIndex(account, env.seq(account))};
            env(check::create(account, dest, XRP(100)), ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cash check
            env(check::cash(dest, checkId, XRP(100)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create check
            env(check::create(account, dest, XRP(100)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh destination
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create check
            env(check::create(account, dest, XRP(100)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // claim reward
            env(reward::claim(account),
                reward::issuer(issuer),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const issuer = Account("issuer");
            env.fund(XRP(1000), account, issuer);
            env.close();

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // claim reward
            env(reward::claim(account),
                reward::issuer(issuer),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh authorize
        // w/s: strong
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

            // set tsh hook
            env(hook(authed, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // deposit preauth
            env(deposit::auth(account, authed), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Escrow
    // | otxn  | tsh | cancel | create | finish |
    // |   A   |  A  |    S   |    S   |    S   |
    // |   A   |  D  |    W   |    S   |    W   |
    // |   D   |  D  |    S   |    N   |    S   |
    // |   D   |  A  |    S   |    N   |    S   |

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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrow::cancel(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
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
            env(fset(dest, asfTshCollect));
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrow::cancel(account, account, seq1),
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
            BEAST_EXPECT(executions.size() == 0);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrow::cancel(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: strong
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
            env(fset(account, asfTshCollect));
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrow::cancel(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
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
            env(fset(dest, asfTshCollect));
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
            // auto const execution = executions[0u][sfHookExecution.jsonName];
            // BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            // BEAST_EXPECT(execution[sfHookReturnString.jsonName] ==
            // "00000001");
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: weak
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
            env(fset(account, asfTshCollect));
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrow::finish(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
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
            // env(fset(dest, asfTshCollect));
            // env.close();

            // create escrow
            auto const seq1 = env.seq(account);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrow::finish(account, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrow::finish(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: strong
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrow::finish(dest, account, seq1),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
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
            env(fset(dest, asfTshCollect));
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: weak
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
            env(fset(account, asfTshCollect));
            env.close();

            // create escrow
            uint256 const escrowId{getEscrowIndex(account, env.seq(account))};
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(account, dest, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
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

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh bene
        // w/s: weak
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
            env(fset(bene, asfTshCollect));
            env.close();

            // set tsh hook
            env(hook(bene, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            auto const executions1 = meta1[sfHookExecutions.jsonName];
            auto const execution1 = executions1[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000001");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // import
            env(import::import(
                    account, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // import
            env(import::import(
                    account, import::loadXpop(ImportTCAccountSet::w_seed)),
                import::issuer(issuer),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution1 = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution1[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution1[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Invoke
    // | otxn  | tsh | invoke |
    // |   A   |  A  |    S   |
    // |   A   |  D  |    W   |

    void
    testInvokeTSH(FeatureBitset features)
    {
        testcase("invoke tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttINVOKE
            env(invoke::invoke(account),
                invoke::dest(dest),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account("bob");
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttINVOKE
            env(invoke::invoke(account),
                invoke::dest(dest),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    // Offer
    // | otxn  | tsh | cancel | create |
    // |   A   |  A  |    S   |    S   |
    // |   A   |  C  |    N  |     N   |

    void
    testOfferCancelTSH(FeatureBitset features)
    {
        testcase("offer cancel tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: account
        // tsh account
        // w/s: strong
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel offer
            env(offer_cancel(account, offerSeq), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh cross
        // w/s: weak
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

            // set tsh collect
            env(fset(cross, asfTshCollect));

            // gw create offer
            env(offer(gw, USD(1000), XRP(1000)));
            env.close();

            // set tsh hook
            env(hook(cross, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create offer
            env(offer(account, USD(1000), XRP(1000)),
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
            BEAST_EXPECT(executions.size() == 0);
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(account, dest, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh cross
        // w/s: weak
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
            env(fset(cross, asfTshCollect));

            // set tsh hook
            env(hook(cross, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(account, dest, USDB(10)), paths(USDA), fee(XRP(1)));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }
    }

    // PaymentChannel
    // | otxn  | tsh | claim | create | fund |
    // |   A   |  A  |   S   |   S    |   S  |
    // |   A   |  D  |   W   |   S    |   W  |
    // |   D   |  D  |   S   |   N    |   N  |
    // |   D   |  A  |   S   |   N    |   N  |

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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
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
            env(fset(dest, asfTshCollect));

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(account, chan, reqBal, authAmt),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: dest
        // tsh dest
        // w/s: strong
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

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(account.pk(), account.sk(), chan, authAmt);
            env(paychan::claim(
                    dest, chan, reqBal, authAmt, Slice(sig), account.pk()),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: dest
        // tsh account
        // w/s: strong
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(account.pk(), account.sk(), chan, authAmt);
            env(paychan::claim(
                    dest, chan, reqBal, authAmt, Slice(sig), account.pk()),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: strong
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // fund paychannel
            env(paychan::fund(account, chan, XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
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
            env(fset(dest, asfTshCollect));

            // create paychannel
            auto const pk = account.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(account, dest, env.seq(account));
            env(paychan::create(account, dest, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // fund paychannel
            env(paychan::fund(account, chan, XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh hook
            auto hook1 = hso(TshHook, overrideFlag);
            hook1[jss::HookOn] =
                "00000000000000000000000000000000000000000000000000000000004000"
                "00";
            env(hook(account, {{hook1}}, 0), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // set hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set regular key
            env(regkey(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh dest
        // w/s: weak
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            auto const dest = Account{"bob"};
            env.fund(XRP(1000), account, dest);
            env.close();

            // set tsh hook
            env(hook(dest, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // set regular key
            env(regkey(account, dest), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh signer
        // w/s: strong
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

            // set tsh hook
            env(hook(signer2, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

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
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const account = Account("alice");
            env.fund(XRP(1000), account);
            env.close();

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ticket create
            env(ticket::create(account, 2), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(account, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: account
        // tsh issuer
        // w/s: weak
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

            // set tsh collect on bob
            env(fset(issuer, asfTshCollect));

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // trust set
            env(trust(account, USD(1000)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }
    }

    // | otxn | tfBurnable | tsh | mint | burn | buy | sell | cancel
    // |   O  |    false   |  O  |   N  |   S  |  S  |   S  |   S
    // |   O  |    false   |  I  |   N  |   W  |  W  |   W  |   N
    // |   O  |    false   |  B  |   N  |   N  |  N  |   S  |   N
    // |   O  |    true    |  B  |   N  |   N  |  N  |   S  |   N
    // |   O  |    true    |  O  |   N  |   S  |  S  |   S  |   S
    // |   O  |    true    |  I  |   N  |   W  |  S  |   S  |   N
    // |   I  |    false   |  O  |   N  |   N  |  N  |   N  |   N
    // |   I  |    false   |  I  |   S  |   N  |  N  |   N  |   N
    // |   I  |    false   |  B  |   N  |   N  |  N  |   N  |   N
    // |   I  |    true    |  O  |   N  |   W  |  N  |   N  |   N
    // |   I  |    true    |  I  |   S  |   S  |  N  |   N  |   N
    // |   I  |    true    |  B  |   N  |   N  |  N  |   N  |   N
    // |   B  |    true    |  O  |   N  |   N  |  ?  |   N  |   N
    // |   B  |    true    |  B  |   N  |   N  |  ?  |   N  |   N

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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

    void
    testURITokenBuyTSH(FeatureBitset features)
    {
        testcase("uritoken buy tsh");

        using namespace test::jtx;
        using namespace std::literals;

        // otxn: owner
        // flag: not burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: buyer
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: buyer
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(buyer, hexid),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: none
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

            env(fset(buyer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
        }

        // otxn: owner
        // flag: burnable
        // tsh buyer
        // w/s: none
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

            env(fset(buyer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // cancel uritoken
            env(uritoken::cancel(owner, hexid), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh issuer
        // w/s: weak
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

            env(fset(issuer, asfTshCollect));
            env.close();

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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000001");
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: not burnable
        // tsh buyer
        // w/s: strong
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

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh owner
        // w/s: strong
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

            // set tsh hook
            env(hook(owner, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: owner
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // sell uritoken
            env(uritoken::sell(owner, hexid),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }
    }

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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: not burnable
        // tsh buyer
        // w/s: none
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            env(fset(buyer, asfTshCollect));
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
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
            BEAST_EXPECT(executions.size() == 0);
        }

        // otxn: issuer
        // flag: burnable
        // tsh issuer
        // w/s: strong
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

            // set tsh hook
            env(hook(issuer, {{hso(TshHook, overrideFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
            BEAST_EXPECT(execution[sfHookReturnString.jsonName] == "00000000");
        }

        // otxn: issuer
        // flag: burnable
        // tsh buyer
        // w/s: none
        {
            test::jtx::Env env{
                *this,
                network::makeNetworkConfig(21337, "10", "1000000", "200000"),
                features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            env(fset(buyer, asfTshCollect));
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // set tsh hook
            env(hook(buyer, {{hso(TshHook, collectFlag)}}, 0),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                fee(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // verify tsh hook triggered
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const executions = meta[sfHookExecutions.jsonName];
            BEAST_EXPECT(executions.size() == 0);
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
        testURITokenBurnTSH(features);
        testURITokenBuyTSH(features);
        testURITokenCancelSellOfferTSH(features);
        testURITokenCreateSellOfferTSH(features);
        testURITokenMintTSH(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
        testWithFeats(sa - fixXahauV1);
    }
};

BEAST_DEFINE_TESTSUITE(SetHookTSH, app, ripple);

}  // namespace test
}  // namespace ripple