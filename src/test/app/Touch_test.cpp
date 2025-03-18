//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2025 XRPL-Labs

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  S
    OFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES  OF
    MERCHANTABILITY  AND  FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY  SPECIAL ,  DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER  RESULTING  FROM  LOSS  OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION  OF  CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <ripple/app/hook/Enum.h>
#include <ripple/app/misc/TxQ.h>
#include <ripple/app/tx/apply.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/PayChan.h>
#include <ripple/protocol/jss.h>
#include <test/app/Import_json.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

struct Touch_test : public beast::unit_test::suite
{
private:
    struct TestLedgerData
    {
        std::string txType;
        std::string result;
    };

    void
    validateTouch(
        jtx::Env& env,
        jtx::Account const& account,
        TestLedgerData const& testCase)
    {
        Json::Value params;
        params[jss::account] = account.human();
        params[jss::limit] = 1;
        params[jss::ledger_index_min] = -1;
        params[jss::ledger_index_max] = -1;
        auto const jrr = env.rpc("json", "account_tx", to_string(params));
        auto const transactions = jrr[jss::result][jss::transactions];
        BEAST_EXPECT(transactions.size() == 1);
        BEAST_EXPECT(
            transactions[0u][jss::tx][jss::TransactionType] == testCase.txType);
        BEAST_EXPECT(
            transactions[0u][jss::meta][sfTransactionResult.jsonName] ==
            testCase.result);
    }

    void
    testAccountSet(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("account set");

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // alice set
        env(fset(alice, asfDefaultRipple), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"AccountSet", "tesSUCCESS"});
    }

    void
    testAccountDelete(FeatureBitset features)
    {
        using namespace test::jtx;
        using namespace std::literals;
        testcase("account delete");

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // AccountDelete
        incLgrSeqForAccDel(env, alice);
        env(acctdelete(alice, bob),
            fee(env.current()->fees().reserve),
            ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"AccountDelete", "tesSUCCESS"});
        validateTouch(env, bob, {"AccountDelete", "tesSUCCESS"});
    }

    static uint256
    getCheckIndex(AccountID const& alice, std::uint32_t uSequence)
    {
        return keylet::check(alice, uSequence).key;
    }

    void
    testCheckCancel(FeatureBitset features)
    {
        testcase("check cancel");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // create check
        uint256 const checkId{getCheckIndex(alice, env.seq(alice))};
        env(check::create(alice, bob, XRP(100)), ter(tesSUCCESS));
        env.close();

        // cancel check
        env(check::cancel(alice, checkId), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"CheckCancel", "tesSUCCESS"});
        validateTouch(env, bob, {"CheckCancel", "tesSUCCESS"});
    }

    void
    testCheckCash(FeatureBitset features)
    {
        testcase("check cash");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // create check
        uint256 const checkId{getCheckIndex(alice, env.seq(alice))};
        env(check::create(alice, bob, XRP(100)), ter(tesSUCCESS));
        env.close();

        // cash check
        env(check::cash(bob, checkId, XRP(100)), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"CheckCash", "tesSUCCESS"});
        validateTouch(env, bob, {"CheckCash", "tesSUCCESS"});
    }

    void
    testCheckCreate(FeatureBitset features)
    {
        testcase("check create");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // create check
        env(check::create(alice, bob, XRP(100)), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"CheckCreate", "tesSUCCESS"});
        validateTouch(env, bob, {"CheckCreate", "tesSUCCESS"});
    }

    void
    testClaimReward(FeatureBitset features)
    {
        testcase("claim reward");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const issuer = Account("issuer");
        env.fund(XRP(1000), alice, issuer);
        env.close();

        // claim reward
        env(reward::claim(alice), reward::issuer(issuer), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"ClaimReward", "tesSUCCESS"});
        auto const tt = env.current()->rules().enabled(featureTouch)
            ? "ClaimReward"
            : "AccountSet";
        validateTouch(env, issuer, {tt, "tesSUCCESS"});
    }

    void
    testDepositPreauth(FeatureBitset features)
    {
        testcase("deposit preauth");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // require authorization for deposits.
        env(fset(alice, asfDepositAuth));

        // deposit preauth
        env(deposit::auth(alice, bob), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"DepositPreauth", "tesSUCCESS"});
        validateTouch(env, bob, {"DepositPreauth", "tesSUCCESS"});
    }

    void
    testEscrowCancel(FeatureBitset features)
    {
        testcase("escrow cancel");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // create escrow
            auto const seq1 = env.seq(alice);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(alice, bob, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrow::cancel(alice, alice, seq1), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"EscrowCancel", "tesSUCCESS"});
            validateTouch(env, bob, {"EscrowCancel", "tesSUCCESS"});
        }

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // create escrow
            auto const seq1 = env.seq(alice);
            NetClock::time_point const finishTime = env.now() + 1s;
            NetClock::time_point const cancelTime = env.now() + 2s;
            auto createTx = escrow::create(alice, bob, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            createTx[sfCancelAfter.jsonName] =
                cancelTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // cancel escrow
            env(escrow::cancel(bob, alice, seq1), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"EscrowCancel", "tesSUCCESS"});
            validateTouch(env, bob, {"EscrowCancel", "tesSUCCESS"});
        }
    }

    void
    testEscrowCreate(FeatureBitset features)
    {
        testcase("escrow create");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // create escrow
        NetClock::time_point const finishTime = env.now() + 1s;
        NetClock::time_point const cancelTime = env.now() + 2s;
        auto createTx = escrow::create(alice, bob, XRP(10));
        createTx[sfFinishAfter.jsonName] =
            finishTime.time_since_epoch().count();
        createTx[sfCancelAfter.jsonName] =
            cancelTime.time_since_epoch().count();
        env(createTx, ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"EscrowCreate", "tesSUCCESS"});
        validateTouch(env, bob, {"EscrowCreate", "tesSUCCESS"});
    }

    void
    testEscrowFinish(FeatureBitset features)
    {
        testcase("escrow finish");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // create escrow
            auto const seq1 = env.seq(alice);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(alice, bob, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrow::finish(alice, alice, seq1), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"EscrowFinish", "tesSUCCESS"});
            validateTouch(env, bob, {"EscrowFinish", "tesSUCCESS"});
        }

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            env.fund(XRP(1000), alice, bob);
            env.close();

            // create escrow
            auto const seq1 = env.seq(alice);
            NetClock::time_point const finishTime = env.now() + 1s;
            auto createTx = escrow::create(alice, bob, XRP(10));
            createTx[sfFinishAfter.jsonName] =
                finishTime.time_since_epoch().count();
            env(createTx, ter(tesSUCCESS));
            env.close();

            // finish escrow
            env(escrow::finish(bob, alice, seq1), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"EscrowFinish", "tesSUCCESS"});
            validateTouch(env, bob, {"EscrowFinish", "tesSUCCESS"});
        }
    }

    void
    testGenesisMint(FeatureBitset features)
    {
        testcase("genesis mint");

        using namespace jtx;
        using namespace std::chrono;
        using namespace std::literals;

        test::jtx::Env env{
            *this,
            network::makeNetworkConfig(21337, "10", "1000000", "200000"),
            features};

        auto const alice = Account("alice");
        auto const issuer = env.master;
        auto const bene = Account("bob");
        env.fund(XRP(1000), alice, bene);
        env.close();

        // burn down the total ledger coins so that genesis mints don't mint
        // above 100B tripping invariant
        env(noop(issuer), fee(XRP(10'000'000ULL)));
        env.close();

        // set mint hook on master
        env(hook(issuer, {{hso(genesis::MintTestHook, overrideFlag)}}, 0),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        env(invoke::invoke(
                alice,
                issuer,
                genesis::makeBlob({
                    {bene.id(), XRP(123).value(), std::nullopt, std::nullopt},
                })),
            fee(XRP(10)),
            ter(tesSUCCESS));
        env.close();
        env.close();

        // verify touch
        validateTouch(env, alice, {"Invoke", "tesSUCCESS"});
        validateTouch(env, issuer, {"GenesisMint", "tesSUCCESS"});
        validateTouch(env, bene, {"GenesisMint", "tesSUCCESS"});
    }

    void
    testImport(FeatureBitset features)
    {
        testcase("import");

        using namespace test::jtx;
        using namespace std::literals;

        std::vector<std::string> const keys = {
            "ED74D4036C6591A4BDF9C54CEFA39B996A5DCE5F86D11FDA1874481CE9D5A1CDC"
            "1"};

        test::jtx::Env env{
            *this,
            network::makeNetworkVLConfig(
                21337, keys, "10", "1000000", "200000"),
            features};

        auto const alice = Account("alice");
        auto const issuer = Account("bob");
        env.fund(XRP(1000), alice, issuer);
        env.close();

        // burn down the total ledger coins so that genesis mints don't mint
        // above 100B tripping invariant
        env(noop(env.master), fee(XRP(10'000'000ULL)));
        env.close();

        // import
        env(import::import(alice, import::loadXpop(ImportTCAccountSet::w_seed)),
            import::issuer(issuer),
            fee(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"Import", "tesSUCCESS"});
        auto const tt = env.current()->rules().enabled(featureTouch)
            ? "Import"
            : "AccountSet";
        validateTouch(env, issuer, {tt, "tesSUCCESS"});
    }

    void
    testInvoke(FeatureBitset features)
    {
        testcase("invoke");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        // ttINVOKE
        env(invoke::invoke(alice), invoke::dest(bob), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"Invoke", "tesSUCCESS"});
        auto const tt = env.current()->rules().enabled(featureTouch)
            ? "Invoke"
            : "AccountSet";
        validateTouch(env, bob, {tt, "tesSUCCESS"});
    }

    void
    testOfferCancel(FeatureBitset features)
    {
        testcase("offer cancel");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(1000), alice, gw);
        env.close();

        // gw create offer
        env(offer(gw, USD(1000), XRP(1000)));
        env.close();

        // create offer
        auto const offerSeq = env.seq(alice);
        env(offer(alice, USD(1000), XRP(1000)), ter(tesSUCCESS));
        env.close();

        // cancel offer
        env(offer_cancel(alice, offerSeq), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"OfferCancel", "tesSUCCESS"});
    }

    void
    testOfferCreate(FeatureBitset features)
    {
        testcase("offer create");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const gw = Account{"gateway"};
        auto const USD = gw["USD"];
        env.fund(XRP(1000), alice, gw);
        env.close();

        // gw create offer
        env(offer(gw, USD(1000), XRP(1000)));
        env.close();

        // create offer
        env(offer(alice, USD(1000), XRP(1000)), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, gw, {"OfferCreate", "tesSUCCESS"});
        validateTouch(env, alice, {"OfferCreate", "tesSUCCESS"});
    }

    void
    testPayment(FeatureBitset features)
    {
        testcase("payment");

        using namespace test::jtx;
        using namespace std::literals;

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            env.fund(XRP(1000), alice, bob);
            env.close();

            // payment
            env(pay(alice, bob, XRP(1)), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Payment", "tesSUCCESS"});
            validateTouch(env, bob, {"Payment", "tesSUCCESS"});
        }

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            auto const gw = Account{"gw"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env(trust(alice, USD(100)), ter(tesSUCCESS));
            env(trust(bob, USD(100)), ter(tesSUCCESS));
            env.close();
            env(pay(gw, alice, USD(100)), ter(tesSUCCESS));
            env.close();

            // payment
            env(pay(alice, bob, USD(1)), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Payment", "tesSUCCESS"});
            validateTouch(env, bob, {"Payment", "tesSUCCESS"});
            validateTouch(env, gw, {"Payment", "tesSUCCESS"});
        }

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account("bob");
            auto const carol = Account("carol");
            env.fund(XRP(1000), alice, bob, carol);
            env.close();

            // setup rippling
            auto const USDA = alice["USD"];
            auto const USDB = bob["USD"];
            auto const USDC = carol["USD"];
            env.trust(USDA(10), bob);
            env.trust(USDB(10), carol);

            // payment
            env(pay(alice, carol, USDB(10)), paths(USDA));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Payment", "tesSUCCESS"});
            validateTouch(env, bob, {"Payment", "tesSUCCESS"});
            validateTouch(env, carol, {"Payment", "tesSUCCESS"});
        }
    }

    static uint256
    channel(
        jtx::Account const& alice,
        jtx::Account const& dst,
        std::uint32_t seqProxyValue)
    {
        auto const k = keylet::payChan(alice, dst, seqProxyValue);
        return k.key;
    }

    static Buffer
    signClaimAuth(
        PublicKey const& pk,
        SecretKey const& sk,
        uint256 const& channel,
        STAmount const& authAmt)
    {
        Serializer msg;
        serializePayChanAuthorization(msg, channel, authAmt.xrp());
        return sign(pk, sk, msg.slice());
    }

    void
    testPaymentChannelClaim(FeatureBitset features)
    {
        testcase("payment channel claim");

        using namespace test::jtx;
        using namespace std::literals;

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            env.fund(XRP(1000), alice, bob);
            env.close();

            // create paychannel
            auto const pk = alice.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(alice, bob, env.seq(alice));
            env(paychan::create(alice, bob, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            env(paychan::claim(alice, chan, reqBal, authAmt),
                txflags(tfClose),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"PaymentChannelClaim", "tesSUCCESS"});
            validateTouch(env, bob, {"PaymentChannelClaim", "tesSUCCESS"});
        }

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            env.fund(XRP(1000), alice, bob);
            env.close();

            // create paychannel
            auto const pk = alice.pk();
            auto const settleDelay = 100s;
            auto const chan = channel(alice, bob, env.seq(alice));
            env(paychan::create(alice, bob, XRP(10), settleDelay, pk),
                ter(tesSUCCESS));
            env.close();

            auto const delta = XRP(1);
            auto const reqBal = delta;
            auto const authAmt = reqBal + XRP(1);

            // claim paychannel
            auto const sig =
                signClaimAuth(alice.pk(), alice.sk(), chan, authAmt);
            env(paychan::claim(
                    bob, chan, reqBal, authAmt, Slice(sig), alice.pk()),
                txflags(tfClose),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"PaymentChannelClaim", "tesSUCCESS"});
            validateTouch(env, bob, {"PaymentChannelClaim", "tesSUCCESS"});
        }
    }

    void
    testPaymentChannelCreate(FeatureBitset features)
    {
        testcase("payment channel create");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        // create paychannel
        auto const pk = alice.pk();
        auto const settleDelay = 100s;
        env(paychan::create(alice, bob, XRP(10), settleDelay, pk),
            ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"PaymentChannelCreate", "tesSUCCESS"});
        validateTouch(env, bob, {"PaymentChannelCreate", "tesSUCCESS"});
    }

    void
    testPaymentChannelFund(FeatureBitset features)
    {
        testcase("payment channel fund");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        // create paychannel
        auto const pk = alice.pk();
        auto const settleDelay = 100s;
        auto const chan = channel(alice, bob, env.seq(alice));
        env(paychan::create(alice, bob, XRP(10), settleDelay, pk),
            ter(tesSUCCESS));
        env.close();

        // fund paychannel
        env(paychan::fund(alice, chan, XRP(1)), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"PaymentChannelFund", "tesSUCCESS"});
    }

    // helper
    void static overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }

    void
    testSetHook(FeatureBitset features)
    {
        testcase("set hook");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // set tsh hook
        auto hook1 = hso(jtx::genesis::AcceptHook, overrideFlag);
        hook1[jss::HookOn] =
            "00000000000000000000000000000000000000000000000000000000004000"
            "00";
        env(hook(alice, {{hook1}}, 0), fee(XRP(1)), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"SetHook", "tesSUCCESS"});
    }

    void
    testSetRegularKey(FeatureBitset features)
    {
        testcase("set regular key");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const bob = Account{"bob"};
        env.fund(XRP(1000), alice, bob);
        env.close();

        // set regular key
        env(regkey(alice, bob), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"SetRegularKey", "tesSUCCESS"});
        validateTouch(env, bob, {"SetRegularKey", "tesSUCCESS"});
    }

    void
    testSignersListSet(FeatureBitset features)
    {
        testcase("signers list set");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const signer1 = Account{"bob"};
        auto const signer2 = Account{"carol"};
        env.fund(XRP(1000), alice, signer1, signer2);
        env.close();

        // signers list set
        env(signers(alice, 2, {{signer1, 1}, {signer2, 1}}), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"SignerListSet", "tesSUCCESS"});
        auto const tt = env.current()->rules().enabled(featureTouch)
            ? "SignerListSet"
            : "AccountSet";
        validateTouch(env, signer1, {tt, "tesSUCCESS"});
        validateTouch(env, signer2, {tt, "tesSUCCESS"});
    }

    void
    testTicketCreate(FeatureBitset features)
    {
        testcase("ticket create");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        env.fund(XRP(1000), alice);
        env.close();

        // ticket create
        env(ticket::create(alice, 2), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"TicketCreate", "tesSUCCESS"});
    }

    void
    testTrustSet(FeatureBitset features)
    {
        testcase("trust set");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const alice = Account("alice");
        auto const issuer = Account{"gw"};
        auto const USD = issuer["USD"];
        env.fund(XRP(1000), alice, issuer);
        env.close();

        // trust set
        env(trust(alice, USD(1000)), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, alice, {"TrustSet", "tesSUCCESS"});
        validateTouch(env, issuer, {"TrustSet", "tesSUCCESS"});
    }

    void
    testURITokenMint(FeatureBitset features)
    {
        testcase("uritoken mint");

        using namespace test::jtx;
        using namespace std::literals;

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, issuer, {"URITokenMint", "tesSUCCESS"});
        }

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const issuer = Account("alice");
            auto const buyer = Account("carol");
            env.fund(XRP(1000), issuer, buyer);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(buyer),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, buyer, {"URITokenMint", "tesSUCCESS"});
            validateTouch(env, issuer, {"URITokenMint", "tesSUCCESS"});
        }
    }

    void
    testURITokenBurn(FeatureBitset features)
    {
        testcase("uritoken burn");

        using namespace test::jtx;
        using namespace std::literals;

        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(owner, hexid), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, owner, {"URITokenBurn", "tesSUCCESS"});
            validateTouch(env, issuer, {"URITokenBurn", "tesSUCCESS"});
        }

        // Issuer
        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const issuer = Account("alice");
            auto const owner = Account("bob");
            env.fund(XRP(1000), issuer, owner);
            env.close();

            std::string const uri(2, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            std::string const hexid{strHex(tid)};

            // mint uritoken
            env(uritoken::mint(issuer, uri),
                uritoken::dest(owner),
                uritoken::amt(XRP(1)),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(owner, hexid),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // ttURITOKEN_BURN
            env(uritoken::burn(issuer, hexid), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, owner, {"URITokenBurn", "tesSUCCESS"});
            validateTouch(env, issuer, {"URITokenBurn", "tesSUCCESS"});
        }
    }

    void
    testURITokenBuy(FeatureBitset features)
    {
        testcase("uritoken buy");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const issuer = Account("alice");
        auto const owner = Account("bob");
        auto const buyer = Account("carol");
        env.fund(XRP(1000), issuer, owner, buyer);
        env.close();

        std::string const uri(2, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};

        // mint uritoken
        env(uritoken::mint(issuer, uri),
            uritoken::dest(owner),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // buy uritoken
        env(uritoken::buy(owner, hexid),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // sell uritoken
        env(uritoken::sell(owner, hexid),
            uritoken::dest(buyer),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // buy uritoken
        env(uritoken::buy(buyer, hexid),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, buyer, {"URITokenBuy", "tesSUCCESS"});
        validateTouch(env, issuer, {"URITokenBuy", "tesSUCCESS"});
    }

    void
    testURITokenCancelSellOffer(FeatureBitset features)
    {
        testcase("uritoken cancel sell offer");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const issuer = Account("alice");
        auto const owner = Account("bob");
        auto const buyer = Account("carol");
        env.fund(XRP(1000), issuer, owner, buyer);
        env.close();

        std::string const uri(2, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};

        // mint uritoken
        env(uritoken::mint(issuer, uri),
            uritoken::dest(owner),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // buy uritoken
        env(uritoken::buy(owner, hexid),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // sell uritoken
        env(uritoken::sell(owner, hexid),
            uritoken::dest(buyer),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // cancel uritoken
        env(uritoken::cancel(owner, hexid), ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, owner, {"URITokenCancelSellOffer", "tesSUCCESS"});
        validateTouch(env, issuer, {"URITokenCancelSellOffer", "tesSUCCESS"});
    }

    void
    testURITokenCreateSellOffer(FeatureBitset features)
    {
        testcase("uritoken create sell offer");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig(), features};

        auto const issuer = Account("alice");
        auto const owner = Account("bob");
        auto const buyer = Account("carol");
        env.fund(XRP(1000), issuer, owner, buyer);
        env.close();

        std::string const uri(2, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};

        // mint uritoken
        env(uritoken::mint(issuer, uri),
            uritoken::dest(owner),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // buy uritoken
        env(uritoken::buy(owner, hexid),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // sell uritoken
        env(uritoken::sell(owner, hexid),
            uritoken::dest(buyer),
            uritoken::amt(XRP(1)),
            ter(tesSUCCESS));
        env.close();

        // verify touch
        validateTouch(env, owner, {"URITokenCreateSellOffer", "tesSUCCESS"});
        validateTouch(env, buyer, {"URITokenCreateSellOffer", "tesSUCCESS"});
        validateTouch(env, issuer, {"URITokenCreateSellOffer", "tesSUCCESS"});
    }

    void
    testRemit(FeatureBitset features)
    {
        testcase("remit");

        using namespace test::jtx;
        using namespace std::literals;

        // No Amount
        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            env.fund(XRP(1000), alice, bob);
            env.close();

            // remit
            env(remit::remit(alice, bob), ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Remit", "tesSUCCESS"});
            auto const tt = env.current()->rules().enabled(featureTouch)
                ? "Remit"
                : "AccountSet";
            validateTouch(env, bob, {tt, "tesSUCCESS"});
        }

        // IOU
        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            auto const gw = Account{"gw"};
            auto const USD = gw["USD"];
            env.fund(XRP(1000), alice, bob, gw);
            env.close();
            env(trust(alice, USD(100)), ter(tesSUCCESS));
            env(trust(bob, USD(100)), ter(tesSUCCESS));
            env.close();
            env(pay(gw, alice, USD(100)), ter(tesSUCCESS));
            env.close();

            // remit
            env(remit::remit(alice, bob),
                remit::amts({USD(1)}),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Remit", "tesSUCCESS"});
            validateTouch(env, bob, {"Remit", "tesSUCCESS"});
            validateTouch(env, gw, {"Remit", "tesSUCCESS"});
        }

        // Inform
        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            auto const inform = Account{"inform"};
            env.fund(XRP(1000), alice, bob, inform);
            env.close();

            // remit
            env(remit::remit(alice, bob),
                remit::inform(inform),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Remit", "tesSUCCESS"});
            auto const tt = env.current()->rules().enabled(featureTouch)
                ? "Remit"
                : "AccountSet";
            validateTouch(env, bob, {tt, "tesSUCCESS"});
            validateTouch(env, inform, {tt, "tesSUCCESS"});
        }

        // URITokenIDs
        {
            test::jtx::Env env{*this, envconfig(), features};

            auto const alice = Account("alice");
            auto const bob = Account{"bob"};
            auto const issuer = Account{"issuer"};
            env.fund(XRP(1000), alice, bob, issuer);
            env.close();

            // mint uritoken
            std::string const uri(maxTokenURILength, '?');
            auto const tid = uritoken::tokenid(issuer, uri);
            env(uritoken::mint(issuer, uri),
                txflags(tfBurnable),
                ter(tesSUCCESS));

            // sell uritoken
            env(uritoken::sell(issuer, strHex(tid)),
                uritoken::amt(XRP(1)),
                uritoken::dest(alice),
                ter(tesSUCCESS));
            env.close();

            // buy uritoken
            env(uritoken::buy(alice, strHex(tid)),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            // remit
            env(remit::remit(alice, bob),
                remit::token_ids({strHex(tid)}),
                ter(tesSUCCESS));
            env.close();

            // verify touch
            validateTouch(env, alice, {"Remit", "tesSUCCESS"});
            validateTouch(env, bob, {"Remit", "tesSUCCESS"});
            validateTouch(env, issuer, {"Remit", "tesSUCCESS"});
        }
    }

    void
    testAllTxns(FeatureBitset features)
    {
        testAccountSet(features);
        testAccountDelete(features);
        testCheckCancel(features);
        testCheckCash(features);
        testCheckCreate(features);
        testClaimReward(features);
        testDepositPreauth(features);
        testEscrowCancel(features);
        testEscrowCreate(features);
        testEscrowFinish(features);
        testGenesisMint(features);
        testImport(features);
        testInvoke(features);
        testOfferCancel(features);
        testOfferCreate(features);
        testPayment(features);
        testPaymentChannelClaim(features);
        testPaymentChannelCreate(features);
        testPaymentChannelFund(features);
        testSetHook(features);
        testSetRegularKey(features);
        testSignersListSet(features);
        testTicketCreate(features);
        testTrustSet(features);
        testURITokenMint(features);
        testURITokenBurn(features);
        testURITokenBuy(features);
        testURITokenCancelSellOffer(features);
        testURITokenCreateSellOffer(features);
        testRemit(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testAllTxns(sa - featureTouch);
        testAllTxns(sa);
    }
};

BEAST_DEFINE_TESTSUITE(Touch, app, ripple);

}  // namespace test
}  // namespace ripple