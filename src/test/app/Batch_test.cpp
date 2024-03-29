//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2019 Ripple Labs Inc.

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

class Batch_test : public beast::unit_test::suite
{

    struct TestBatchData
    {
        std::string result;
        std::string txType;
    };

    void
    validateBatchTxns(
        Json::Value meta,
        std::array<TestBatchData, 2> batchResults)
    {
        size_t index = 0;
        for (auto const& _batchTxn : meta[sfBatchExecutions.jsonName])
        {
            auto const batchTxn = _batchTxn[sfBatchExecution.jsonName];
            BEAST_EXPECT(batchTxn[sfTransactionResult.jsonName] == batchResults[index].result);
            BEAST_EXPECT(batchTxn[sfTransactionType.jsonName] == batchResults[index].txType);
            ++index;
        }
    }
  
    Json::Value
    addBatchTx(
        Json::Value jv,
        Json::Value const& tx,
        jtx::Account const& account,
        XRPAmount feeDrops,
        std::uint32_t index,
        std::uint32_t next)
    {
        jv[sfRawTransactions.jsonName][index] = Json::Value{};
        jv[sfRawTransactions.jsonName][index][jss::RawTransaction] = tx;
        jv[sfRawTransactions.jsonName][index][jss::RawTransaction][jss::SigningPubKey] = strHex(account.pk());
        jv[sfRawTransactions.jsonName][index][jss::RawTransaction][sfFee.jsonName] = 0;
        jv[sfRawTransactions.jsonName][index][jss::RawTransaction][jss::Sequence] = 0;
        jv[sfRawTransactions.jsonName][index][jss::RawTransaction][sfOperationLimit.jsonName] = index;
        jv[sfRawTransactions.jsonName][index][jss::RawTransaction][sfCloseResolution.jsonName] = next;
        return jv;
    }

    // OnSucess -> (0)
    // OnFailure -> Next Tx Index
    // Ex.
    // 0: MintURIToken: If Fail -> 2
    // 1: Payment: If Fail -> 2
    // 2: Payment: 0
  
    void
    testBadPubKey(FeatureBitset features)
    {
        testcase("bad pubkey");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig()};
        // Env env{
        //     *this,
        //     envconfig(),
        //     features,
        //     nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace};


        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        env.fund(XRP(1000), alice, bob);
        env.close();

        auto const seq = env.seq("alice");
        // ttBATCH
        Json::Value jv;
        jv[jss::TransactionType] = jss::Batch;
        jv[jss::Account] = alice.human();

        // Batch Transactions
        jv[sfRawTransactions.jsonName] = Json::Value{Json::arrayValue};

        // Tx 1
        Json::Value const tx1 = pay(alice, bob, XRP(1));
        jv = addBatchTx(jv, tx1, alice, feeDrops, 0, 0);

        // Tx 2
        Json::Value const tx2 = pay(alice, bob, XRP(1));
        jv = addBatchTx(jv, tx2, bob, feeDrops, 1, 0);

        env(jv, fee(feeDrops * 2), ter(tesSUCCESS));
        env.close();

        std::array<TestBatchData, 2> testCases = {{
            {"tesSUCCESS", "Payment"},
            {"tesSUCCESS", "Payment"},
        }};

        Json::Value params;
        params[jss::transaction] = env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        std::cout << "jrr: " << jrr << "\n";
        auto const meta = jrr[jss::result][jss::meta];
        validateBatchTxns(meta, testCases);

        BEAST_EXPECT(env.seq(alice) == 2);
        BEAST_EXPECT(env.balance(alice) == XRP(1000) - XRP(1) - (feeDrops * 2));
        BEAST_EXPECT(env.balance(bob) == XRP(1000) + XRP(1));
    }
  
    void
    testUnfunded(FeatureBitset features)
    {
        testcase("unfunded");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig()};

        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        auto const seq = env.seq("alice");
        // ttBATCH
        Json::Value jv;
        jv[jss::TransactionType] = jss::Batch;
        jv[jss::Account] = alice.human();

        // Batch Transactions
        jv[sfRawTransactions.jsonName] = Json::Value{Json::arrayValue};

        // Tx 1
        Json::Value const tx1 = pay(alice, bob, XRP(1));
        jv = addBatchTx(jv, tx1, alice, feeDrops, 0, 0);

        // Tx 2
        Json::Value const tx2 = pay(alice, bob, XRP(999));
        jv = addBatchTx(jv, tx2, alice, feeDrops, 1, 0);

        env(jv, fee(feeDrops * 2), ter(tesSUCCESS));
        env.close();

        std::array<TestBatchData, 2> testCases = {{
            {"tesSUCCESS", "Payment"},
            {"tecUNFUNDED_PAYMENT", "Payment"},
        }};

        Json::Value params;
        params[jss::transaction] = env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        std::cout << "jrr: " << jrr << "\n";
        auto const meta = jrr[jss::result][jss::meta];
        validateBatchTxns(meta, testCases);

        BEAST_EXPECT(env.seq(alice) == 2);
        BEAST_EXPECT(env.balance(alice) == XRP(1000) - XRP(1) - (feeDrops * 1));
        BEAST_EXPECT(env.balance(bob) == XRP(1000) + XRP(1));
    }

    void
    testSuccess(FeatureBitset features)
    {
        testcase("batch success");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig()};
        // Env env{
        //     *this,
        //     envconfig(),
        //     features,
        //     nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace};

        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        auto const seq = env.seq("alice");
        // ttBATCH
        Json::Value jv;
        jv[jss::TransactionType] = jss::Batch;
        jv[jss::Account] = alice.human();

        // Batch Transactions
        jv[sfRawTransactions.jsonName] = Json::Value{Json::arrayValue};

        // Tx 1
        Json::Value const tx1 = pay(alice, bob, XRP(1));
        jv = addBatchTx(jv, tx1, alice, feeDrops, 0, 0);

        // Tx 2
        Json::Value const tx2 = pay(alice, bob, XRP(1));
        jv = addBatchTx(jv, tx2, alice, feeDrops, 1, 0);

        env(jv, fee(feeDrops * 2), ter(tesSUCCESS));
        env.close();

        std::array<TestBatchData, 2> testCases = {{
            {"tesSUCCESS", "Payment"},
            {"tesSUCCESS", "Payment"},
        }};

        Json::Value params;
        params[jss::transaction] = env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        std::cout << "jrr: " << jrr << "\n";
        auto const meta = jrr[jss::result][jss::meta];
        validateBatchTxns(meta, testCases);

        BEAST_EXPECT(env.seq(alice) == 2);
        BEAST_EXPECT(env.balance(alice) == XRP(1000) - XRP(2) - (feeDrops * 2));
        BEAST_EXPECT(env.balance(bob) == XRP(1000) + XRP(2));
    }

    void
    testSingle(FeatureBitset features)
    {
        testcase("batch single");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this, envconfig()};
        // Env env{
        //     *this,
        //     envconfig(),
        //     features,
        //     nullptr,
        //     // beast::severities::kWarning
        //     beast::severities::kTrace};

        auto const feeDrops = env.current()->fees().base;

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        auto const seq = env.seq("alice");
        // ttBATCH
        Json::Value jv;
        jv[jss::TransactionType] = jss::Batch;
        jv[jss::Account] = alice.human();
        jv[jss::Sequence] = seq;

        // Batch Transactions
        jv[sfRawTransactions.jsonName] = Json::Value{Json::arrayValue};

        // Tx 1
        jv[sfRawTransactions.jsonName][0U] = Json::Value{};
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction] = Json::Value{};
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction]
          [jss::TransactionType] = jss::Payment;
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction]
          [sfAccount.jsonName] = alice.human();
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction]
          [sfDestination.jsonName] = bob.human();
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction]
          [sfAmount.jsonName] = "1000000";
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction][sfFee.jsonName] =
            to_string(feeDrops);
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction][jss::Sequence] =
            seq + 0;
        jv[sfRawTransactions.jsonName][0U][jss::RawTransaction]
          [jss::SigningPubKey] = strHex(alice.pk());

        // Tx 2
        jv[sfRawTransactions.jsonName][1U] = Json::Value{};
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction] = Json::Value{};
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction]
          [jss::TransactionType] = jss::Payment;
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction]
          [sfAccount.jsonName] = alice.human();
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction]
          [sfDestination.jsonName] = bob.human();
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction]
          [sfAmount.jsonName] = "999900000";
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction][sfFee.jsonName] =
            to_string(feeDrops);
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction][jss::Sequence] =
            seq + 1;
        jv[sfRawTransactions.jsonName][1U][jss::RawTransaction]
          [jss::SigningPubKey] = strHex(alice.pk());

        env(jv, fee((feeDrops * 2)), ter(tesSUCCESS));
        env.close();

        std::array<TestBatchData, 2> testCases = {{
            {"tesSUCCESS", "Payment"},
            {"tecUNFUNDED_PAYMENT", "Payment"},
        }};

        Json::Value params;
        params[jss::transaction] = env.tx()->getJson(JsonOptions::none)[jss::hash];
        auto const jrr = env.rpc("json", "tx", to_string(params));
        std::cout << "jrr: " << jrr << "\n";
        auto const meta = jrr[jss::result][jss::meta];
        validateBatchTxns(meta, testCases);

        BEAST_EXPECT(env.seq(alice) == 4);
        BEAST_EXPECT(env.balance(alice) == XRP(1000) - XRP(1) - (feeDrops * 2));
        BEAST_EXPECT(env.balance(bob) == XRP(1000) + XRP(1));
    }

    void
    testInvalidBatch(FeatureBitset features)
    {
        testcase("invalid batch");

        using namespace test::jtx;
        using namespace std::literals;

        test::jtx::Env env{*this};

        auto const alice = Account("alice");
        auto const bob = Account("bob");
        auto const carol = Account("carol");
        env.fund(XRP(1000), alice, bob, carol);
        env.close();

        Json::Value jv;
        jv[jss::TransactionType] = jss::AccountSet;
        jv[jss::Account] = alice.human();
        jv[jss::Destination] = bob.human();
        jv[sfFee.jsonName] = "10";
        jv[sfCloseResolution.jsonName] = to_string(1);
        env(jv, fee(drops(10)), ter(tesSUCCESS));
        env.close();
    }

    void
    testWithFeats(FeatureBitset features)
    {
        // testBadPubKey(features);
        testUnfunded(features);
        // testSuccess(features);
        // testSingle(features);
        // testInvalidBatch(features);
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

BEAST_DEFINE_TESTSUITE(Batch, app, ripple);

}  // namespace test
}  // namespace ripple
