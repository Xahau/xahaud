//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2023 XRPL Labs

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
#include <test/jtx.h>

namespace ripple {
namespace test {
struct ClaimReward_test : public beast::unit_test::suite
{
    Json::Value
    claim(
        jtx::Account const& account,
        std::optional<jtx::Account> const& issuer = std::nullopt,
        std::uint32_t flags = 0)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::ClaimReward;
        jv[jss::Account] = account.human();
        if (issuer)
            jv[sfIssuer.jsonName] = issuer->human();
        if (flags)
            jv[jss::Flags] = flags;
        return jv;
    }

    std::unique_ptr<Config>
    makeNetworkConfig(uint32_t networkID)
    {
        using namespace jtx;
        return envconfig([&](std::unique_ptr<Config> cfg) {
            cfg->NETWORK_ID = networkID;
            Section config;
            config.append(
                {"reference_fee = 10",
                 "account_reserve = 1000000",
                 "owner_reserve = 200000"});
            auto setup = setup_FeeVote(config);
            cfg->FEES = setup;
            return cfg;
        });
    }

    bool
    expectRewards(
        jtx::Env const& env,
        jtx::Account const& acct,
        std::uint32_t ledgerFirst,
        std::uint32_t ledgerLast,
        std::uint64_t accumulator,
        std::uint32_t time)
    {
        auto const sle = env.le(keylet::account(acct));
        if (!sle->isFieldPresent(sfRewardLgrFirst) ||
            sle->getFieldU32(sfRewardLgrFirst) != ledgerFirst)
        {
            return false;
        }
        if (!sle->isFieldPresent(sfRewardLgrLast) ||
            sle->getFieldU32(sfRewardLgrLast) != ledgerLast)
        {
            return false;
        }
        if (!sle->isFieldPresent(sfRewardAccumulator) ||
            sle->getFieldU64(sfRewardAccumulator) != accumulator)
        {
            return false;
        }
        if (!sle->isFieldPresent(sfRewardTime) ||
            sle->getFieldU32(sfRewardTime) != time)
        {
            return false;
        }
        return true;
    }

    bool
    expectNoRewards(jtx::Env const& env, jtx::Account const& acct)
    {
        auto const sle = env.le(keylet::account(acct));
        if (sle->isFieldPresent(sfRewardLgrFirst))
        {
            return false;
        }
        if (sle->isFieldPresent(sfRewardLgrLast))
        {
            return false;
        }
        if (sle->isFieldPresent(sfRewardAccumulator))
        {
            return false;
        }
        if (sle->isFieldPresent(sfRewardTime))
        {
            return false;
        }
        return true;
    }

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        // setup env
        auto const alice = Account("alice");
        auto const issuer = Account("issuer");

        for (bool const withClaimReward : {false, true})
        {
            // If the BalanceRewards amendment is not enabled, you should not be
            // able to claim rewards.
            auto const amend =
                withClaimReward ? features : features - featureBalanceRewards;
            Env env{*this, amend};

            env.fund(XRP(1000), alice, issuer);
            env.close();

            auto const txResult =
                withClaimReward ? ter(tesSUCCESS) : ter(temDISABLED);

            auto const currentLedger = env.current()->seq();
            auto const currentTime =
                std::chrono::duration_cast<std::chrono::seconds>(
                    env.app()
                        .getLedgerMaster()
                        .getValidatedLedger()
                        ->info()
                        .parentCloseTime.time_since_epoch())
                    .count();

            // CLAIM
            auto tx = claim(alice);
            tx[sfIssuer.jsonName] = issuer.human();
            env(tx, txResult);
            env.close();

            if (withClaimReward)
            {
                BEAST_EXPECT(
                    expectRewards(
                        env,
                        alice,
                        currentLedger,
                        currentLedger,
                        0,
                        currentTime) == true);
            }
            else
            {
                BEAST_EXPECT(expectNoRewards(env, alice) == true);
            }
        }
    }

    void
    testInvalidPreflight(FeatureBitset features)
    {
        testcase("invalid preflight");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preflight

        // temDISABLED
        // amendment is disabled
        {
            test::jtx::Env env{
                *this,
                makeNetworkConfig(21337),
                features - featureBalanceRewards};

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");

            env.fund(XRP(1000), alice, issuer);
            env.close();

            auto tx = claim(alice, issuer);
            env(tx, ter(temDISABLED));
            env.close();
        }

        // temINVALID_FLAG
        // can have flag 1 set to opt-out of rewards
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");

            env.fund(XRP(1000), alice, issuer);
            env.close();

            auto tx = claim(alice, issuer);
            env(tx, txflags(tfClose), ter(temINVALID_FLAG));
            env.close();
        }

        // temMALFORMED
        // Issuer cannot be the source account.
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const alice = Account("alice");
            env.fund(XRP(1000), alice);
            env.close();

            env(claim(alice, alice), ter(temMALFORMED));
            env.close();
        }
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("invalid preclaim");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED: DA tested in testEnabled() & testInvalidPreflight()
        // amendment is disabled

        // terNO_ACCOUNT
        // otxn account does not exist.
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");
            env.memoize(alice);

            env.fund(XRP(1000), issuer);
            env.close();

            auto tx = claim(alice, issuer);
            tx[jss::Sequence] = 0;
            tx[jss::Fee] = 10;
            env(tx, ter(terNO_ACCOUNT));
            env.close();
        }

        // temMALFORMED
        // (issuer && isOptOut)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");

            env.fund(XRP(1000), alice, issuer);
            env.close();

            env(claim(alice, issuer, 1), ter(temMALFORMED));
            env.close();
        }
        // (!issuer && !isOptOut)
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const alice = Account("alice");

            env.fund(XRP(1000), alice);
            env.close();

            env(claim(alice), ter(temMALFORMED));
            env.close();
        }

        // tecNO_ISSUER
        // issuer account does not exist.
        {
            test::jtx::Env env{*this, makeNetworkConfig(21337)};

            auto const alice = Account("alice");
            auto const issuer = Account("issuer");
            env.memoize(issuer);

            env.fund(XRP(1000), alice);
            env.close();

            auto tx = claim(alice, issuer);
            env(tx, ter(tecNO_ISSUER));
            env.close();
        }
    }

    void
    testValidNoHook(FeatureBitset features)
    {
        testcase("valid no hook");
        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337)};

        auto const alice = Account("alice");
        auto const issuer = Account("issuer");

        env.fund(XRP(1000), alice, issuer);
        env.close();

        // test claim rewards - no opt out
        auto const currentLedger = env.current()->seq();
        auto const currentTime =
            std::chrono::duration_cast<std::chrono::seconds>(
                env.app()
                    .getLedgerMaster()
                    .getValidatedLedger()
                    ->info()
                    .parentCloseTime.time_since_epoch())
                .count();

        auto tx = claim(alice, issuer);
        env(tx, ter(tesSUCCESS));
        env.close();

        BEAST_EXPECT(
            expectRewards(
                env, alice, currentLedger, currentLedger, 0, currentTime) ==
            true);

        // test claim rewards - opt out
        env(claim(alice, std::nullopt, 1), ter(tesSUCCESS));
        env.close();

        BEAST_EXPECT(expectNoRewards(env, alice) == true);
    }

    void
    testUsingTickets(FeatureBitset features)
    {
        testcase("using tickets");
        using namespace jtx;
        using namespace std::literals::chrono_literals;
        Env env{*this, features};
        auto const alice = Account("alice");
        auto const issuer = Account("issuer");
        env.fund(XRP(10000), alice, issuer);
        std::uint32_t aliceTicketSeq{env.seq(alice) + 1};
        env(ticket::create(alice, 10));
        std::uint32_t const aliceSeq{env.seq(alice)};
        env.require(owners(alice, 10));

        env(claim(alice, issuer), ticket::use(aliceTicketSeq++), ter(tesSUCCESS));

        env.require(tickets(alice, env.seq(alice) - aliceTicketSeq));
        BEAST_EXPECT(env.seq(alice) == aliceSeq);
        env.require(owners(alice, 9));
    }

    void
    testWithFeats(FeatureBitset features)
    {
        testEnabled(features);
        testInvalidPreflight(features);
        testInvalidPreclaim(features);
        testValidNoHook(features);
        testUsingTickets(features);
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

BEAST_DEFINE_TESTSUITE(ClaimReward, app, ripple);

}  // namespace test
}  // namespace ripple
