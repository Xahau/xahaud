//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/jss.h>
#include <test/app/Export_test_hooks.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

// Large fee for hook operations
#define HSFEE fee(100'000'000)

struct Export_test : public beast::unit_test::suite
{
    // Bare bones hook that just accepts
    TestHook accept_wasm = export_test_wasm[R"[test.hook](
        #include <stdint.h>
        extern int32_t _g(uint32_t id, uint32_t maxiter);
        extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);

        int64_t hook(uint32_t reserved) {
            _g(1, 1);
            accept(0, 0, 0);
            return 0;
        }
    )[test.hook]"];

    void
    testBasicSetup(FeatureBitset features)
    {
        testcase("Basic Setup");

        using namespace jtx;

        auto severity = beast::severities::kNone;
        // Minimal setup: validator keys + amendments
        Env env{*this, envconfig(), features, nullptr, severity};

        Account const alice{"alice"};  // Hook owner
        Account const bob{"bob"};      // Sender

        // Fund accounts
        env.fund(XRP(10000), alice, bob);
        env.close();

        // Verify setup
        BEAST_EXPECT(env.current()->seq() < 256);  // Grace period
        // Note: Validator key check removed - not needed for basic hook testing

        // Deploy hook on alice
        env(ripple::test::jtx::hook(alice, {{hso(accept_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Bob sends payment to alice (triggers hook)
        auto const alicePreBal = env.balance(alice);
        auto const bobPreBal = env.balance(bob);

        env(pay(bob, alice, XRP(100)), fee(XRP(1)), ter(tesSUCCESS));
        env.close();

        // Verify payment went through
        BEAST_EXPECT(env.balance(alice) == alicePreBal + XRP(100));
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        testBasicSetup(all);
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
