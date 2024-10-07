//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL-Labs

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
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class Memory_test : public beast::unit_test::suite
{
    void
    testPayment(FeatureBitset features)
    {
        testcase("payment");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, envconfig(), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();
    }

    void
    testHook(FeatureBitset features)
    {
        testcase("hook");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, envconfig(), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(10000), account, dest);
        env.close();

        env(genesis::setAcceptHook(account), fee(XRP(2)));
        env.close();

        for (int i = 0; i < 10; ++i)
        {
            env(pay(account, dest, XRP(1)), fee(XRP(1)));
            env.close();
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testPayment(features);
        testHook(features);
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

BEAST_DEFINE_TESTSUITE(Memory, app, ripple);

}  // namespace test
}  // namespace ripple
