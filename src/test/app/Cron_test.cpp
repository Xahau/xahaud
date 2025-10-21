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
            auto const amend = withCron ? features : features - featureCron;
            Env env{*this, amend};

            env.fund(XRP(1000), alice, issuer);
            env.close();

            auto const expectResult =
                withCron ? ter(tesSUCCESS) : ter(temDISABLED);

            // CLAIM
            env(cron::set(alice),
                cron::startTime(0),
                cron::repeat(100),
                cron::delay(100),
                fee(XRP(1)),
                expectResult);
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

        // create
        auto expected = baseFee * 2 + baseFee * 256;
        env(cron::set(alice),
            cron::startTime(0),
            cron::delay(356 * 24 * 60 * 60),
            cron::repeat(256),
            fee(expected - 1),
            ter(telINSUF_FEE_P));
        env.close();

        env(cron::set(alice),
            cron::startTime(0),
            cron::delay(356 * 24 * 60 * 60),
            cron::repeat(256),
            fee(expected),
            ter(tesSUCCESS));
        env.close();

        // delete
        expected = baseFee;
        env(cron::set(alice),
            txflags(tfCronUnset),
            fee(expected - 1),
            ter(telINSUF_FEE_P));
        env.close();

        env(cron::set(alice),
            txflags(tfCronUnset),
            fee(expected),
            ter(tesSUCCESS));
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
        {
            env(cron::set(alice), txflags(tfClose), ter(temINVALID_FLAG));
            env(cron::set(alice),
                txflags(tfUniversalMask),
                ter(temINVALID_FLAG));
        }

        // temMALFORMED
        {
            // Invalid DelaySeconds and RepeatCount and StartTime are not
            // specified
            env(cron::set(alice), ter(temMALFORMED));

            // Invalid DelaySeconds and RepeatCount combination with StartTime
            env(cron::set(alice),
                cron::startTime(100),
                cron::delay(356 * 24 * 60 * 60),
                ter(temMALFORMED));
            env(cron::set(alice),
                cron::startTime(100),
                cron::repeat(256),
                ter(temMALFORMED));

            // Invalid DelaySeconds
            env(cron::set(alice),
                cron::startTime(100),
                cron::delay(365 * 24 * 60 * 60 + 1),
                cron::repeat(256),
                ter(temMALFORMED));

            // Invalid RepeatCount
            env(cron::set(alice),
                cron::startTime(100),
                cron::delay(365 * 24 * 60 * 60),
                cron::repeat(257),
                ter(temMALFORMED));

            // Invalid with tfCronUnset flag
            env(cron::set(alice),
                cron::delay(365 * 24 * 60 * 60),
                txflags(tfCronUnset),
                ter(temMALFORMED));
            env(cron::set(alice),
                cron::repeat(100),
                txflags(tfCronUnset),
                ter(temMALFORMED));
            env(cron::set(alice),
                cron::startTime(100),
                txflags(tfCronUnset),
                ter(temMALFORMED));
        }
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("invalid preclaim");
        using namespace test::jtx;
        using namespace std::literals;

        auto const alice = Account("alice");
        Env env{*this, features | featureCron};
        env.fund(XRP(1000), alice);
        env.close();

        // Past StartTime
        env(cron::set(alice),
            cron::startTime(
                env.timeKeeper().now().time_since_epoch().count() - 1),
            fee(XRP(1)),
            ter(tecEXPIRED));
        env.close();

        // Too far Future StartTime
        env(cron::set(alice),
            cron::startTime(
                env.timeKeeper().now().time_since_epoch().count() +
                365 * 24 * 60 * 60 + 1),
            fee(XRP(1)),
            ter(tecEXPIRED));
        env.close();
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
        auto parentCloseTime =
            env.current()->parentCloseTime().time_since_epoch().count();
        env(cron::set(alice),
            cron::startTime(parentCloseTime + 356 * 24 * 60 * 60),
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
        BEAST_EXPECT(
            cronSle->getFieldU32(sfStartTime) ==
            parentCloseTime + 356 * 24 * 60 * 60);

        // update cron
        parentCloseTime =
            env.current()->parentCloseTime().time_since_epoch().count();
        env(cron::set(alice),
            cron::startTime(0),
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
        BEAST_EXPECT(cronSle2->getFieldU32(sfStartTime) == parentCloseTime);

        // delete cron
        env(cron::set(alice),
            fee(XRP(1)),
            txflags(tfCronUnset),
            ter(tesSUCCESS));
        env.close();

        // owner count decremented
        BEAST_EXPECT(ownerCount(env, alice) == aliceOwnerCount);

        auto const accSle3 = env.le(keylet::account(alice.id()));
        BEAST_EXPECT(accSle3);
        BEAST_EXPECT(!accSle3->isFieldPresent(sfCron));

        // old cron sle is deleted
        BEAST_EXPECT(!env.le(cronKey2));

        // delete cron without object will succeed
        env(cron::set(alice),
            fee(XRP(1)),
            txflags(tfCronUnset),
            ter(tesSUCCESS));
        env.close();
    }

    void
    testCronExecution(FeatureBitset features)
    {
        testcase("cron execution");
        using namespace jtx;
        using namespace std::literals::chrono_literals;
        auto const alice = Account("alice");

        {
            // test ttCron execution and repeatCount
            Env env{*this, features | featureCron};

            env.fund(XRP(1000), alice);
            env.close();

            auto baseTime = env.timeKeeper().now().time_since_epoch().count();

            auto repeatCount = 10;

            env(cron::set(alice),
                cron::startTime(baseTime + 100),
                cron::delay(100),
                cron::repeat(repeatCount),
                fee(XRP(1)));
            env.close(10s);

            auto lastCronKeylet =
                keylet::child(env.le(alice)->getFieldH256(sfCron));

            while (repeatCount >= 0)
            {
                // close ledger until 100 seconds has passed
                while (env.timeKeeper().now().time_since_epoch().count() -
                           baseTime <
                       100)
                {
                    env.close(10s);
                    auto txns = env.closed()->txs;
                    auto size = std::distance(txns.begin(), txns.end());
                    BEAST_EXPECT(size == 0);
                }

                // close after 100 seconds passed
                env.close(10s);

                auto txns = env.closed()->txs;
                auto size = std::distance(txns.begin(), txns.end());
                BEAST_EXPECT(size == 1);
                for (auto it = txns.begin(); it != txns.end(); ++it)
                {
                    auto const& tx = *it->first;
                    // check pseudo txn format
                    BEAST_EXPECT(tx.getTxnType() == ttCRON);
                    BEAST_EXPECT(tx.getAccountID(sfAccount) == AccountID());
                    BEAST_EXPECT(tx.getAccountID(sfOwner) == alice.id());
                    BEAST_EXPECT(
                        tx.getFieldU32(sfLedgerSequence) ==
                        env.closed()->info().seq);
                    BEAST_EXPECT(tx.getFieldAmount(sfFee) == XRP(0));
                    BEAST_EXPECT(tx.getFieldVL(sfSigningPubKey).size() == 0);

                    // check old Cron object is deleted
                    BEAST_EXPECT(!env.le(lastCronKeylet));

                    if (repeatCount > 0)
                    {
                        // check new Cron object
                        auto const cronKeylet =
                            keylet::child(env.le(alice)->getFieldH256(sfCron));
                        auto const cronSle = env.le(cronKeylet);
                        BEAST_EXPECT(cronSle);
                        BEAST_EXPECT(
                            cronSle->getFieldU32(sfDelaySeconds) == 100);
                        BEAST_EXPECT(
                            cronSle->getFieldU32(sfRepeatCount) ==
                            --repeatCount);
                        BEAST_EXPECT(
                            cronSle->getAccountID(sfOwner) == alice.id());

                        // set new base time
                        baseTime = baseTime + 100;
                        lastCronKeylet = cronKeylet;
                    }
                    else
                    {
                        // after all executions, the cron object should be
                        // deleted
                        BEAST_EXPECT(!env.le(alice)->isFieldPresent(sfCron));
                        BEAST_EXPECT(!env.le(lastCronKeylet));
                        --repeatCount;  // decrement for break double loop
                    }
                }
            }
        }

        {
            // test ttCron limit in a ledger
            Env env{*this, features | featureCron};
            std::vector<Account> accounts;
            accounts.reserve(300);
            for (int i = 0; i < 300; ++i)
            {
                auto const& account = accounts.emplace_back(
                    Account("account_" + std::to_string(i)));
                accounts.emplace_back(account);
                env.fund(XRP(10000), account);
            }
            env.close();

            for (auto const& account : accounts)
            {
                env(cron::set(account), cron::startTime(0), fee(XRP(1)));
            }
            env.close();

            // proceed ledger
            env.close();
            {
                auto const txns = env.closed()->txs;
                auto size = std::distance(txns.begin(), txns.end());
                BEAST_EXPECT(size == 128);
                for (auto it = txns.begin(); it != txns.end(); ++it)
                {
                    auto const& tx = *it->first;
                    BEAST_EXPECT(tx.getTxnType() == ttCRON);
                }
            }

            // proceed ledger
            env.close();
            {
                auto const txns = env.closed()->txs;
                auto size = std::distance(txns.begin(), txns.end());
                BEAST_EXPECT(size == 128);
                for (auto it = txns.begin(); it != txns.end(); ++it)
                {
                    auto const& tx = *it->first;
                    BEAST_EXPECT(tx.getTxnType() == ttCRON);
                }
            }

            // proceed ledger
            env.close();
            {
                auto const txns = env.closed()->txs;
                auto size = std::distance(txns.begin(), txns.end());
                BEAST_EXPECT(size == 44);
                for (auto it = txns.begin(); it != txns.end(); ++it)
                {
                    auto const& tx = *it->first;
                    BEAST_EXPECT(tx.getTxnType() == ttCRON);
                }
            }

            // proceed ledger
            env.close();
            {
                auto const txns = env.closed()->txs;
                auto size = std::distance(txns.begin(), txns.end());
                BEAST_EXPECT(size == 0);
            }
        }
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testFee(features);
        testInvalidPreflight(features);
        testInvalidPreclaim(features);
        testDoApply(features);

        testCronExecution(features);
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
