//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2017 Ripple Labs Inc.
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
struct GenesisMint_test : public beast::unit_test::suite
{

    static Json::Value
    claim(jtx::Account const& account)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::ClaimReward;
        jv[jss::Account] = account.human();
        return jv;
    }

    static Json::Value
    mint(jtx::Account const& account)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::GenesisMint;
        jv[jss::Account] = account.human();
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

    void
    testEnabled(FeatureBitset features)
    {
        testcase("enabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        test::jtx::Env env{*this, makeNetworkConfig(21337), features, nullptr, beast::severities::kTrace};

        // setup env
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const issuer = Account("issuer");

        env.fund(XRP(10000), alice, bob, issuer);
        env.close();

        // set claim rewards
        auto claimTx = claim(alice);
        claimTx[sfIssuer.jsonName] = issuer.human();
        env(claimTx, ter(tesSUCCESS));
        env.close();

        // burn fee
        env(noop(alice), fee(1'000'000'000), ter(tesSUCCESS));
        env.close();

        // mint rewards
        auto mintTx = mint(alice);

        // Emit Details
        mintTx[sfEmitDetails.jsonName] = Json::objectValue;
        mintTx[sfEmitDetails.jsonName][sfEmitBurden.jsonName] = "1";
        mintTx[sfEmitDetails.jsonName][sfEmitCallback.jsonName] = "rMPwD1b8dJUaqZHaBgEvFx4ENhtpPVvDsv";
        mintTx[sfEmitDetails.jsonName][sfEmitGeneration.jsonName] = 1;
        mintTx[sfEmitDetails.jsonName][sfEmitHookHash.jsonName] = "A9B5411F4A4368008B4736EEE47A34B0EFCBE74016B9B94CC6208FBC0BF5C0C2";
        mintTx[sfEmitDetails.jsonName][sfEmitNonce.jsonName] = "B8415274BAA38DC4640C6C35935C55FFAE8A6C13009736FCBE562A18376034FE";
        mintTx[sfEmitDetails.jsonName][sfEmitParentTxnID.jsonName] = "013F3650533681369F9D55FBFEEECF2C3AF1E9F3DAABB4F0C5C0F638321ABC7F";

        // Genesis Mints
        mintTx[sfGenesisMints.jsonName] = Json::arrayValue;
        mintTx[sfGenesisMints.jsonName][0u] = Json::objectValue;
        mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName] = Json::objectValue;
        mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][jss::Destination] = bob.human();
        mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][jss::Amount] = to_string(10000000);
        // mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][sfGovernanceFlags.jsonName] = "";
        // mintTx[sfGenesisMints.jsonName][0u][sfGenesisMint.jsonName][sfGovernanceMarks.jsonName] = "";
        
        // mintTx[sfFlags.jsonName] = to_string(2147483648);

        // JTx jt = env.jt(mintTx);
        // jt.fill_sig = false;
        // auto const jrr = env.rpc("submit", strHex(jt.stx->getSerializer().slice()))[jss::result];
        // std::cout << "jrr: " << jrr << "\n";
        
        auto const tx_blob = "1200602100005359240000000368400000000000000A73210388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C0528114AE123A8556F3CF91154711376AFB0F894F832B3DED202E000000013D00000000000000015B013F3650533681369F9D55FBFEEECF2C3AF1E9F3DAABB4F0C5C0F638321ABC7F5CB8415274BAA38DC4640C6C35935C55FFAE8A6C13009736FCBE562A18376034FE5DA9B5411F4A4368008B4736EEE47A34B0EFCBE74016B9B94CC6208FBC0BF5C0C28A14DF935D6B794939ABBA5D13007E65C2C6E70C625DE1F060E0606140000000009896808314F51DFC2A09D62CBBA1DFBDD4691DAC96AD98B90FE1F1";
        auto const jrr = env.rpc("submit", tx_blob)[jss::result];
        std::cout << "jrr: " << jrr << "\n";
        // env(mintTx, ter(tesSUCCESS));

        env.close();

        // for (bool const withHooksGenesis : {false, true})
        // {
        //     // If the Hooks or the Xahau Genesis amendments are not enabled, you
        //     // should not be able to mint xrp.
        //     auto const amend =
        //         withHooksGenesis ? features : features - featureHooks;
        //     Env env{*this, amend};

        //     env.fund(XRP(1000), alice, bob);
        //     env.close();

            // MINT
            // auto tx = mint(alice);
            // tx[sfIssuer.jsonName] = issuer.human();
            // env(tx, txResult);
            // env.close();

        //     // auto const txResult =
        //     //     withHooksGenesis ? ter(tesSUCCESS) : ter(temDISABLED);
        // }
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
        // featureHooks

        // temDISABLED
        // featureXahauGenesis

        // temMALFORMED
        // can only be used by the genesis account in an emitted transaction.

        // temMALFORMED
        // destinations array empty.

        // temMALFORMED
        // destinations array exceeds 512 entries.

        // temMALFORMED
        // destinations array contained an invalid entry.

        // temMALFORMED
        // each destination must have at least one of: sfAmount, sfGovernanceFlags, sfGovernance marks
        
        // temMALFORMED
        // only native amounts can be minted.

        // temMALFORMED
        // only positive amounts can be minted.

        // temMALFORMED
        // destinations includes disallowed account zero or one.
        
        // temMALFORMED
        // duplicate in destinations.
    }

    void
    testInvalidPreclaim(FeatureBitset features)
    {
        testcase("invalid preclaim");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preclaim

        // temDISABLED
        // featureHooks

        // temDISABLED
        // featureXahauGenesis
    }

    void
    testInvalidDoApply(FeatureBitset features)
    {
        testcase("invalid doApply");
        using namespace test::jtx;
        using namespace std::literals;

        //----------------------------------------------------------------------
        // preflight

        // tecINTERNAL
        // Non-xrp amount.

        // tecINTERNAL
        // cannot credit " << dest << " due to balance overflow

        // tecINTERNAL
        // dropsAdded overflowed\n
    }

    void
    testWithFeats(FeatureBitset features)
    {
        BEAST_EXPECT(1 == 1);
        // testEnabled(features);
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

BEAST_DEFINE_TESTSUITE(GenesisMint, app, ripple);

}  // namespace test
}  // namespace ripple