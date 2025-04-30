//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL-Labs

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

#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class AccountNamespace_test : public beast::unit_test::suite
{
public:
    void
    testErrors(FeatureBitset features)
    {
        testcase("error cases");

        using namespace jtx;
        Env env(*this);

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const ns = uint256::fromVoid(
            (std::array<uint8_t, 32>{
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU})
                .data());

        {
            // account_namespace with no account.
            auto const info = env.rpc("json", "account_namespace", "{ }");
            BEAST_EXPECT(
                info[jss::result][jss::error_message] ==
                "Missing field 'account'.");
        }
        {
            // account_namespace missing filed namespace_id.
            Json::Value params;
            params[jss::account] = alice.human();
            auto const info =
                env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                info[jss::result][jss::error_message] ==
                "Missing field 'namespace_id'.");
        }
        {
            // account_namespace with a malformed account string.
            Json::Value params;
            params[jss::account] =
                "n94JNrQYkDrpt62bbSR7nVEhdyAvcJXRAsjEkFYyqRkh9SUTYEqV";
            params[jss::namespace_id] = "";
            auto const info =
                env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                info[jss::result][jss::error_message] == "Disallowed seed.");
        }
        {
            // account_namespace with an invalid namespace_id.
            Json::Value params;
            params[jss::account] = alice.human();
            params[jss::namespace_id] = "DEADBEEF";
            auto const info =
                env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                info[jss::result][jss::error_message] == "Invalid parameters.");
        }
        {
            // account_namespace with an account that's not in the ledger.
            Json::Value params;
            params[jss::account] = carol.human();
            params[jss::namespace_id] = to_string(ns);
            auto const info =
                env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                info[jss::result][jss::error_message] == "Account not found.");
        }
        {
            // account_namespace with a namespace that doesnt exist.
            Json::Value params;
            params[jss::account] = alice.human();
            params[jss::namespace_id] = to_string(ns);
            auto const info =
                env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                info[jss::result][jss::error_message] ==
                "Namespace not found.");
        }
        // test errors on marker
        {
            auto const ns = uint256::fromVoid(
                (std::array<uint8_t, 32>{
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU,
                     0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU, 0xCAU, 0xFEU})
                    .data());

            // Lambda to create a hook.
            auto setHook = [](test::jtx::Account const& account) {
                std::string const createCodeHex =
                    "0061736D01000000011B0460027F7F017F60047F7F7F7F017E60037F7F"
                    "7E01"
                    "7E60017F017E02270303656E76025F67000003656E760973746174655F"
                    "7365"
                    "74000103656E76066163636570740002030201030503010002062B077F"
                    "0141"
                    "9088040B7F004180080B7F00418A080B7F004180080B7F00419088040B"
                    "7F00"
                    "41000B7F0041010B07080104686F6F6B00030AE7800001E3800002017F"
                    "017E"
                    "230041106B220124002001200036020C41012200200010001A20014180"
                    "0828"
                    "0000360208200141046A410022002F0088083B01002001200028008408"
                    "3602"
                    "004100200020014106200141086A4104100110022102200141106A2400"
                    "2002"
                    "0B0B1001004180080B096B65790076616C7565";
                std::string ns_str =
                    "CAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECAFECA"
                    "FECA"
                    "FE";
                Json::Value jv =
                    ripple::test::jtx::hook(account, {{hso(createCodeHex)}}, 0);
                jv[jss::Hooks][0U][jss::Hook][jss::HookNamespace] = ns_str;
                return jv;
            };

            env(setHook(bob), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            env(pay(alice, bob, XRP(1)), fee(XRP(1)), ter(tesSUCCESS));
            env.close();

            Json::Value params;
            params[jss::account] = bob.human();
            params[jss::namespace_id] = to_string(ns);
            params[jss::limit] = 1;
            auto resp = env.rpc("json", "account_namespace", to_string(params));

            auto resume_marker = resp[jss::result][jss::marker];
            std::string mark = to_string(resume_marker);
            params[jss::marker] = 10;
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker', not string.");

            params[jss::marker] = "This is a string with no comma";
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker'.");

            params[jss::marker] = "This string has a comma, but is not hex";
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker'.");

            params[jss::marker] = std::string(&mark[1U], 64);
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker'.");

            params[jss::marker] = std::string(&mark[1U], 65);
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker'.");

            params[jss::marker] = std::string(&mark[1U], 65) + "not hex";
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker'.");

            params[jss::marker] = std::string(&mark[1U], 128);
            resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'marker'.");
        }

        // test error on limit -ve
        {
            Account const bob{"bob"};
            Json::Value params;
            params[jss::account] = bob.human();
            params[jss::namespace_id] = to_string(ns);
            params[jss::limit] = -1;
            auto resp = env.rpc("json", "account_namespace", to_string(params));
            BEAST_EXPECT(
                resp[jss::result][jss::error_message] ==
                "Invalid field 'limit', not unsigned integer.");
        }
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testErrors(all);
    }
};

BEAST_DEFINE_TESTSUITE(AccountNamespace, rpc, ripple);

}  // namespace test
}  // namespace ripple
