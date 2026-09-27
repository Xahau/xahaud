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

#include <test/jtx.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/jss.h>

#include <test/jtx/WSClient.h>
#include <test/rpc/GRPCTestClientBase.h>
#include <xrpld/rpc/GRPCHandlers.h>
#include <xrpl/hook/Misc.h>
#include <xrpl/protocol/InnerObjectFormats.h>
#include <xrpl/resource/Charge.h>
#include <xrpl/resource/Fees.h>

namespace ripple {
namespace test {

class AccountInfo_test : public beast::unit_test::suite
{
public:
    void
    testErrors()
    {
        testcase("Errors");
        using namespace jtx;
        Env env(*this);
        {
            // account_info with no account.
            auto const info = env.rpc("json", "account_info", "{ }");
            BEAST_EXPECT(
                info[jss::result][jss::error_message] ==
                "Missing field 'account'.");
        }
        {
            // account_info with a malformed account string.
            auto const info = env.rpc(
                "json",
                "account_info",
                "{\"account\": "
                "\"n94JNrQYkDrpt62bbSR7nVEhdyAvcJXRAsjEkFYyqRkh9SUTYEqV\"}");
            BEAST_EXPECT(
                info[jss::result][jss::error_code] == rpcACT_MALFORMED);
            BEAST_EXPECT(
                info[jss::result][jss::error_message] == "Account malformed.");
        }
        {
            // account_info with an account that's not in the ledger.
            Account const bogie{"bogie"};
            auto const info = env.rpc(
                "json",
                "account_info",
                R"({ "account": ")" + bogie.human() + R"("})");
            BEAST_EXPECT(
                info[jss::result][jss::error_code] == rpcACT_NOT_FOUND);
            BEAST_EXPECT(
                info[jss::result][jss::error_message] == "Account not found.");
        }
        {
            // Cannot use a seed as account
            auto const info =
                env.rpc("json", "account_info", R"({"account": "foo"})");
            BEAST_EXPECT(
                info[jss::result][jss::error_code] == rpcACT_MALFORMED);
            BEAST_EXPECT(
                info[jss::result][jss::error_message] == "Account malformed.");
        }
        {
            // Cannot pass a non-string into the `account` param

            auto testInvalidAccountParam = [&](auto const& param) {
                Json::Value params;
                params[jss::account] = param;
                auto jrr = env.rpc(
                    "json", "account_info", to_string(params))[jss::result];
                BEAST_EXPECT(jrr[jss::error] == "invalidParams");
                BEAST_EXPECT(
                    jrr[jss::error_message] == "Invalid field 'account'.");
            };

            testInvalidAccountParam(1);
            testInvalidAccountParam(1.1);
            testInvalidAccountParam(true);
            testInvalidAccountParam(Json::Value(Json::nullValue));
            testInvalidAccountParam(Json::Value(Json::objectValue));
            testInvalidAccountParam(Json::Value(Json::arrayValue));
        }
        {
            // Cannot pass a non-string into the `ident` param

            auto testInvalidIdentParam = [&](auto const& param) {
                Json::Value params;
                params[jss::ident] = param;
                auto jrr = env.rpc(
                    "json", "account_info", to_string(params))[jss::result];
                BEAST_EXPECT(jrr[jss::error] == "invalidParams");
                BEAST_EXPECT(
                    jrr[jss::error_message] == "Invalid field 'ident'.");
            };

            testInvalidIdentParam(1);
            testInvalidIdentParam(1.1);
            testInvalidIdentParam(true);
            testInvalidIdentParam(Json::Value(Json::nullValue));
            testInvalidIdentParam(Json::Value(Json::objectValue));
            testInvalidIdentParam(Json::Value(Json::arrayValue));
        }
    }

    void
    testHooksRequest()
    {
        testcase("Hooks request");
        using namespace jtx;
        Env env(*this);
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);

        auto request = [&](Json::Value const& hooks, unsigned apiVersion) {
            Json::Value params;
            params[jss::account] = alice.human();
            params[jss::hooks] = hooks;
            if (apiVersion == 1)
                return env.rpc("json", "account_info", to_string(params));

            params[jss::api_version] = 2;
            return env.rpc("json", "account_info", to_string(params));
        };

        for (Json::Value const& invalid :
             {Json::Value(1),
              Json::Value(1.1),
              Json::Value("true"),
              Json::Value(Json::nullValue),
              Json::Value(Json::objectValue),
              Json::Value(Json::arrayValue)})
        {
            for (unsigned const apiVersion : {1u, 2u})
            {
                auto const info = request(invalid, apiVersion);
                BEAST_EXPECT(info[jss::result][jss::error] == "invalidParams");
                BEAST_EXPECT(
                    info[jss::result][jss::error_message] ==
                    "Invalid field 'hooks'.");
            }
        }

        auto const withoutHooks = env.rpc(
            "json",
            "account_info",
            R"({"account": ")" + alice.human() + R"("})");
        BEAST_EXPECT(!withoutHooks[jss::result].isMember(jss::hooks));

        auto const explicitlyFalse = env.rpc(
            "json",
            "account_info",
            R"({"account": ")" + alice.human() + R"(","hooks":false})");
        BEAST_EXPECT(!explicitlyFalse[jss::result].isMember(jss::hooks));

        auto const explicitlyTrue = env.rpc(
            "json",
            "account_info",
            std::string{"{ "} + "\"account\": \"" + alice.human() +
                "\", \"hooks\": true }");
        BEAST_EXPECT(explicitlyTrue[jss::result][jss::hooks].isArray());
        BEAST_EXPECT(explicitlyTrue[jss::result][jss::hooks].size() == 0);
    }

    void
    testHooksEffectiveValues()
    {
        testcase("Effective hook values");

        auto const* hookTemplate =
            InnerObjectFormats::getInstance().findSOTemplateBySField(sfHook);
        if (!BEAST_EXPECTS(hookTemplate != nullptr, "sfHook template"))
            return;
        // If this count changes, add or remove tests for the corresponding Hook
        // fields.
        BEAST_EXPECT(hookTemplate->size() == 12);

        using namespace jtx;
        Env env(*this);
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);

        uint256 const definitionNamespace{1};
        uint256 const accountNamespace{2};
        auto const makeParameter = [](std::string const& name,
                                      std::string const& value) {
            Json::Value parameter;
            parameter[jss::HookParameter][jss::HookParameterName] = name;
            parameter[jss::HookParameter][jss::HookParameterValue] = value;
            return parameter;
        };

        auto create = hso(genesis::AcceptHook);
        create[jss::Flags] = hsfCOLLECT;
        create[jss::HookName] = strHex(std::string{"ACCT"});
        create[jss::HookNamespace] = to_string(definitionNamespace);
        create[jss::HookParameters] = Json::arrayValue;
        create[jss::HookParameters].append(makeParameter("AA", "BB"));
        create[jss::HookParameters].append(makeParameter("CC", "DD"));
        env(hook(alice, {{create, Json::Value{}}}, 0),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        auto const hookSLE = env.le(keylet::hook(alice.id()));
        BEAST_EXPECT(hookSLE && hookSLE->isFieldPresent(sfHooks));
        if (!hookSLE || !hookSLE->isFieldPresent(sfHooks))
            return;
        auto const& installed = hookSLE->getFieldArray(sfHooks)[0];
        BEAST_EXPECT(installed.isFieldPresent(sfHookHash));
        if (!installed.isFieldPresent(sfHookHash))
            return;
        auto const hash = installed.getFieldH256(sfHookHash);

        auto const hookDefinition = env.le(keylet::hookDefinition(hash));
        BEAST_EXPECT(hookDefinition);
        if (hookDefinition)
        {
            BEAST_EXPECT(!hookDefinition->isFieldPresent(sfHookName));
            BEAST_EXPECT(!hookDefinition->isFieldPresent(sfHookGrants));
            BEAST_EXPECT(hookDefinition->isFieldPresent(sfHookApiVersion));
            BEAST_EXPECT(hookDefinition->getFieldU16(sfHookApiVersion) == 0);
        }

        auto accountInfo = env.rpc(
            "json",
            "account_info",
            R"({"account": ")" + alice.human() + R"(","hooks":true})");
        auto const& hookJson =
            accountInfo[jss::result][jss::hooks][0U][sfHook.jsonName];
        BEAST_EXPECT(hookJson[sfHookHash.jsonName] == to_string(hash));
        BEAST_EXPECT(!hookJson.isMember(sfCreateCode.jsonName));
        BEAST_EXPECT(
            hookJson[sfHookNamespace.jsonName] ==
            to_string(definitionNamespace));
        BEAST_EXPECT(
            hookJson[sfHookName.jsonName].isString() &&
            hookJson[sfHookName.jsonName] == strHex(std::string{"ACCT"}));
        BEAST_EXPECT(
            hookJson[sfHookApiVersion.jsonName].isInt() &&
            hookJson[sfHookApiVersion.jsonName].asUInt() == 0);
        BEAST_EXPECT(hookJson[sfHookOn.jsonName] == to_string(uint256{0}));
        BEAST_EXPECT(!hookJson.isMember(sfHookOnIncoming.jsonName));
        BEAST_EXPECT(!hookJson.isMember(sfHookOnOutgoing.jsonName));
        BEAST_EXPECT(
            hookJson[sfHookCanEmit.jsonName] ==
            to_string(UINT256_BIT[ttHOOK_SET]));
        BEAST_EXPECT(hookJson[sfFlags.jsonName] == hsfCOLLECT);
        BEAST_EXPECT(hookJson[sfHookParameters.jsonName].size() == 2);
        auto const& hooksJson = accountInfo[jss::result][jss::hooks];
        BEAST_EXPECT(hooksJson.isArray());
        BEAST_EXPECT(hooksJson.size() == 2);
        if (!hooksJson.isArray() || hooksJson.size() < 2)
            return;
        BEAST_EXPECT(hooksJson[1U].isObject());
        BEAST_EXPECT(hooksJson[1U].isMember(sfHook.jsonName));
        if (!hooksJson[1U].isObject() ||
            !hooksJson[1U].isMember(sfHook.jsonName))
            return;
        auto const& blankHook = hooksJson[1U][sfHook.jsonName];
        BEAST_EXPECT(blankHook.isObject());
        BEAST_EXPECT(blankHook.size() == 0);

        Json::Value update;
        update[jss::HookHash] = to_string(hash);
        update[jss::Flags] = hsfOVERRIDE;
        update[jss::HookName] = strHex(std::string{"ACCT"});
        update[jss::HookGrants] = Json::arrayValue;
        update[jss::HookGrants][0U][jss::HookGrant][jss::HookHash] =
            to_string(hash);
        update[jss::HookNamespace] = to_string(accountNamespace);
        update[jss::HookOnIncoming] = to_string(uint256{5});
        update[jss::HookOnOutgoing] = to_string(uint256{6});
        update[jss::HookCanEmit] = to_string(uint256{4});
        update[jss::HookParameters] = Json::arrayValue;
        update[jss::HookParameters].append(makeParameter("AA", ""));
        update[jss::HookParameters].append(makeParameter("EE", "FF"));
        env(hook(alice, {{update}}, 0), fee(XRP(1)), ter(tesSUCCESS));
        env.close();

        accountInfo = env.rpc(
            "json",
            "account_info",
            R"({"account": ")" + alice.human() + R"(","hooks":true})");
        auto const& overridden =
            accountInfo[jss::result][jss::hooks][0U][sfHook.jsonName];
        BEAST_EXPECT(
            overridden[sfHookNamespace.jsonName] ==
            to_string(accountNamespace));
        BEAST_EXPECT(
            overridden[sfHookName.jsonName].isString() &&
            overridden[sfHookName.jsonName] == strHex(std::string{"ACCT"}));
        BEAST_EXPECT(
            overridden[sfHookApiVersion.jsonName].isInt() &&
            overridden[sfHookApiVersion.jsonName].asUInt() == 0);
        BEAST_EXPECT(overridden[sfHookGrants.jsonName].isArray());
        BEAST_EXPECT(overridden[sfHookGrants.jsonName].size() == 1);
        if (overridden[sfHookGrants.jsonName].isArray() &&
            overridden[sfHookGrants.jsonName].size() == 1)
        {
            auto const& grant = overridden[sfHookGrants.jsonName][0U];
            BEAST_EXPECT(grant.isObject());
            BEAST_EXPECT(grant.size() == 1);
            BEAST_EXPECT(grant.isMember(sfHookGrant.jsonName));
            if (grant.isObject() && grant.size() == 1 &&
                grant.isMember(sfHookGrant.jsonName))
            {
                auto const& grantObject = grant[sfHookGrant.jsonName];
                BEAST_EXPECT(grantObject.isObject());
                BEAST_EXPECT(grantObject.size() == 1);
                BEAST_EXPECT(grantObject.isMember(sfHookHash.jsonName));
                if (grantObject.isObject() && grantObject.size() == 1 &&
                    grantObject.isMember(sfHookHash.jsonName))
                    BEAST_EXPECT(
                        grantObject[sfHookHash.jsonName] == to_string(hash));
            }
        }
        BEAST_EXPECT(overridden[sfFlags.jsonName] == 0);
        BEAST_EXPECT(!overridden.isMember(sfHookOn.jsonName));
        BEAST_EXPECT(
            overridden[sfHookOnIncoming.jsonName] == to_string(uint256{5}));
        BEAST_EXPECT(
            overridden[sfHookOnOutgoing.jsonName] == to_string(uint256{6}));
        BEAST_EXPECT(
            overridden[sfHookCanEmit.jsonName] == to_string(uint256{4}));
        BEAST_EXPECT(overridden[sfHookParameters.jsonName].size() == 3);
        if (overridden[sfHookParameters.jsonName].size() != 3)
            return;
        auto expectParameter = [&](std::string const& name,
                                   std::string const& value) {
            bool found = false;
            for (auto const& parameter : overridden[sfHookParameters.jsonName])
            {
                if (!parameter.isObject() ||
                    !parameter.isMember(sfHookParameter.jsonName))
                    continue;
                auto const& object = parameter[sfHookParameter.jsonName];
                if (!object.isObject() ||
                    object[sfHookParameterName.jsonName] != name)
                    continue;
                found = true;
                BEAST_EXPECT(object[sfHookParameterValue.jsonName] == value);
                break;
            }
            BEAST_EXPECT(found);
        };
        expectParameter("AA", "");
        expectParameter("CC", "DD");
        expectParameter("EE", "FF");
    }

    // Test the "signer_lists" argument in account_info.
    void
    testSignerLists()
    {
        testcase("Signer lists");
        using namespace jtx;
        Env env(*this);
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);

        auto const withoutSigners =
            std::string("{ ") + "\"account\": \"" + alice.human() + "\"}";

        auto const withSigners = std::string("{ ") + "\"account\": \"" +
            alice.human() + "\", " + "\"signer_lists\": true }";

        // Alice has no SignerList yet.
        {
            // account_info without the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withoutSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            BEAST_EXPECT(!info[jss::result][jss::account_data].isMember(
                jss::signer_lists));
        }
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            auto const& data = info[jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 0);
        }

        // Give alice a SignerList.
        Account const bogie{"bogie"};

        Json::Value const smallSigners = signers(alice, 2, {{bogie, 3}});
        env(smallSigners);
        {
            // account_info without the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withoutSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            BEAST_EXPECT(!info[jss::result][jss::account_data].isMember(
                jss::signer_lists));
        }
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            auto const& data = info[jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 1);
            auto const& signers = signerLists[0u];
            BEAST_EXPECT(signers.isObject());
            BEAST_EXPECT(signers[sfSignerQuorum.jsonName] == 2);
            auto const& signerEntries = signers[sfSignerEntries.jsonName];
            BEAST_EXPECT(signerEntries.size() == 1);
            auto const& entry0 = signerEntries[0u][sfSignerEntry.jsonName];
            BEAST_EXPECT(entry0[sfSignerWeight.jsonName] == 3);
        }

        // Give alice a big signer list
        Account const demon{"demon"};
        Account const ghost{"ghost"};
        Account const haunt{"haunt"};
        Account const jinni{"jinni"};
        Account const phase{"phase"};
        Account const shade{"shade"};
        Account const spook{"spook"};

        Json::Value const bigSigners = signers(
            alice,
            4,
            {
                {bogie, 1},
                {demon, 1},
                {ghost, 1},
                {haunt, 1},
                {jinni, 1},
                {phase, 1},
                {shade, 1},
                {spook, 1},
            });
        env(bigSigners);
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            auto const& data = info[jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 1);
            auto const& signers = signerLists[0u];
            BEAST_EXPECT(signers.isObject());
            BEAST_EXPECT(signers[sfSignerQuorum.jsonName] == 4);
            auto const& signerEntries = signers[sfSignerEntries.jsonName];
            BEAST_EXPECT(signerEntries.size() == 8);
            for (unsigned i = 0u; i < 8; ++i)
            {
                auto const& entry = signerEntries[i][sfSignerEntry.jsonName];
                BEAST_EXPECT(entry.size() == 2);
                BEAST_EXPECT(entry.isMember(sfAccount.jsonName));
                BEAST_EXPECT(entry[sfSignerWeight.jsonName] == 1);
            }
        }
    }

    // Test the "signer_lists" argument in account_info, with api_version 2.
    void
    testSignerListsApiVersion2()
    {
        testcase("Signer lists APIv2");
        using namespace jtx;
        Env env{*this};
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);

        auto const withoutSigners = std::string("{ ") +
            "\"api_version\": 2, \"account\": \"" + alice.human() + "\"}";

        auto const withSigners = std::string("{ ") +
            "\"api_version\": 2, \"account\": \"" + alice.human() + "\", " +
            "\"signer_lists\": true }";

        auto const withSignersAsString = std::string("{ ") +
            "\"api_version\": 2, \"account\": \"" + alice.human() + "\", " +
            "\"signer_lists\": asdfggh }";

        // Alice has no SignerList yet.
        {
            // account_info without the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withoutSigners);
            BEAST_EXPECT(info.isMember(jss::result));
            BEAST_EXPECT(!info[jss::result].isMember(jss::signer_lists));
        }
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withSigners);
            BEAST_EXPECT(info.isMember(jss::result));
            auto const& data = info[jss::result];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 0);
        }

        // Give alice a SignerList.
        Account const bogie{"bogie"};

        Json::Value const smallSigners = signers(alice, 2, {{bogie, 3}});
        env(smallSigners);
        {
            // account_info without the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withoutSigners);
            BEAST_EXPECT(info.isMember(jss::result));
            BEAST_EXPECT(!info[jss::result].isMember(jss::signer_lists));
        }
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withSigners);
            BEAST_EXPECT(info.isMember(jss::result));
            auto const& data = info[jss::result];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 1);
            auto const& signers = signerLists[0u];
            BEAST_EXPECT(signers.isObject());
            BEAST_EXPECT(signers[sfSignerQuorum.jsonName] == 2);
            auto const& signerEntries = signers[sfSignerEntries.jsonName];
            BEAST_EXPECT(signerEntries.size() == 1);
            auto const& entry0 = signerEntries[0u][sfSignerEntry.jsonName];
            BEAST_EXPECT(entry0[sfSignerWeight.jsonName] == 3);
        }
        {
            // account_info with "signer_lists" as not bool should error out
            auto const info =
                env.rpc("json", "account_info", withSignersAsString);
            BEAST_EXPECT(info[jss::status] == "error");
            BEAST_EXPECT(info[jss::error] == "invalidParams");
        }

        // Give alice a big signer list
        Account const demon{"demon"};
        Account const ghost{"ghost"};
        Account const haunt{"haunt"};
        Account const jinni{"jinni"};
        Account const phase{"phase"};
        Account const shade{"shade"};
        Account const spook{"spook"};

        Json::Value const bigSigners = signers(
            alice,
            4,
            {
                {bogie, 1},
                {demon, 1},
                {ghost, 1},
                {haunt, 1},
                {jinni, 1},
                {phase, 1},
                {shade, 1},
                {spook, 1},
            });
        env(bigSigners);
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json", "account_info", withSigners);
            BEAST_EXPECT(info.isMember(jss::result));
            auto const& data = info[jss::result];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 1);
            auto const& signers = signerLists[0u];
            BEAST_EXPECT(signers.isObject());
            BEAST_EXPECT(signers[sfSignerQuorum.jsonName] == 4);
            auto const& signerEntries = signers[sfSignerEntries.jsonName];
            BEAST_EXPECT(signerEntries.size() == 8);
            for (unsigned i = 0u; i < 8; ++i)
            {
                auto const& entry = signerEntries[i][sfSignerEntry.jsonName];
                BEAST_EXPECT(entry.size() == 2);
                BEAST_EXPECT(entry.isMember(sfAccount.jsonName));
                BEAST_EXPECT(entry[sfSignerWeight.jsonName] == 1);
            }
        }
    }

    // Test the "signer_lists" argument in account_info, version 2 API.
    void
    testSignerListsV2()
    {
        testcase("Signer lists v2");
        using namespace jtx;
        Env env(*this);
        Account const alice{"alice"};
        env.fund(XRP(1000), alice);

        auto const withoutSigners = std::string("{ ") +
            "\"jsonrpc\": \"2.0\", "
            "\"ripplerpc\": \"2.0\", "
            "\"id\": 5, "
            "\"method\": \"account_info\", "
            "\"params\": { "
            "\"account\": \"" +
            alice.human() + "\"}}";

        auto const withSigners = std::string("{ ") +
            "\"jsonrpc\": \"2.0\", "
            "\"ripplerpc\": \"2.0\", "
            "\"id\": 6, "
            "\"method\": \"account_info\", "
            "\"params\": { "
            "\"account\": \"" +
            alice.human() + "\", " + "\"signer_lists\": true }}";
        // Alice has no SignerList yet.
        {
            // account_info without the "signer_lists" argument.
            auto const info = env.rpc("json2", withoutSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            BEAST_EXPECT(!info[jss::result][jss::account_data].isMember(
                jss::signer_lists));
            BEAST_EXPECT(
                info.isMember(jss::jsonrpc) && info[jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info.isMember(jss::ripplerpc) && info[jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info.isMember(jss::id) && info[jss::id] == 5);
        }
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json2", withSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            auto const& data = info[jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 0);
            BEAST_EXPECT(
                info.isMember(jss::jsonrpc) && info[jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info.isMember(jss::ripplerpc) && info[jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info.isMember(jss::id) && info[jss::id] == 6);
        }
        {
            // Do both of the above as a batch job
            auto const info = env.rpc(
                "json2", '[' + withoutSigners + ", " + withSigners + ']');
            BEAST_EXPECT(
                info[0u].isMember(jss::result) &&
                info[0u][jss::result].isMember(jss::account_data));
            BEAST_EXPECT(!info[0u][jss::result][jss::account_data].isMember(
                jss::signer_lists));
            BEAST_EXPECT(
                info[0u].isMember(jss::jsonrpc) &&
                info[0u][jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info[0u].isMember(jss::ripplerpc) &&
                info[0u][jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info[0u].isMember(jss::id) && info[0u][jss::id] == 5);

            BEAST_EXPECT(
                info[1u].isMember(jss::result) &&
                info[1u][jss::result].isMember(jss::account_data));
            auto const& data = info[1u][jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 0);
            BEAST_EXPECT(
                info[1u].isMember(jss::jsonrpc) &&
                info[1u][jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info[1u].isMember(jss::ripplerpc) &&
                info[1u][jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info[1u].isMember(jss::id) && info[1u][jss::id] == 6);
        }

        // Give alice a SignerList.
        Account const bogie{"bogie"};

        Json::Value const smallSigners = signers(alice, 2, {{bogie, 3}});
        env(smallSigners);
        {
            // account_info without the "signer_lists" argument.
            auto const info = env.rpc("json2", withoutSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            BEAST_EXPECT(!info[jss::result][jss::account_data].isMember(
                jss::signer_lists));
            BEAST_EXPECT(
                info.isMember(jss::jsonrpc) && info[jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info.isMember(jss::ripplerpc) && info[jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info.isMember(jss::id) && info[jss::id] == 5);
        }
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json2", withSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            auto const& data = info[jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 1);
            auto const& signers = signerLists[0u];
            BEAST_EXPECT(signers.isObject());
            BEAST_EXPECT(signers[sfSignerQuorum.jsonName] == 2);
            auto const& signerEntries = signers[sfSignerEntries.jsonName];
            BEAST_EXPECT(signerEntries.size() == 1);
            auto const& entry0 = signerEntries[0u][sfSignerEntry.jsonName];
            BEAST_EXPECT(entry0[sfSignerWeight.jsonName] == 3);
            BEAST_EXPECT(
                info.isMember(jss::jsonrpc) && info[jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info.isMember(jss::ripplerpc) && info[jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info.isMember(jss::id) && info[jss::id] == 6);
        }

        // Give alice a big signer list
        Account const demon{"demon"};
        Account const ghost{"ghost"};
        Account const haunt{"haunt"};
        Account const jinni{"jinni"};
        Account const phase{"phase"};
        Account const shade{"shade"};
        Account const spook{"spook"};

        Json::Value const bigSigners = signers(
            alice,
            4,
            {
                {bogie, 1},
                {demon, 1},
                {ghost, 1},
                {haunt, 1},
                {jinni, 1},
                {phase, 1},
                {shade, 1},
                {spook, 1},
            });
        env(bigSigners);
        {
            // account_info with the "signer_lists" argument.
            auto const info = env.rpc("json2", withSigners);
            BEAST_EXPECT(
                info.isMember(jss::result) &&
                info[jss::result].isMember(jss::account_data));
            auto const& data = info[jss::result][jss::account_data];
            BEAST_EXPECT(data.isMember(jss::signer_lists));
            auto const& signerLists = data[jss::signer_lists];
            BEAST_EXPECT(signerLists.isArray());
            BEAST_EXPECT(signerLists.size() == 1);
            auto const& signers = signerLists[0u];
            BEAST_EXPECT(signers.isObject());
            BEAST_EXPECT(signers[sfSignerQuorum.jsonName] == 4);
            auto const& signerEntries = signers[sfSignerEntries.jsonName];
            BEAST_EXPECT(signerEntries.size() == 8);
            for (unsigned i = 0u; i < 8; ++i)
            {
                auto const& entry = signerEntries[i][sfSignerEntry.jsonName];
                BEAST_EXPECT(entry.size() == 2);
                BEAST_EXPECT(entry.isMember(sfAccount.jsonName));
                BEAST_EXPECT(entry[sfSignerWeight.jsonName] == 1);
            }
            BEAST_EXPECT(
                info.isMember(jss::jsonrpc) && info[jss::jsonrpc] == "2.0");
            BEAST_EXPECT(
                info.isMember(jss::ripplerpc) && info[jss::ripplerpc] == "2.0");
            BEAST_EXPECT(info.isMember(jss::id) && info[jss::id] == 6);
        }
    }

    void
    testAccountFlags(FeatureBitset const& features)
    {
        testcase("Account flags");
        using namespace jtx;

        Env env(*this, features);
        Account const alice{"alice"};
        Account const bob{"bob"};
        env.fund(XRP(1000), alice, bob);

        auto getAccountFlag = [&env](
                                  std::string_view fName,
                                  Account const& account) {
            auto const info = env.rpc(
                "json",
                "account_info",
                R"({"account" : ")" + account.human() + R"("})");

            std::optional<bool> res;
            if (info[jss::result][jss::status] == "success" &&
                info[jss::result][jss::account_flags].isMember(fName.data()))
                res.emplace(info[jss::result][jss::account_flags][fName.data()]
                                .asBool());

            return res;
        };

        static constexpr std::
            array<std::pair<std::string_view, std::uint32_t>, 8>
                asFlags{
                    {{"defaultRipple", asfDefaultRipple},
                     {"depositAuth", asfDepositAuth},
                     {"disallowIncomingXRP", asfDisallowXRP},
                     {"globalFreeze", asfGlobalFreeze},
                     {"noFreeze", asfNoFreeze},
                     {"requireAuthorization", asfRequireAuth},
                     {"tshCollect", asfTshCollect},
                     {"requireDestinationTag", asfRequireDest}}};

        for (auto& asf : asFlags)
        {
            // Clear a flag and check that account_info returns results
            // as expected
            env(fclear(alice, asf.second));
            env.close();
            auto const f1 = getAccountFlag(asf.first, alice);
            BEAST_EXPECT(f1.has_value());
            BEAST_EXPECT(!f1.value());

            // Set a flag and check that account_info returns results
            // as expected
            env(fset(alice, asf.second));
            env.close();
            auto const f2 = getAccountFlag(asf.first, alice);
            BEAST_EXPECT(f2.has_value());
            BEAST_EXPECT(f2.value());
        }

        static constexpr std::
            array<std::pair<std::string_view, std::uint32_t>, 5>
                disallowIncomingFlags{
                    {{"disallowIncomingCheck", asfDisallowIncomingCheck},
                     {"disallowIncomingNFTokenOffer",
                      asfDisallowIncomingNFTokenOffer},
                     {"disallowIncomingPayChan", asfDisallowIncomingPayChan},
                     {"disallowIncomingTrustline",
                      asfDisallowIncomingTrustline},
                     {"disallowIncomingRemit", asfDisallowIncomingRemit}}};

        if (features[featureDisallowIncoming])
        {
            for (auto& asf : disallowIncomingFlags)
            {
                // Clear a flag and check that account_info returns results
                // as expected
                env(fclear(alice, asf.second));
                env.close();
                auto const f1 = getAccountFlag(asf.first, alice);
                BEAST_EXPECT(f1.has_value());
                BEAST_EXPECT(!f1.value());

                // Set a flag and check that account_info returns results
                // as expected
                env(fset(alice, asf.second));
                env.close();
                auto const f2 = getAccountFlag(asf.first, alice);
                BEAST_EXPECT(f2.has_value());
                BEAST_EXPECT(f2.value());
            }
        }
        else
        {
            for (auto& asf : disallowIncomingFlags)
            {
                BEAST_EXPECT(!getAccountFlag(asf.first, alice));
            }
        }

        static constexpr std::pair<std::string_view, std::uint32_t>
            allowTrustLineClawbackFlag{
                "allowTrustLineClawback", asfAllowTrustLineClawback};

        if (features[featureClawback])
        {
            // must use bob's account because alice has noFreeze set
            auto const f1 =
                getAccountFlag(allowTrustLineClawbackFlag.first, bob);
            BEAST_EXPECT(f1.has_value());
            BEAST_EXPECT(!f1.value());

            // Set allowTrustLineClawback
            env(fset(bob, allowTrustLineClawbackFlag.second));
            env.close();
            auto const f2 =
                getAccountFlag(allowTrustLineClawbackFlag.first, bob);
            BEAST_EXPECT(f2.has_value());
            BEAST_EXPECT(f2.value());
        }
        else
        {
            BEAST_EXPECT(
                !getAccountFlag(allowTrustLineClawbackFlag.first, bob));
        }
    }

    void
    run() override
    {
        testErrors();
        testHooksRequest();
        testHooksEffectiveValues();
        testSignerLists();
        testSignerListsApiVersion2();
        testSignerListsV2();

        FeatureBitset const allFeatures{
            ripple::test::jtx::supported_amendments()};
        testAccountFlags(allFeatures);
        testAccountFlags(allFeatures - featureDisallowIncoming);
        testAccountFlags(
            allFeatures - featureDisallowIncoming - featureClawback);
    }
};

BEAST_DEFINE_TESTSUITE(AccountInfo, rpc, ripple);

}  // namespace test
}  // namespace ripple
