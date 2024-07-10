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

#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {
struct SetHookDefinition_test : public beast::unit_test::suite
{

    Json::Value
    setDefinition(jtx::Account const& account, std::string const& hex)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::SetHookDefinition;
        jv[jss::Account] = account.human();
        jv[jss::CreateCode] = hex;
        jv[sfHookNamespace.jsonName] = "0000000000000000000000000000000000000000000000000000000000000000";
        jv[sfHookOn.jsonName] = "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFBFFFFFFFFFFFFFFFFFFBFFFFF";
        jv[sfHookApiVersion.jsonName] = 0;
        jv[sfFlags.jsonName] = hsfMUTALBE;
        return jv;
    }

    void
    testEnabled(FeatureBitset features)
    {
        using namespace test::jtx;

        testcase("Enabled");
        Account const alice = Account("alice");

        for (bool const withFeature : {false, true})
        {
            auto const amend = withFeature ? features : features - featureHooksUpdate2;
            Env env{*this, amend};

            env.fund(XRP(1000), alice);
            env.close();

            std::string const createCodeHex =
                "0061736D01000000011C0460057F7F7F7F7F017E60037F7F7E017E60027F7F"
                "017F60017F017E02250303656E76057472616365000003656E7608726F6C6C"
                "6261636B000103656E76025F670002030201030503010002062B077F0141C0"
                "88040B7F004180080B7F0041BE080B7F004180080B7F0041C088040B7F0041"
                "000B7F0041010B07080104686F6F6B00030AC3800001BF800001017F230041"
                "106B220124002001200036020C41940841154180084114410010001A41AA08"
                "4114420A10011A41012200200010021A200141106A240042000B0B44010041"
                "80080B3D526F6C6C6261636B2E633A2043616C6C65642E0022526F6C6C6261"
                "636B2E633A2043616C6C65642E2200526F6C6C6261636B3A20526F6C6C6261"
                "636B21";

            // strHex(wasmBytes)
            auto const txResult = withFeature ? ter(tesSUCCESS) : ter(temDISABLED);
            env(setDefinition(alice, createCodeHex), fee(10), txResult);
            env.close();

            // Json::Value jvParams;
            // jvParams[jss::index] = "ledgerHash";
            // Json::Value const jrr = env.rpc("json", "ledger_entry", to_string(jvParams))[jss::result];
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
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

BEAST_DEFINE_TESTSUITE(SetHookDefinition, app, ripple);
}  // namespace test
}  // namespace ripple
