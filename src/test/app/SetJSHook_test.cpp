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

namespace ripple {

namespace test {

#define DEBUG_TESTS 1

using TestHook = std::vector<uint8_t> const&;

// class JSSHasher
// {
// public:
//     size_t
//     operator()(const Json::StaticString& n) const
//     {
//         return std::hash<std::string_view>{}(n.c_str());
//     }
// };

// class JSSEq
// {
// public:
//     bool
//     operator()(const Json::StaticString& a, const Json::StaticString& b) const
//     {
//         return a == b;
//     }
// };

using JSSMap =
    std::unordered_map<Json::StaticString, Json::Value, JSSHasher, JSSEq>;

// Identical to BEAST_EXPECT except it returns from the function
// if the condition isn't met (and would otherwise therefore cause a crash)
#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }

#define HASH_WASM(x)                                                           \
    uint256 const x##_hash =                                                   \
        ripple::sha512Half_s(ripple::Slice(x##_wasm.data(), x##_wasm.size())); \
    std::string const x##_hash_str = to_string(x##_hash);                      \
    Keylet const x##_keylet = keylet::hookDefinition(x##_hash);

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

        // std::cout << "RESULT: " << accept_wasm.size() << "\n";

        // TestHook accept_wasm1 = {
        //     0x43U, 0x06U, 0x0cU, 0x61U, 0x63U, 0x63U, 0x65U, 0x70U, 0x74U,
        //     0x08U, 0x48U, 0x6fU, 0x6fU, 0x6bU, 0x28U, 0x77U, 0x61U, 0x73U,
        //     0x6dU, 0x6aU, 0x73U, 0x2fU, 0x74U, 0x65U, 0x73U, 0x74U, 0x2dU,
        //     0x30U, 0x2dU, 0x67U, 0x65U, 0x6eU, 0x2eU, 0x6aU, 0x73U, 0x06U,
        //     0x6dU, 0x73U, 0x67U, 0x08U, 0x63U, 0x6fU, 0x64U, 0x65U, 0x06U,
        //     0x61U, 0x72U, 0x67U, 0x0cU, 0x00U, 0x06U, 0x00U, 0xa2U, 0x01U,
        //     0x00U, 0x01U, 0x00U, 0x01U, 0x00U, 0x02U, 0x32U, 0x01U, 0xa4U,
        //     0x01U, 0x00U, 0x00U, 0x00U, 0x3fU, 0xe3U, 0x00U, 0x00U, 0x00U,
        //     0x80U, 0x3fU, 0xe4U, 0x00U, 0x00U, 0x00U, 0x80U, 0x3eU, 0xe3U,
        //     0x00U, 0x00U, 0x00U, 0x80U, 0x3eU, 0xe4U, 0x00U, 0x00U, 0x00U,
        //     0x80U, 0xc2U, 0x00U, 0x4dU, 0xe3U, 0x00U, 0x00U, 0x00U, 0x3aU,
        //     0xe3U, 0x00U, 0x00U, 0x00U, 0xc2U, 0x01U, 0x4dU, 0xe4U, 0x00U,
        //     0x00U, 0x00U, 0x3aU, 0xe4U, 0x00U, 0x00U, 0x00U, 0xc7U, 0x28U,
        //     0xcaU, 0x03U, 0x01U, 0x03U, 0x3dU, 0x40U, 0x41U, 0x0cU, 0x02U,
        //     0x06U, 0x00U, 0x00U, 0x02U, 0x00U, 0x02U, 0x00U, 0x00U, 0x00U,
        //     0x01U, 0x02U, 0xccU, 0x03U, 0x00U, 0x01U, 0x00U, 0xceU, 0x03U,
        //     0x00U, 0x01U, 0x00U, 0x29U, 0xcaU, 0x03U, 0x02U, 0x00U, 0x0cU,
        //     0x02U, 0x06U, 0x00U, 0x00U, 0x01U, 0x00U, 0x01U, 0x03U, 0x00U,
        //     0x01U, 0x0bU, 0x01U, 0xd0U, 0x03U, 0x00U, 0x01U, 0x00U, 0x38U,
        //     0xe3U, 0x00U, 0x00U, 0x00U, 0xc1U, 0x00U, 0xb7U, 0x23U, 0x02U,
        //     0x00U, 0xcaU, 0x03U, 0x03U, 0x01U, 0x03U, 0x07U, 0x02U, 0x30U};

        env(ripple::test::jtx::hook(
                alice, {{hso(accept_wasm, 1, overrideFlag)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        env(ripple::test::jtx::hook(
                alice, {{hso(accept_wasm, 1, overrideFlag)}}, 0),
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
    testWithFeatures(FeatureBitset features)
    {
        testHooksOwnerDir(features);
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
            const accept = (msg, code) => {}
            const Hook = (arg) => {
            return accept("0",0);
            }
        )[test.hook]"];

    HASH_WASM(accept);
};
BEAST_DEFINE_TESTSUITE(SetJSHook, app, ripple);
}  // namespace test
}  // namespace ripple
#undef M
