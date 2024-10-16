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
#include <ripple/protocol/jss.h>
#include <optional>
#include <test/jtx.h>

namespace ripple {
namespace test {
struct GenesisMint_test : public beast::unit_test::suite
{
    void
    validateEmittedTxn(jtx::Env& env, std::string result, uint64_t lineno)
    {
        // get the emitted txn id
        Json::Value params;
        params[jss::transaction] =
            env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        auto const meta = jrr[jss::result][jss::meta];
        auto const emissions = meta[sfHookEmissions.jsonName];
        auto const emission = emissions[0u][sfHookEmission.jsonName];
        auto const txId = emission[sfEmittedTxnID.jsonName];
        env.close();

        // verify emitted result
        Json::Value params1;
        params1[jss::transaction] = txId;
        auto const jrr1 = env.rpc("json", "tx", to_string(params1));
        auto const meta1 = jrr1[jss::result][jss::meta];
        BEAST_EXPECT(meta1[sfTransactionResult.jsonName] == result);
        if (meta1[sfTransactionResult.jsonName] != result)
        {
            std::cout << "validateEmittedTxn failed " << lineno
                      << " expected: " << result
                      << " result: " << meta1[sfTransactionResult.jsonName]
                      << "\n";
        }
    }
    void
    testDisabled(FeatureBitset features)
    {
        testcase("Disabled");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        {
            test::jtx::Env env(*this, features - featureXahauGenesis);

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(10000), alice, bob);
            env.close();

            env(genesis::mint(
                    env.master, {genesis::GenMint(bob.id(), XRP(123))}),
                ter(temDISABLED));
        }

        {
            test::jtx::Env env(*this, features - featureHooks);

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(10000), alice, bob);
            env.close();

            env(genesis::mint(env.master, {{bob.id(), XRP(123)}}),
                ter(temDISABLED));
        }
    }

    void
    testNonGenesisEmit(FeatureBitset features)
    {
        testcase("Non-Genesis Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        env.fund(XRP(10000), alice, bob, invoker);
        env.close();

        // set the test hook
        env(genesis::setMintHook(alice), fee(XRP(10)));
        env.close();

        // this should fail because emitted txns are preflighted
        // and the preflight checks the account and will find it's not genesis
        env(invoke::invoke(
                invoker,
                alice,
                genesis::makeBlob({
                    {bob.id(), XRP(123).value(), std::nullopt, std::nullopt},
                })),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));
    }

    Json::Value
    burn(jtx::Account const& account)
    {
        Json::Value tx = Json::objectValue;
        tx[jss::Account] = account.human();
        tx[jss::TransactionType] = "AccountSet";
        return tx;
    }

    void
    testGenesisEmit(FeatureBitset features)
    {
        testcase("Genesis Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        auto const gw = Account("gateway");
        auto const USD = gw["USD"];
        auto const edward = Account("edward");
        env.fund(XRP(10000), alice, bob, invoker, edward);
        env.close();

        // burn down the total ledger coins so that genesis mints don't mint
        // above 100B tripping invariant
        env(burn(env.master), fee(XRP(10'000'000ULL)));
        env.close();

        // set the test hook
        env(genesis::setMintHook(env.master), fee(XRP(10)));
        env.close();

        // test a mint
        {
            auto const initCoins = env.current()->info().drops;
            {
                auto acc = env.le(keylet::account(bob.id()));
                BEAST_EXPECT(
                    acc->getFieldAmount(sfBalance).xrp().drops() ==
                    10000000000ULL);
            }

            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {bob.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)),
                ter(tesSUCCESS));

            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);

            {
                auto acc = env.le(keylet::account(bob.id()));
                BEAST_EXPECT(
                    acc->getFieldAmount(sfBalance).xrp().drops() ==
                    10123000000ULL);
            }
            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(
                initCoins - 1'000'000 /* txn fee */
                    - 10              /* emitted txn fee */
                    + 123'000'000     /* minted */
                == postCoins);
        }

        // creating accounts
        auto const carol = Account("carol");
        auto const david = Account("david");
        BEAST_EXPECT(!env.le(keylet::account(carol.id())));
        BEAST_EXPECT(!env.le(keylet::account(david.id())));

        env(invoke::invoke(
                invoker,
                env.master,
                genesis::makeBlob({
                    {david.id(),
                     XRP(12345).value(),
                     std::nullopt,
                     std::nullopt},
                    {carol.id(),
                     XRP(67890).value(),
                     std::nullopt,
                     std::nullopt},
                })),
            fee(XRP(1)),
            ter(tesSUCCESS));

        env.close();

        // validate emitted txn
        validateEmittedTxn(env, "tesSUCCESS", __LINE__);

        env.close();

        {
            auto acc = env.le(keylet::account(carol.id()));
            BEAST_EXPECT(
                acc->getFieldAmount(sfBalance).xrp().drops() == 67890000000ULL);
            BEAST_EXPECT(acc->getFieldU32(sfSequence) == 60);
        }

        {
            auto acc = env.le(keylet::account(david.id()));
            BEAST_EXPECT(
                acc->getFieldAmount(sfBalance).xrp().drops() == 12345000000ULL);
            BEAST_EXPECT(acc->getFieldU32(sfSequence) == 60);
        }

        // lots of entries
        uint16_t accid[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};

        std::vector<std::tuple<
            std::optional<AccountID>,
            std::optional<STAmount>,
            std::optional<uint256>,
            std::optional<uint256>>>
            mints;

        for (uint16_t i = 0; i < 512; ++i)
        {
            accid[0] = i;
            AccountID acc = AccountID::fromVoid((void*)&accid);
            mints.emplace_back(
                acc, XRP(i + 1).value(), std::nullopt, std::nullopt);
        }

        env(invoke::invoke(invoker, env.master, genesis::makeBlob(mints)),
            fee(XRP(1)));

        env.close();

        // validate emitted txn
        validateEmittedTxn(env, "tesSUCCESS", __LINE__);

        for (auto const& [acc, amt, _, __] : mints)
        {
            auto const le = env.le(keylet::account(*acc));
            BEAST_EXPECT(!!le && le->getFieldAmount(sfBalance) == *amt);
        }

        // again, and check the amounts increased x2
        env(invoke::invoke(invoker, env.master, genesis::makeBlob(mints)),
            fee(XRP(1)));

        env.close();

        // validate emitted txn
        validateEmittedTxn(env, "tesSUCCESS", __LINE__);

        for (auto const& [acc, amt, _, __] : mints)
        {
            auto const le = env.le(keylet::account(*acc));
            BEAST_EXPECT(!!le && le->getFieldAmount(sfBalance) == *amt * 2);
            BEAST_EXPECT(le->getAccountID(sfAccount) == acc);
        }

        // too many entries
        {
            accid[0] = 512;
            AccountID acc = AccountID::fromVoid((void*)&accid);
            mints.emplace_back(acc, XRP(1).value(), std::nullopt, std::nullopt);
        }

        env(invoke::invoke(invoker, env.master, genesis::makeBlob(mints)),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        int i = 0;

        // check the amounts didn't change
        for (auto const& [acc, amt, _, __] : mints)
        {
            auto const le = env.le(keylet::account(*acc));
            BEAST_EXPECT(!!le && le->getFieldAmount(sfBalance) == *amt * 2);
            if (++i == 512)
                break;
        }

        // invalid amount should cause emit fail which should cause hook
        // rollback
        env(invoke::invoke(
                invoker,
                env.master,
                genesis::makeBlob(
                    {{edward.id(), USD(100), std::nullopt, std::nullopt}})),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        // zero xrp is not allowed
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob(
                        {{edward.id(), XRP(0), std::nullopt, std::nullopt}})),
                fee(XRP(1)),
                ter(tesSUCCESS));

            env.close();

            // validate emitted txn
            auto const txResult = env.current()->rules().enabled(fixXahauV1)
                ? "tesSUCCESS"
                : "tecINTERNAL";
            validateEmittedTxn(env, txResult, __LINE__);
        }

        // missing an amount
        env(invoke::invoke(
                invoker,
                env.master,
                genesis::makeBlob({
                    {edward.id(), std::nullopt, std::nullopt, std::nullopt},
                })),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        // missing a destination
        env(invoke::invoke(
                invoker,
                env.master,
                genesis::makeBlob({
                    {std::nullopt, XRP(1), std::nullopt, std::nullopt},
                })),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        uint256 marks;
        uint256 flags;
        BEAST_EXPECT(
            flags.parseHex("000000000000000000000000000000000000000000000000000"
                           "0000000000001"));
        BEAST_EXPECT(
            marks.parseHex("100000000000000000000000000000000000000000000000000"
                           "0000000000000"));

        // dest + flags
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {alice.id(), std::nullopt, flags, std::nullopt},
                    })),
                fee(XRP(1)));

            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);
        }

        // check that alice has the right balance, and Governance Flags set
        {
            auto const le = env.le(keylet::account(alice.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance) == XRP(10000).value());
            BEAST_EXPECT(
                !!le && le->isFieldPresent(sfGovernanceFlags) &&
                le->getFieldH256(sfGovernanceFlags) == flags);
        }

        // now governance marks on bob
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {bob.id(), std::nullopt, std::nullopt, marks},
                    })),
                fee(XRP(1)));

            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);
        }

        // check that bob has the right balance, and Governance Marks set
        {
            auto const le = env.le(keylet::account(bob.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance) == XRP(10123).value());
            BEAST_EXPECT(
                !!le && le->isFieldPresent(sfGovernanceMarks) &&
                le->getFieldH256(sfGovernanceMarks) == marks);
        }

        // all at once
        auto const fred = Account("fred");
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {fred.id(), XRP(589).value(), flags, marks},
                    })),
                fee(XRP(1)));

            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);
        }

        // check
        {
            auto const le = env.le(keylet::account(fred.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance) == XRP(589).value());
            BEAST_EXPECT(
                !!le && le->isFieldPresent(sfGovernanceMarks) &&
                le->getFieldH256(sfGovernanceMarks) == marks);
            BEAST_EXPECT(
                !!le && le->isFieldPresent(sfGovernanceFlags) &&
                le->getFieldH256(sfGovernanceFlags) == flags);
        }

        // mint a zero account
        auto const greg = Account("greg");
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob(
                        {{greg.id(),
                          XRP(0).value(),
                          std::nullopt,
                          std::nullopt}})),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance).xrp().drops() == 0);
        }

        // try to mint negative
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob(
                        {{greg.id(),
                          XRP(-1).value(),
                          std::nullopt,
                          std::nullopt}})),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));
            env.close();
            env.close();
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance).xrp().drops() == 0);
        }

        // try to mint too much
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob(
                        {{greg.id(),
                          XRP(100000000000ULL).value(),
                          std::nullopt,
                          std::nullopt}})),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));
            env.close();
            env.close();
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance).xrp().drops() == 0);
        }

        // mint a regular amount
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob(
                        {{greg.id(),
                          XRP(10).value(),
                          std::nullopt,
                          std::nullopt}})),
                fee(XRP(1)));
            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le &&
                le->getFieldAmount(sfBalance).xrp().drops() == 10000000ULL);
        }

        // try destination is genesis
        {
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {env.master.id(),
                         XRP(10).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tesSUCCESS", __LINE__);
        }

        auto const amtResult = env.current()->rules().enabled(fixXahauV1)
            ? 30000000ULL
            : 10000000ULL;
        // try to include the same destination twice
        {
            auto const txResult = env.current()->rules().enabled(fixXahauV1)
                ? ter(tesSUCCESS)
                : ter(tecHOOK_REJECTED);
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {greg.id(),
                         XRP(10).value(),
                         std::nullopt,
                         std::nullopt},
                        {greg.id(),
                         XRP(10).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)),
                txResult);
            env.close();
            env.close();
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le &&
                le->getFieldAmount(sfBalance).xrp().drops() == amtResult);
        }

        // trip the supply cap invariant
        {
            auto const initCoins = env.current()->info().drops;
            env(invoke::invoke(
                    invoker,
                    env.master,
                    genesis::makeBlob({
                        {greg.id(),
                         XRP(9'999'999).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)));
            env.close();

            // validate emitted txn
            validateEmittedTxn(env, "tecINVARIANT_FAILED", __LINE__);

            // check balance wasn't changed
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le &&
                le->getFieldAmount(sfBalance).xrp().drops() == amtResult);

            auto const postCoins = env.current()->info().drops;
            BEAST_EXPECT(
                initCoins - 1'000'000 /* txn fee  */ - 10 /* emitted txn fee */
                == postCoins);
        }
    }

    void
    testGenesisNonEmit(FeatureBitset features)
    {
        testcase("Genesis Non-Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        env(genesis::mint(env.master, {genesis::GenMint(bob.id(), XRP(123))}),
            ter(temMALFORMED));

        auto const le = env.le(keylet::account(bob.id()));
        BEAST_EXPECT(
            !!le &&
            le->getFieldAmount(sfBalance).xrp().drops() == 10000000000ULL);
    }

    void
    testNonGenesisNonEmit(FeatureBitset features)
    {
        testcase("Non-Genesis Non-Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        env(genesis::mint(alice, {genesis::GenMint(bob.id(), XRP(123))}),
            ter(temMALFORMED));

        auto const le = env.le(keylet::account(bob.id()));
        BEAST_EXPECT(
            !!le &&
            le->getFieldAmount(sfBalance).xrp().drops() == 10000000000ULL);
    }

public:
    void
    testWithFeats(FeatureBitset features)
    {
        testDisabled(features);
        testGenesisEmit(features);
        testGenesisNonEmit(features);
        testNonGenesisEmit(features);
        testNonGenesisNonEmit(features);
    }
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
        testWithFeats(sa - fixXahauV1);
    }
};

BEAST_DEFINE_TESTSUITE(GenesisMint, app, ripple);

}  // namespace test
}  // namespace ripple