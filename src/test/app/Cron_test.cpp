//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL Labs

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

#include <ripple/app/ledger/LedgerMaster.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/jss.h>
#include "ripple/protocol/Indexes.h"
#include "ripple/protocol/TER.h"
#include "ripple/protocol/TxFlags.h"
#include "test/jtx/TestHelpers.h"
#include "test/jtx/cron.h"
#include <test/jtx.h>

namespace ripple {
namespace test {
struct Cron_test : public beast::unit_test::suite
{
    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const issuer = Account("issuer");

        for (bool const withCron : {false, true})
        {
            // If the BalanceRewards amendment is not enabled, you should not be
            // able to claim rewards.
            auto const amend = withCron ? features : features - featureCron;
            Env env{*this, amend};

            env.fund(XRP(1000), alice, issuer);
            env.close();

            auto const expectResult =
                withCron ? ter(tesSUCCESS) : ter(temDISABLED);

            auto tx = cron::set(alice);
            // CLAIM
            env(cron::set(alice), fee(XRP(1)), expectResult);
            env.close();
        }
    }

    void
    testFee(FeatureBitset features)
    {
        testcase("fee");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        auto const alice = Account("alice");
        Env env{*this, features | featureCron};

        auto const baseFee = env.current()->fees().base;

        env.fund(XRP(1000), alice);
        env.close();

        // create with RepeatCount
        auto expected = baseFee * 2 + baseFee * 256;
        env(cron::set(alice),
            cron::delay(356 * 24 * 60 * 60),
            cron::repeat(256),
            fee(expected - 1),
            ter(telINSUF_FEE_P));
        env.close();

        env(cron::set(alice),
            cron::delay(356 * 24 * 60 * 60),
            cron::repeat(256),
            fee(expected),
            ter(tesSUCCESS));
        env.close();

        // create with no RepeatCount
        expected = baseFee * 2;
        env(cron::set(alice),
            cron::delay(356 * 24 * 60 * 60),
            fee(expected - 1),
            ter(telINSUF_FEE_P));
        env.close();

        env(cron::set(alice),
            cron::delay(356 * 24 * 60 * 60),
            fee(expected),
            ter(tesSUCCESS));
        env.close();

        // delete
        expected = baseFee;
        env(cron::set(alice), fee(expected - 1), ter(telINSUF_FEE_P));
        env.close();

        env(cron::set(alice), fee(expected), ter(tesSUCCESS));
        env.close();
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("invalid preflight");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");

        test::jtx::Env env{
            *this, network::makeNetworkConfig(21337), features | featureCron};

        env.fund(XRP(1000), alice);
        env.close();

        //----------------------------------------------------------------------
        // preflight

        // temINVALID_FLAG
        // can have flag 1 set to opt-out of rewards
        {
            env(cron::set(alice), txflags(tfClose), ter(temINVALID_FLAG));
            env(cron::set(alice),
                txflags(tfUniversalMask),
                ter(temINVALID_FLAG));
        }

        // temMALFORMED
        {
            // Invalid DelaySeconds and RepeatCount combination
            // (only RepeatCount specified)
            env(cron::set(alice), cron::repeat(256), ter(temMALFORMED));
            env.close();

            // Invalid DelaySeconds
            env(cron::set(alice),
                cron::delay(365 * 24 * 60 * 60 + 1),
                cron::repeat(256),
                ter(temMALFORMED));
            env.close();

            // Invalid RepeatCount
            env(cron::set(alice),
                cron::delay(365 * 24 * 60 * 60),
                cron::repeat(257),
                ter(temMALFORMED));
            env.close();
        }
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("invalid preclaim");
        using namespace test::jtx;
        using namespace std::literals;

        // no preclaim checks exists
        BEAST_EXPECT(true);
    }

    void
    testDoApply(FeatureBitset features)
    {
        testcase("doApply");
        using namespace jtx;
        using namespace std::literals::chrono_literals;
        auto const alice = Account("alice");
        Env env{*this, features | featureCron};

        env.fund(XRP(1000), alice);
        env.close();

        auto const aliceOwnerCount = ownerCount(env, alice);

        // create cron
        env(cron::set(alice),
            cron::delay(356 * 24 * 60 * 60),
            cron::repeat(256),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // increment owner count
        BEAST_EXPECT(ownerCount(env, alice) == aliceOwnerCount + 1);

        auto const accSle = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accSle);
        BEAST_EXPECT(accSle->isFieldPresent(sfCron));

        auto const cronKey = keylet::child(accSle->getFieldH256(sfCron));
        auto const cronSle = env.le(cronKey);
        BEAST_EXPECT(cronSle);
        BEAST_EXPECT(
            cronSle->getFieldU32(sfDelaySeconds) == 356 * 24 * 60 * 60);
        BEAST_EXPECT(cronSle->getFieldU32(sfRepeatCount) == 256);

        // update cron
        env(cron::set(alice),
            cron::delay(100),
            cron::repeat(10),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // owner count does not change
        BEAST_EXPECT(ownerCount(env, alice) == aliceOwnerCount + 1);

        auto const accSle2 = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accSle2);
        BEAST_EXPECT(accSle2->isFieldPresent(sfCron));

        // old cron sle is deleted
        BEAST_EXPECT(!env.le(cronKey));

        auto const cronKey2 = keylet::child(accSle2->getFieldH256(sfCron));
        auto const cronSle2 = env.le(cronKey2);
        BEAST_EXPECT(cronSle2);
        BEAST_EXPECT(cronSle2->getFieldU32(sfDelaySeconds) == 100);
        BEAST_EXPECT(cronSle2->getFieldU32(sfRepeatCount) == 10);

        // delete cron
        env(cron::set(alice), fee(XRP(1)), ter(tesSUCCESS));
        env.close();

        // owner count decremented
        BEAST_EXPECT(ownerCount(env, alice) == aliceOwnerCount);

        auto const accSle3 = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accSle3);
        BEAST_EXPECT(!accSle3->isFieldPresent(sfCron));

        // old cron sle is deleted
        BEAST_EXPECT(!env.le(cronKey2));
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testFee(features);
        testInvalidPreflight(features);
        testInvalidPreclaim(features);
        testDoApply(features);
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

BEAST_DEFINE_TESTSUITE(Cron, app, ripple);

}  // namespace test
}  // namespace ripple
