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

#include <ripple/app/tx/impl/XahauGenesis.h>
#include <ripple/protocol/Feature.h>
#include <optional>
#include <test/jtx.h>

namespace ripple {
namespace test {
struct GenesisMint_test : public beast::unit_test::suite
{
    struct GenMint
    {
        std::optional<std::string> dest;
        std::optional<jtx::PrettyAmount> amt;
        std::optional<std::string> marks;
        std::optional<std::string> flags;

        GenMint(
            AccountID const& dst,
            jtx::PrettyAmount const& x,
            std::optional<std::string> m = std::nullopt,
            std::optional<std::string> f = std::nullopt)
            : dest(toBase58(dst)), amt(x), marks(m), flags(f)
        {
        }
    };

    Json::Value
    mint(jtx::Account const& account, std::vector<GenMint> mints)
    {
        using namespace jtx;
        Json::Value jv;
        jv[jss::TransactionType] = jss::GenesisMint;
        jv[jss::Account] = account.human();
        jv[jss::GenesisMints] = Json::arrayValue;

        uint32_t counter = 0;
        for (auto const& m : mints)
        {
            Json::Value inner = Json::objectValue;
            if (m.dest)
                inner[jss::Destination] = *m.dest;
            if (m.amt)
                inner[jss::Amount] =
                    (*m.amt).value().getJson(JsonOptions::none);
            if (m.marks)
                inner[jss::GovernanceMarks] = *m.marks;
            if (m.flags)
                inner[jss::GovernanceFlags] = *m.flags;

            jv[jss::GenesisMints][counter] = Json::objectValue;
            jv[jss::GenesisMints][counter][jss::GenesisMint] = inner;
            counter++;
        }
        return jv;
    }

    Json::Value
    setMintHook(jtx::Account const& account)
    {
        using namespace jtx;
        Json::Value tx;
        tx[jss::Account] = account.human();
        tx[jss::TransactionType] = "SetHook";
        tx[jss::Hooks] = Json::arrayValue;
        tx[jss::Hooks][0u] = Json::objectValue;
        tx[jss::Hooks][0u][jss::Hook] = Json::objectValue;
        tx[jss::Hooks][0u][jss::Hook][jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        tx[jss::Hooks][0u][jss::Hook][jss::HookNamespace] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        tx[jss::Hooks][0u][jss::Hook][jss::HookApiVersion] = 0;

        tx[jss::Hooks][0u][jss::Hook][jss::CreateCode] =
            strHex(XahauGenesis::MintTestHook);
        return tx;
    }

    Json::Value
    setAcceptHook(jtx::Account const& account)
    {
        using namespace jtx;
        Json::Value tx;
        tx[jss::Account] = account.human();
        tx[jss::TransactionType] = "SetHook";
        tx[jss::Hooks] = Json::arrayValue;
        tx[jss::Hooks][0u] = Json::objectValue;
        tx[jss::Hooks][0u][jss::Hook] = Json::objectValue;
        tx[jss::Hooks][0u][jss::Hook][jss::HookOn] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        tx[jss::Hooks][0u][jss::Hook][jss::HookNamespace] =
            "0000000000000000000000000000000000000000000000000000000000000000";
        tx[jss::Hooks][0u][jss::Hook][jss::HookApiVersion] = 0;
        tx[jss::Hooks][0u][jss::Hook][jss::Flags] = 5;
        tx[jss::Hooks][0u][jss::Hook][jss::CreateCode] =
            strHex(XahauGenesis::AcceptHook);
        return tx;
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

            env(mint(env.master, {GenMint(bob.id(), XRP(123))}),
                ter(temDISABLED));
        }

        {
            test::jtx::Env env(*this, features - featureHooks);

            auto const alice = Account("alice");
            auto const bob = Account("bob");

            env.fund(XRP(10000), alice, bob);
            env.close();

            env(mint(env.master, {{bob.id(), XRP(123)}}), ter(temDISABLED));
        }
    }

    std::string
    makeBlob(std::vector<std::tuple<
                 std::optional<AccountID>,
                 std::optional<STAmount>,
                 std::optional<uint256>,
                 std::optional<uint256>>> entries)
    {
        std::string blob = "F060";

        for (auto const& [acc, amt, flags, marks] : entries)
        {
            STObject m(sfGenesisMint);
            if (acc)
                m.setAccountID(sfDestination, *acc);
            if (amt)
                m.setFieldAmount(sfAmount, *amt);
            if (flags)
                m.setFieldH256(sfGovernanceFlags, *flags);
            if (marks)
                m.setFieldH256(sfGovernanceMarks, *marks);

            Serializer s;
            m.add(s);
            blob += "E060" + strHex(s.getData()) + "E1";
        }

        blob += "F1";
        return blob;
    }

    Json::Value
    invoke(
        jtx::Account const& account,
        jtx::Account const& destination,
        std::string blob)
    {
        Json::Value tx = Json::objectValue;
        tx[jss::Account] = account.human();
        tx[jss::TransactionType] = "Invoke";
        tx[jss::Destination] = destination.human();
        tx[jss::Blob] = blob;
        return tx;
    }

    void
    testNonGenesisEmit(FeatureBitset features)
    {
        testcase("Non-Genesis Emit");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features, nullptr};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        env.fund(XRP(10000), alice, bob, invoker);
        env.close();

        // set the test hook
        env(setMintHook(alice), fee(XRP(10)));
        env.close();

        // this should fail because emitted txns are preflighted
        // and the preflight checks the account and will find it's not genesis
        env(invoke(
                invoker,
                alice,
                makeBlob({
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

        Env env{*this, envconfig(), features, nullptr};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        env.fund(XRP(10000), alice, bob, invoker);

        // burn down the total ledger coins so that genesis mints don't mint
        // above 100B tripping invariant
        env(burn(env.master), fee(XRP(10'000'000ULL)));
        env.close();

        // set the test hook
        env(setMintHook(env.master), fee(XRP(10)));
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

            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {bob.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)),
                ter(tesSUCCESS));

            env.close();
            env.close();

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

        env(invoke(
                invoker,
                env.master,
                makeBlob({
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
        env.close();
        env.close();

        {
            auto acc = env.le(keylet::account(carol.id()));
            BEAST_EXPECT(
                acc->getFieldAmount(sfBalance).xrp().drops() == 67890000000ULL);
            BEAST_EXPECT(acc->getFieldU32(sfSequence) == 50);
        }

        {
            auto acc = env.le(keylet::account(david.id()));
            BEAST_EXPECT(
                acc->getFieldAmount(sfBalance).xrp().drops() == 12345000000ULL);
            BEAST_EXPECT(acc->getFieldU32(sfSequence) == 50);
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

        env(invoke(invoker, env.master, makeBlob(mints)), fee(XRP(1)));

        env.close();
        env.close();

        for (auto const& [acc, amt, _, __] : mints)
        {
            auto const le = env.le(keylet::account(*acc));
            BEAST_EXPECT(!!le && le->getFieldAmount(sfBalance) == *amt);
        }

        // again, and check the amounts increased x2
        env(invoke(invoker, env.master, makeBlob(mints)), fee(XRP(1)));

        env.close();
        env.close();

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

        env(invoke(invoker, env.master, makeBlob(mints)),
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
        auto const gw = Account("gateway");
        auto const USD = gw["USD"];
        auto const edward = Account("edward");
        env(invoke(
                invoker,
                env.master,
                makeBlob(
                    {{edward.id(), USD(100), std::nullopt, std::nullopt}})),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        // zero xrp is allowed
        env(invoke(
                invoker,
                env.master,
                makeBlob({{edward.id(), XRP(0), std::nullopt, std::nullopt}})),
            fee(XRP(1)));

        // missing an amount
        env(invoke(
                invoker,
                env.master,
                makeBlob({
                    {edward.id(), std::nullopt, std::nullopt, std::nullopt},
                })),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        // missing a destination
        env(invoke(
                invoker,
                env.master,
                makeBlob({
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {alice.id(), std::nullopt, flags, std::nullopt},
                    })),
                fee(XRP(1)));

            env.close();
            env.close();
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {bob.id(), std::nullopt, std::nullopt, marks},
                    })),
                fee(XRP(1)));

            env.close();
            env.close();
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {fred.id(), XRP(589).value(), flags, marks},
                    })),
                fee(XRP(1)));

            env.close();
            env.close();
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob(
                        {{greg.id(),
                          XRP(0).value(),
                          std::nullopt,
                          std::nullopt}})),
                fee(XRP(1)));
            env.close();
            env.close();
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le && le->getFieldAmount(sfBalance).xrp().drops() == 0);
        }

        // try to mint negative
        {
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob(
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob(
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob(
                        {{greg.id(),
                          XRP(10).value(),
                          std::nullopt,
                          std::nullopt}})),
                fee(XRP(1)));
            env.close();
            env.close();
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
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {env.master.id(),
                         XRP(10).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)),
                ter(tesSUCCESS));
            env.close();
            env.close();
        }

        // try to include the same destination twice
        {
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
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
                ter(tecHOOK_REJECTED));
            env.close();
            env.close();
        }

        // check
        {
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le &&
                le->getFieldAmount(sfBalance).xrp().drops() == 10000000ULL);
        }

        // trip the supply cap invariant
        {
            auto const initCoins = env.current()->info().drops;
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {greg.id(),
                         XRP(9'999'999).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(1)));
            env.close();
            env.close();

            // check balance wasn't changed
            auto const le = env.le(keylet::account(greg.id()));
            BEAST_EXPECT(
                !!le &&
                le->getFieldAmount(sfBalance).xrp().drops() == 10000000ULL);

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

        Env env{*this, envconfig(), features, nullptr};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        env(mint(env.master, {GenMint(bob.id(), XRP(123))}), ter(temMALFORMED));

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

        Env env{*this, envconfig(), features, nullptr};
        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(10000), alice, bob);
        env.close();

        env(mint(alice, {GenMint(bob.id(), XRP(123))}), ter(temMALFORMED));

        auto const le = env.le(keylet::account(bob.id()));
        BEAST_EXPECT(
            !!le &&
            le->getFieldAmount(sfBalance).xrp().drops() == 10000000000ULL);
    }

    void
    testGenesisMintTSH(FeatureBitset features)
    {
        testcase("GenesisMint TSH");
        using namespace jtx;
        using namespace std::literals::chrono_literals;

        Env env{*this, envconfig(), features, nullptr};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const invoker = Account("invoker");
        env.fund(XRP(10000), alice, bob, invoker);

        // set tsh collect on bob
        env(fset(bob, asfTshCollect));

        // burn down the total ledger coins so that genesis mints don't mint
        // above 100B tripping invariant
        env(burn(env.master), fee(XRP(10'000'000ULL)));
        env.close();

        // set the test hook
        env(setMintHook(env.master), fee(XRP(10)));
        env.close();

        // set the accept hook
        env(setAcceptHook(bob), fee(XRP(10)));
        env.close();

        // test a mint
        {
            env(invoke(
                    invoker,
                    env.master,
                    makeBlob({
                        {bob.id(),
                         XRP(123).value(),
                         std::nullopt,
                         std::nullopt},
                    })),
                fee(XRP(10)),
                ter(tesSUCCESS));

            env.close();

            // get the emitted txn id
            Json::Value params;
            params[jss::transaction] =
                env.tx()->getJson(JsonOptions::none)[jss::hash];
            auto const jrr = env.rpc("json", "tx", to_string(params));
            auto const meta = jrr[jss::result][jss::meta];
            auto const emissions = meta[sfHookEmissions.jsonName];
            auto const emission = emissions[0u][sfHookEmission.jsonName];
            auto const txId = emission[sfEmittedTxnID.jsonName];

            // trigger the emitted txn
            env.close();

            // verify tsh hook triggered
            Json::Value params1;
            params1[jss::transaction] = txId;
            auto const jrr1 = env.rpc("json", "tx", to_string(params1));
            auto const meta1 = jrr1[jss::result][jss::meta];
            auto const executions = meta1[sfHookExecutions.jsonName];
            auto const execution = executions[0u][sfHookExecution.jsonName];
            BEAST_EXPECT(execution[sfHookResult.jsonName] == 3);
        }
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testDisabled(sa);
        testGenesisEmit(sa);
        testGenesisNonEmit(sa);
        testNonGenesisEmit(sa);
        testNonGenesisNonEmit(sa);
        testGenesisMintTSH(sa);
    }
};

BEAST_DEFINE_TESTSUITE(GenesisMint, app, ripple);

}  // namespace test
}  // namespace ripple
