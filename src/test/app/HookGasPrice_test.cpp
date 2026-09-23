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

#include <test/jtx.h>
#include <xrpld/app/misc/FeeVote.h>
#include <xrpld/app/tx/detail/Transactor.h>
#include <xrpld/core/Config.h>
#include <xrpl/basics/BasicConfig.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Fees.h>
#include <xrpl/protocol/Indexes.h>

namespace ripple {
namespace test {

class HookGasPrice_test : public beast::unit_test::suite
{
    // Advance to flag ledger so fee voting sets sfHookGasPrice
    void
    advanceToFlagLedger(jtx::Env& env)
    {
        auto const seq = env.current()->info().seq;
        for (auto i = seq; i <= 256; ++i)
            env.close();
        env.close();
    }

    void
    testHookGasPriceGenesis(FeatureBitset features)
    {
        testcase("GasPrice genesis");
        using namespace jtx;

        // Test Env passes empty amendments to genesis, so sfHookGasPrice
        // is NOT set in the genesis FeeSettings SLE.
        {
            Env env{*this, features};
            auto const sle = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(!sle->isFieldPresent(sfHookGasPrice));
        }

        // Without featureHookGas - also no sfHookGasPrice
        {
            Env env{*this, features - featureHookGas};
            auto const sle = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(!sle->isFieldPresent(sfHookGasPrice));
        }
    }

    void
    testHookGasPriceFeeVoting(FeatureBitset features)
    {
        testcase("GasPrice fee voting");
        using namespace jtx;

        Env env{*this, features};
        env.fund(XRP(10000), Account{"alice"});
        env.close();

        // Before flag ledger: sfHookGasPrice not in SLE
        {
            auto const sle = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(!sle->isFieldPresent(sfHookGasPrice));
        }

        // Advance to flag ledger to trigger fee voting
        advanceToFlagLedger(env);

        // After fee voting with featureHookGas enabled:
        // sfHookGasPrice should be set in SLE via ttFEE pseudo-transaction
        {
            auto const sle = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(sle->isFieldPresent(sfHookGasPrice));
            if (sle->isFieldPresent(sfHookGasPrice))
                BEAST_EXPECT(sle->getFieldU64(sfHookGasPrice) == 1'000'000);
        }
        BEAST_EXPECT(
            env.current()->fees().hookGasPrice == XRPAmount{1'000'000});
    }

    void
    testHookGasPriceVotingUpdate(FeatureBitset features)
    {
        testcase("GasPrice voting update");
        using namespace jtx;

        // Create env with custom gas_price = 2,000,000 via voting config
        auto cfg = envconfig();
        auto& votingSection = cfg->section("voting");
        votingSection.set("hook_gas_price", "2000000");
        // A validation_seed is required for fee voting to work
        cfg->section("validation_seed").legacy("shUwVw52ofnCUX5m7kPTKzJdr4HEH");

        Env env{*this, std::move(cfg), features};
        env.fund(XRP(10000), Account{"alice"});
        env.close();

        // Before flag ledger: sfHookGasPrice not in SLE
        {
            auto const sle = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(!sle->isFieldPresent(sfHookGasPrice));
        }

        // Advance to flag ledger to trigger fee voting
        advanceToFlagLedger(env);

        // After voting: sfHookGasPrice should be updated to 2,000,000
        {
            auto const sle = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle))
                return;
            BEAST_EXPECT(sle->isFieldPresent(sfHookGasPrice));
            if (sle->isFieldPresent(sfHookGasPrice))
                BEAST_EXPECT(sle->getFieldU64(sfHookGasPrice) == 2'000'000);
        }
    }

    void
    testHookGasPriceFeeCalculation(FeatureBitset features)
    {
        testcase("GasPrice fee calculation");
        using namespace jtx;

        {
            // With default gasPrice = 1,000,000:
            auto cfg = envconfig();
            auto& votingSection = cfg->section("voting");
            votingSection.set("hook_gas_price", "1000000");
            // A validation_seed is required for fee voting to work
            cfg->section("validation_seed")
                .legacy("shUwVw52ofnCUX5m7kPTKzJdr4HEH");

            Env env{*this, std::move(cfg), features};
            env.fund(XRP(10000), Account{"alice"});
            env.close();

            // Advance to flag ledger so gasPrice is set via fee voting
            advanceToFlagLedger(env);

            auto const& fees = env.current()->fees();
            BEAST_EXPECT(fees.hookGasPrice == XRPAmount{1'000'000});

            // Verify the gas price calculation formula:
            // fee = gasCount * gasPrice / 1,000,000

            // With default gasPrice = 1,000,000:
            // 1,000,000 gas => 1,000,000 * 1,000,000 / 1,000,000 = 1,000,000
            // 500,000 gas => 500,000 drops
            // 0 gas => 0 drops
            for (auto gasCount : {1'000'000, 500'000, 0})
            {
                auto gasFee = Transactor::calculateHookGas(gasCount, fees);
                BEAST_EXPECT(gasFee == XRPAmount{gasCount});
            }
        }

        {
            // With gasPrice = 100,000:
            auto cfg = envconfig();
            auto& votingSection = cfg->section("voting");
            votingSection.set("hook_gas_price", "100000");
            // A validation_seed is required for fee voting to work
            cfg->section("validation_seed")
                .legacy("shUwVw52ofnCUX5m7kPTKzJdr4HEH");

            Env env{*this, std::move(cfg), features};
            env.fund(XRP(10000), Account{"alice"});
            env.close();

            // Advance to flag ledger so gasPrice is set via fee voting
            advanceToFlagLedger(env);

            auto const& fees = env.current()->fees();
            BEAST_EXPECT(fees.hookGasPrice == XRPAmount{100'000});

            // Verify the gas price calculation formula:
            // fee = gasCount * gasPrice / 1,000,000

            // With default gasPrice = 1,000,000:
            // 1,000,000 gas => 1,000,000 * 100,000 / 1,000,000 = 100,000
            // 500,000 gas => 50,000 drops
            // 0 gas => 0 drops
            for (auto gasCount : {1'000'000, 500'000, 0})
            {
                auto gasFee = Transactor::calculateHookGas(gasCount, fees);
                BEAST_EXPECT(gasFee == XRPAmount{gasCount / 10});
            }
        }
    }

    void
    testHookGasPriceDisabled(FeatureBitset features)
    {
        testcase("GasPrice disabled");
        using namespace jtx;

        Env env{*this, features - featureHookGas};
        env.fund(XRP(10000), Account{"alice"});
        env.close();

        // sfHookGasPrice not in SLE when amendment disabled
        auto const sle = env.le(keylet::fees());
        if (!BEAST_EXPECT(sle))
            return;
        BEAST_EXPECT(!sle->isFieldPresent(sfHookGasPrice));

        // Advance past flag ledger - sfHookGasPrice should still not be set
        advanceToFlagLedger(env);

        {
            auto const sle2 = env.le(keylet::fees());
            if (!BEAST_EXPECT(sle2))
                return;
            BEAST_EXPECT(!sle2->isFieldPresent(sfHookGasPrice));
        }
    }

    void
    run() override
    {
        using namespace jtx;
        auto const sa = supported_amendments();
        testHookGasPriceGenesis(sa);
        testHookGasPriceFeeVoting(sa);
        testHookGasPriceVotingUpdate(sa);
        testHookGasPriceFeeCalculation(sa);
        testHookGasPriceDisabled(sa);
    }
};

BEAST_DEFINE_TESTSUITE(HookGasPrice, app, ripple);

}  // namespace test
}  // namespace ripple
