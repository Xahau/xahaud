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
#include <test/app/Import_json.h>
#include <test/jtx.h>
#include <test/jtx/TestHelpers.h>

namespace ripple {
namespace test {

class BaseFee_test : public beast::unit_test::suite
{
    Json::Value
    addTxParams(Json::Value jv)
    {
        jv[jss::HookParameters] = Json::Value{Json::arrayValue};
        jv[jss::HookParameters][0U] = Json::Value{};
        jv[jss::HookParameters][0U][jss::HookParameter] = Json::Value{};
        jv[jss::HookParameters][0U][jss::HookParameter]
          [jss::HookParameterName] = "CAFE";
        jv[jss::HookParameters][0U][jss::HookParameter]
          [jss::HookParameterValue] = "DEADBEEF";
        return jv;
    }

    void
    testRPCCall(jtx::Env& env, Json::Value tx, std::string expected)
    {
        auto const jv = addTxParams(tx);
        auto const jtx = env.jt(jv);

        auto const feeDrops = env.current()->fees().base;

        // build tx_blob
        Json::Value params;
        params[jss::tx_blob] = strHex(jtx.stx->getSerializer().slice());

        // fee request
        auto const jrr = env.rpc("json", "fee", to_string(params));
        // std::cout << "RESULT: " << jrr << "\n";

        // verify base fee & open ledger fee
        auto const drops = jrr[jss::result][jss::drops];
        auto const baseFee = drops[jss::base_fee_no_hooks];
        BEAST_EXPECT(baseFee == to_string(feeDrops));
        auto const openLedgerFee = drops[jss::open_ledger_fee];
        BEAST_EXPECT(openLedgerFee == expected);

        // verify hooks fee
        auto const hooksFee = jrr[jss::result][jss::fee_hooks_feeunits];
        BEAST_EXPECT(hooksFee == expected);
    }

    void
    testAccountSet(FeatureBitset features)
    {
        testcase("account set w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = fset(account, asfTshCollect);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testAccountDelete(FeatureBitset features)
    {
        testcase("account delete w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const bene = Account("bob");
        env.fund(XRP(1000), account, bene);
        env.close();

        // build tx
        auto tx = acctdelete(account, bene);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "200000" : "200000";
        testRPCCall(env, tx, feeResult);
    }

    static uint256
    getCheckIndex(AccountID const& account, std::uint32_t uSequence)
    {
        return keylet::check(account, uSequence).key;
    }

    void
    testCheckCancel(FeatureBitset features)
    {
        testcase("check cancel w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        uint256 const checkId{getCheckIndex(account, env.seq(account))};
        auto tx = check::cancel(account, checkId);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testCheckCash(FeatureBitset features)
    {
        testcase("check cash w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        uint256 const checkId{getCheckIndex(account, env.seq(account))};
        auto tx = check::cash(dest, checkId, XRP(100));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testCheckCreate(FeatureBitset features)
    {
        testcase("check create w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto tx = check::create(account, dest, XRP(100));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testClaimReward(FeatureBitset features)
    {
        testcase("claim reward w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = reward::claim(account);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testDepositPreauth(FeatureBitset features)
    {
        testcase("deposit preauth w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const authed = Account("bob");
        env.fund(XRP(1000), account, authed);
        env.close();

        // build tx
        auto tx = deposit::auth(account, authed);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testEscrowCancel(FeatureBitset features)
    {
        testcase("escrow cancel w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto const seq1 = env.seq(account);
        auto tx = cancel(account, account, seq1);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testEscrowCreate(FeatureBitset features)
    {
        testcase("escrow create w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto tx = escrow(account, dest, XRP(10));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testEscrowFinish(FeatureBitset features)
    {
        testcase("escrow finish w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto const seq1 = env.seq(account);
        auto tx = finish(account, account, seq1);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testImport(FeatureBitset features)
    {
        testcase("import w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = import::import(
            account, import::loadXpop(ImportTCAccountSet::w_seed));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "106" : "100";
        testRPCCall(env, tx, feeResult);
    }

    void
    testInvoke(FeatureBitset features)
    {
        testcase("invoke w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = invoke::invoke(account);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "16";
        testRPCCall(env, tx, feeResult);
    }

    void
    testOfferCancel(FeatureBitset features)
    {
        testcase("offer cancel w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto const offerSeq = env.seq(account);
        auto tx = offer_cancel(account, offerSeq);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testOfferCreate(FeatureBitset features)
    {
        testcase("offer create w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = offer(account, USD(1000), XRP(1000));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testPayment(FeatureBitset features)
    {
        testcase("payment w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto tx = pay(account, dest, XRP(1));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    static uint256
    channel(
        jtx::Account const& account,
        jtx::Account const& dst,
        std::uint32_t seqProxyValue)
    {
        auto const k = keylet::payChan(account, dst, seqProxyValue);
        return k.key;
    }

    void
    testPaymentChannelClaim(FeatureBitset features)
    {
        testcase("payment channel claim w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto const chan = channel(account, dest, env.seq(account));
        auto const delta = XRP(1);
        auto const reqBal = delta;
        auto const authAmt = reqBal + XRP(1);
        auto tx = paychan::claim(account, chan, reqBal, authAmt);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testPaymentChannelCreate(FeatureBitset features)
    {
        testcase("payment channel create w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto const pk = account.pk();
        auto const settleDelay = 100s;
        auto tx = paychan::create(account, dest, XRP(10), settleDelay, pk);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testPaymentChannelFund(FeatureBitset features)
    {
        testcase("payment channel fund w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto const chan = channel(account, dest, env.seq(account));
        auto tx = paychan::fund(account, chan, XRP(1));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testSetHook(FeatureBitset features)
    {
        testcase("set hook w/ hook params and otxn params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = genesis::setAcceptHook(account);

        // add hook params
        tx[jss::Hooks][0u][jss::Hook][jss::HookParameters] =
            Json::Value{Json::arrayValue};
        tx[jss::Hooks][0u][jss::Hook][jss::HookParameters][0U] = Json::Value{};
        Json::Value& hookParams =
            tx[jss::Hooks][0u][jss::Hook][jss::HookParameters][0U];
        hookParams[jss::HookParameter] = Json::Value{};
        hookParams[jss::HookParameter][jss::HookParameterName] = "CAFE";
        hookParams[jss::HookParameter][jss::HookParameterValue] = "DEADBEEF";

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "73022" : "73016";
        testRPCCall(env, tx, feeResult);
    }

    void
    testSetRegularKey(FeatureBitset features)
    {
        testcase("set regular key w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const dest = Account("bob");
        env.fund(XRP(1000), account, dest);
        env.close();

        // build tx
        auto tx = regkey(account, dest);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "0" : "0";
        testRPCCall(env, tx, feeResult);
    }

    void
    testSignersListSet(FeatureBitset features)
    {
        testcase("signers list set w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const signer1 = Account("bob");
        auto const signer2 = Account("carol");
        env.fund(XRP(1000), account, signer1, signer2);
        env.close();

        // build tx
        auto tx = signers(account, 2, {{signer1, 1}, {signer2, 1}});

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testTicketCreate(FeatureBitset features)
    {
        testcase("ticket create w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        auto tx = ticket::create(account, 2);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testTrustSet(FeatureBitset features)
    {
        testcase("trust set w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        auto const gw = Account("gw");
        auto const USD = gw["USD"];
        env.fund(XRP(1000), account, gw);
        env.close();

        // build tx
        auto tx = trust(account, USD(1000));

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testURITokenBurnFee(FeatureBitset features)
    {
        testcase("uritoken burn w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const issuer = Account("alice");
        env.fund(XRP(1000), issuer);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};
        auto tx = uritoken::burn(issuer, hexid);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testURITokenBuyFee(FeatureBitset features)
    {
        testcase("uritoken buy w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const issuer = Account("alice");
        env.fund(XRP(1000), issuer);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};
        auto tx = uritoken::buy(issuer, hexid);
        tx[jss::Amount] = "1000000";

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testURITokenCancelSellOfferFee(FeatureBitset features)
    {
        testcase("uritoken cancel sell offer w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const issuer = Account("alice");
        env.fund(XRP(1000), issuer);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};
        auto tx = uritoken::cancel(issuer, hexid);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testURITokenCreateSellOfferFee(FeatureBitset features)
    {
        testcase("uritoken create sell offer w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const issuer = Account("alice");
        auto const buyer = Account("bob");
        env.fund(XRP(1000), issuer, buyer);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto const tid = uritoken::tokenid(issuer, uri);
        std::string const hexid{strHex(tid)};
        auto tx = uritoken::sell(issuer, hexid);
        tx[jss::Destination] = buyer.human();
        tx[jss::Amount] = "1000000";

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testURITokenMintFee(FeatureBitset features)
    {
        testcase("uritoken mint w/ hook params");

        using namespace test::jtx;
        using namespace std::literals;

        Env env{*this, network::makeNetworkConfig(21337), features};

        auto const account = Account("alice");
        env.fund(XRP(1000), account);
        env.close();

        // build tx
        std::string const uri(maxTokenURILength, '?');
        auto tx = uritoken::mint(account, uri);

        // verify hooks fee
        std::string const feeResult =
            env.current()->rules().enabled(fixXahauV1) ? "16" : "10";
        testRPCCall(env, tx, feeResult);
    }

    void
    testWithFeats(FeatureBitset features)
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
        // testGenesisMint(features); // N/A
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
        testURITokenBurnFee(features);
        testURITokenBuyFee(features);
        testURITokenCancelSellOfferFee(features);
        testURITokenCreateSellOfferFee(features);
        testURITokenMintFee(features);
    }

public:
    void
    run() override
    {
        using namespace test::jtx;
        auto const sa = supported_amendments();
        testWithFeats(sa);
        testWithFeats(sa - fixXahauV1);
    }
};

BEAST_DEFINE_TESTSUITE(BaseFee, app, ripple);

}  // namespace test
}  // namespace ripple
