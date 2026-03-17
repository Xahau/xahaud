//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL Labs

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

#include <test/app/Export_test_hooks.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/jss.h>

#include <map>

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

// Large fee for hook operations
#define HSFEE fee(100'000'000)

//------------------------------------------------------------------------------
// Per-partition debug logging for tests
// Usage:
//   auto logs = std::make_unique<DebugLogs>(*this, DebugLogs::Levels{
//       {"View", kTrace},    // Hook operations
//       {"TxQ", kDebug},     // Transaction queue
//   });
//   Env env{*this, envconfig(), features, std::move(logs), kError};
//------------------------------------------------------------------------------
class DebugLogs : public Logs
{
public:
    using Levels = std::map<std::string, beast::severities::Severity>;

private:
    beast::unit_test::suite& suite_;
    Levels levels_;

public:
    DebugLogs(beast::unit_test::suite& suite, Levels levels = {})
        : Logs(beast::severities::kError)
        , suite_(suite)
        , levels_(std::move(levels))
    {
    }

    std::unique_ptr<beast::Journal::Sink>
    makeSink(
        std::string const& partition,
        beast::severities::Severity defaultThresh) override
    {
        auto thresh = defaultThresh;
        if (auto it = levels_.find(partition); it != levels_.end())
            thresh = it->second;
        return std::make_unique<SuiteJournalSink>(partition, thresh, suite_);
    }
};

struct Export_test : public beast::unit_test::suite
{
    static std::unique_ptr<Config>
    exportTestConfig()
    {
        auto cfg = jtx::envconfig(jtx::validator, "");
        cfg->NETWORK_ID = 21337;
        return cfg;
    }

    // Hook that exports a payment using xport (for cross-chain export)
    // xport APIs are gated by featureExport amendment, not sfHookApiVersion
    TestHook xport_wasm = export_test_wasm[R"[test.hook](
        #include <stdint.h>
        extern int32_t _g(uint32_t id, uint32_t maxiter);
        extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len);
        extern int64_t xport_reserve(uint32_t count);
        extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
        extern int64_t otxn_param(uint32_t write_ptr, uint32_t write_len, uint32_t name_ptr, uint32_t name_len);
        extern int64_t otxn_type(void);
        extern int64_t ledger_seq(void);

        #define SBUF(x) (uint32_t)(x), sizeof(x)
        #define ASSERT(x) if (!(x)) rollback((uint32_t)#x, sizeof(#x), __LINE__)

        #define ttPAYMENT 0
        #define tfCANONICAL 0x80000000UL

        #define amAMOUNT 1
        #define amFEE 8
        #define atACCOUNT 1
        #define atDESTINATION 3

        #define ENCODE_TT(buf_out, tt) \
            buf_out[0] = 0x12U; \
            buf_out[1] = (tt >> 8) & 0xFFU; \
            buf_out[2] = tt & 0xFFU; \
            buf_out += 3;

        #define ENCODE_FLAGS(buf_out, flags) \
            buf_out[0] = 0x22U; \
            buf_out[1] = (flags >> 24) & 0xFFU; \
            buf_out[2] = (flags >> 16) & 0xFFU; \
            buf_out[3] = (flags >> 8) & 0xFFU; \
            buf_out[4] = flags & 0xFFU; \
            buf_out += 5;

        #define ENCODE_SEQUENCE(buf_out, seq) \
            buf_out[0] = 0x24U; \
            buf_out[1] = (seq >> 24) & 0xFFU; \
            buf_out[2] = (seq >> 16) & 0xFFU; \
            buf_out[3] = (seq >> 8) & 0xFFU; \
            buf_out[4] = seq & 0xFFU; \
            buf_out += 5;

        #define ENCODE_FLS(buf_out, fls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1AU; \
            buf_out[2] = (fls >> 24) & 0xFFU; \
            buf_out[3] = (fls >> 16) & 0xFFU; \
            buf_out[4] = (fls >> 8) & 0xFFU; \
            buf_out[5] = fls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_LLS(buf_out, lls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1BU; \
            buf_out[2] = (lls >> 24) & 0xFFU; \
            buf_out[3] = (lls >> 16) & 0xFFU; \
            buf_out[4] = (lls >> 8) & 0xFFU; \
            buf_out[5] = lls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_DROPS(buf_out, drops, amt_type) \
            buf_out[0] = 0x60U + amt_type; \
            buf_out[1] = 0x40U + ((drops >> 56) & 0x3FU); \
            buf_out[2] = (drops >> 48) & 0xFFU; \
            buf_out[3] = (drops >> 40) & 0xFFU; \
            buf_out[4] = (drops >> 32) & 0xFFU; \
            buf_out[5] = (drops >> 24) & 0xFFU; \
            buf_out[6] = (drops >> 16) & 0xFFU; \
            buf_out[7] = (drops >> 8) & 0xFFU; \
            buf_out[8] = drops & 0xFFU; \
            buf_out += 9;

        #define ENCODE_SIGNING_PUBKEY_EMPTY(buf_out) \
            buf_out[0] = 0x73U; \
            buf_out[1] = 0x00U; \
            buf_out += 2;

        #define ENCODE_ACCOUNT(buf_out, acc, acc_type) \
            buf_out[0] = 0x80U + acc_type; \
            buf_out[1] = 0x14U; \
            for (int i = 0; i < 20; ++i) buf_out[2+i] = acc[i]; \
            buf_out += 22;

        #define PREPARE_PAYMENT_SIMPLE_SIZE 270U

        int64_t hook(uint32_t reserved) {
            _g(1, 1);

            // Only trigger on Payment transactions
            if (otxn_type() != ttPAYMENT)
                return accept(0, 0, 0);

            // Reserve 1 xport slot
            ASSERT(xport_reserve(1) == 1);

            // Get destination from parameter "DST"
            uint8_t dst[20];
            int64_t dst_len = otxn_param(SBUF(dst), "DST", 3);
            ASSERT(dst_len == 20);

            // Get hook account (source) - xport requires sfAccount to match hook account
            uint8_t acc[20];
            ASSERT(hook_account(SBUF(acc)) == 20);

            // Get ledger seq for FLS/LLS
            uint32_t cls = (uint32_t)ledger_seq();

            // Build payment transaction for export
            uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
            uint8_t* buf = tx;

            ENCODE_TT(buf, ttPAYMENT);
            ENCODE_FLAGS(buf, tfCANONICAL);
            ENCODE_SEQUENCE(buf, 0);
            ENCODE_FLS(buf, cls + 1);
            ENCODE_LLS(buf, cls + 5);

            uint64_t drops = 1000000;  // 1 XRP
            ENCODE_DROPS(buf, drops, amAMOUNT);
            ENCODE_DROPS(buf, 10, amFEE);  // minimal fee for exported txn

            ENCODE_SIGNING_PUBKEY_EMPTY(buf);
            ENCODE_ACCOUNT(buf, acc, atACCOUNT);
            ENCODE_ACCOUNT(buf, dst, atDESTINATION);

            // Export!
            uint8_t hash[32];
            int64_t xport_result = xport(SBUF(hash), (uint32_t)tx, buf - tx);
            ASSERT(xport_result == 32);

            return accept(0, 0, 0);
        }
    )[test.hook]"];

    // Helper: run xport test with given config
    // Returns true if the exported directory is empty after the flow
    // (meaning ttEXPORT_FINALIZE cleaned up the entry)
    void
    runXportTest(
        FeatureBitset features,
        std::function<std::unique_ptr<Config>()> makeConfig,
        bool expectCleanup)
    {
        using namespace jtx;
        using namespace beast::severities;

        auto logs = std::make_unique<DebugLogs>(
            *this,
            DebugLogs::Levels{
                {"View", kTrace},
                {"TxQ", kTrace},
            });

        Env env{*this, makeConfig(), features, std::move(logs), kError};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        // Install xport hook on alice
        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        auto const xportLedgerSeq = env.current()->seq();

        // Trigger hook with payment containing DST parameter
        Json::Value params(Json::arrayValue);
        Json::Value param;
        param[jss::HookParameter] = Json::Value(Json::objectValue);
        param[jss::HookParameter][jss::HookParameterName] =
            strHex(std::string("DST"));
        param[jss::HookParameter][jss::HookParameterValue] = strHex(carol.id());
        params.append(param);

        env(pay(bob, alice, XRP(100)),
            fee(XRP(1)),
            json(jss::HookParameters, params),
            ter(tesSUCCESS));
        env.close();  // Ledger N: xport() creates ltEXPORTED_TXN

        // Verify ltEXPORTED_TXN was created
        {
            auto const exportedDirKey = keylet::exportedDir();
            BEAST_EXPECT(env.current()->read(exportedDirKey));
            BEAST_EXPECT(!dirIsEmpty(*env.current(), exportedDirKey));
        }

        // Close additional ledgers for signing flow
        env.close();  // N+1: validators sign via TMValidation
        env.close();  // N+2: ttEXPORT_FINALIZE created (rawTxInsert)
        env.close();  // N+3: does ttEXPORT_FINALIZE get applied here?

        // Check if cleanup happened
        {
            auto const exportedDirKey = keylet::exportedDir();
            bool dirEmpty = dirIsEmpty(*env.current(), exportedDirKey);
            BEAST_EXPECT(dirEmpty == expectCleanup);
        }

        BEAST_EXPECT(env.current()->seq() == xportLedgerSeq + 4);
    }

    void
    testXportPaymentWithValidator(FeatureBitset features)
    {
        testcase("Xport Payment (with validator)");

        // With validator config, full flow should work:
        // N: xport creates entry
        // N+1: validator signs
        // N+2: ttEXPORT_FINALIZE cleans up
        runXportTest(features, exportTestConfig, true);
    }

    // Hook that exports a payment WITH sfNetworkID matching the local network.
    // Should be rejected by the NetworkID self-target guard.
    TestHook xport_self_target_wasm = export_test_wasm[R"[test.hook](
        #include <stdint.h>
        extern int32_t _g(uint32_t id, uint32_t maxiter);
        extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
        extern int64_t xport(uint32_t write_ptr, uint32_t write_len, uint32_t read_ptr, uint32_t read_len);
        extern int64_t xport_reserve(uint32_t count);
        extern int64_t hook_account(uint32_t write_ptr, uint32_t write_len);
        extern int64_t otxn_param(uint32_t write_ptr, uint32_t write_len, uint32_t name_ptr, uint32_t name_len);
        extern int64_t otxn_type(void);
        extern int64_t ledger_seq(void);

        #define SBUF(x) (uint32_t)(x), sizeof(x)
        #define ASSERT(x) if (!(x)) rollback((uint32_t)#x, sizeof(#x), __LINE__)

        #define ttPAYMENT 0
        #define tfCANONICAL 0x80000000UL

        #define amAMOUNT 1
        #define amFEE 8
        #define atACCOUNT 1
        #define atDESTINATION 3

        #define ENCODE_TT(buf_out, tt) \
            buf_out[0] = 0x12U; \
            buf_out[1] = (tt >> 8) & 0xFFU; \
            buf_out[2] = tt & 0xFFU; \
            buf_out += 3;

        #define ENCODE_FLAGS(buf_out, flags) \
            buf_out[0] = 0x22U; \
            buf_out[1] = (flags >> 24) & 0xFFU; \
            buf_out[2] = (flags >> 16) & 0xFFU; \
            buf_out[3] = (flags >> 8) & 0xFFU; \
            buf_out[4] = flags & 0xFFU; \
            buf_out += 5;

        #define ENCODE_SEQUENCE(buf_out, seq) \
            buf_out[0] = 0x24U; \
            buf_out[1] = (seq >> 24) & 0xFFU; \
            buf_out[2] = (seq >> 16) & 0xFFU; \
            buf_out[3] = (seq >> 8) & 0xFFU; \
            buf_out[4] = seq & 0xFFU; \
            buf_out += 5;

        // sfNetworkID = UINT32 field 1 = 0x21
        #define ENCODE_NETWORK_ID(buf_out, id) \
            buf_out[0] = 0x21U; \
            buf_out[1] = (id >> 24) & 0xFFU; \
            buf_out[2] = (id >> 16) & 0xFFU; \
            buf_out[3] = (id >> 8) & 0xFFU; \
            buf_out[4] = id & 0xFFU; \
            buf_out += 5;

        #define ENCODE_FLS(buf_out, fls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1AU; \
            buf_out[2] = (fls >> 24) & 0xFFU; \
            buf_out[3] = (fls >> 16) & 0xFFU; \
            buf_out[4] = (fls >> 8) & 0xFFU; \
            buf_out[5] = fls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_LLS(buf_out, lls) \
            buf_out[0] = 0x20U; \
            buf_out[1] = 0x1BU; \
            buf_out[2] = (lls >> 24) & 0xFFU; \
            buf_out[3] = (lls >> 16) & 0xFFU; \
            buf_out[4] = (lls >> 8) & 0xFFU; \
            buf_out[5] = lls & 0xFFU; \
            buf_out += 6;

        #define ENCODE_DROPS(buf_out, drops, amt_type) \
            buf_out[0] = 0x60U + amt_type; \
            buf_out[1] = 0x40U + ((drops >> 56) & 0x3FU); \
            buf_out[2] = (drops >> 48) & 0xFFU; \
            buf_out[3] = (drops >> 40) & 0xFFU; \
            buf_out[4] = (drops >> 32) & 0xFFU; \
            buf_out[5] = (drops >> 24) & 0xFFU; \
            buf_out[6] = (drops >> 16) & 0xFFU; \
            buf_out[7] = (drops >> 8) & 0xFFU; \
            buf_out[8] = drops & 0xFFU; \
            buf_out += 9;

        #define ENCODE_SIGNING_PUBKEY_EMPTY(buf_out) \
            buf_out[0] = 0x73U; \
            buf_out[1] = 0x00U; \
            buf_out += 2;

        #define ENCODE_ACCOUNT(buf_out, acc, acc_type) \
            buf_out[0] = 0x80U + acc_type; \
            buf_out[1] = 0x14U; \
            for (int i = 0; i < 20; ++i) buf_out[2+i] = acc[i]; \
            buf_out += 22;

        #define PREPARE_PAYMENT_SIMPLE_SIZE 270U

        int64_t hook(uint32_t reserved) {
            _g(1, 1);

            if (otxn_type() != ttPAYMENT)
                return accept(0, 0, 0);

            ASSERT(xport_reserve(1) == 1);

            uint8_t dst[20];
            int64_t dst_len = otxn_param(SBUF(dst), "DST", 3);
            ASSERT(dst_len == 20);

            uint8_t acc[20];
            ASSERT(hook_account(SBUF(acc)) == 20);

            uint32_t cls = (uint32_t)ledger_seq();

            uint8_t tx[PREPARE_PAYMENT_SIMPLE_SIZE];
            uint8_t* buf = tx;

            ENCODE_TT(buf, ttPAYMENT);
            ENCODE_NETWORK_ID(buf, 21337);  // must precede Sequence (canonical order)
            ENCODE_FLAGS(buf, tfCANONICAL);
            ENCODE_SEQUENCE(buf, 0);
            ENCODE_FLS(buf, cls + 1);
            ENCODE_LLS(buf, cls + 5);

            uint64_t drops = 1000000;
            ENCODE_DROPS(buf, drops, amAMOUNT);
            ENCODE_DROPS(buf, 10, amFEE);

            ENCODE_SIGNING_PUBKEY_EMPTY(buf);
            ENCODE_ACCOUNT(buf, acc, atACCOUNT);
            ENCODE_ACCOUNT(buf, dst, atDESTINATION);

            uint8_t hash[32];
            int64_t xport_result = xport(SBUF(hash), (uint32_t)tx, buf - tx);
            // xport should return EXPORT_FAILURE (-46), ASSERT will rollback
            ASSERT(xport_result == 32);

            return accept(0, 0, 0);
        }
    )[test.hook]"];

    void
    testXportRejectsLocalNetworkID(FeatureBitset features)
    {
        testcase("Xport rejects export targeting local NetworkID");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        // Install hook that exports a tx with NetworkID=21337
        env(ripple::test::jtx::hook(alice, {{hso(xport_self_target_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Trigger: xport() should reject because exported tx's NetworkID
        // matches the local network → EXPORT_FAILURE → hook rollback
        Json::Value params(Json::arrayValue);
        Json::Value param;
        param[jss::HookParameter] = Json::Value(Json::objectValue);
        param[jss::HookParameter][jss::HookParameterName] =
            strHex(std::string("DST"));
        param[jss::HookParameter][jss::HookParameterValue] = strHex(carol.id());
        params.append(param);

        env(pay(bob, alice, XRP(100)),
            fee(XRP(1)),
            json(jss::HookParameters, params),
            ter(tecHOOK_REJECTED));
        env.close();

        // Verify no ltEXPORTED_TXN was created
        auto const exportedDirKey = keylet::exportedDir();
        BEAST_EXPECT(dirIsEmpty(*env.current(), exportedDirKey));
    }

    void
    testXportRejectsUnconfiguredNetworkID(FeatureBitset features)
    {
        testcase("Xport rejects export when NETWORK_ID is unconfigured");

        using namespace jtx;

        // Default NETWORK_ID=0: node can't safely distinguish self from
        // cross-chain, so exports without sfNetworkID must be rejected.
        Env env{*this, envconfig(jtx::validator, ""), features};

        Account const alice{"alice"};
        Account const bob{"bob"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, bob, carol);
        env.close();

        // Install the normal xport hook (no NetworkID in exported tx)
        env(ripple::test::jtx::hook(alice, {{hso(xport_wasm)}}, 0),
            HSFEE,
            ter(tesSUCCESS));
        env.close();

        // Trigger: xport() should reject because NETWORK_ID=0 and the
        // exported tx has no sfNetworkID → can't verify it's cross-chain
        Json::Value params(Json::arrayValue);
        Json::Value param;
        param[jss::HookParameter] = Json::Value(Json::objectValue);
        param[jss::HookParameter][jss::HookParameterName] =
            strHex(std::string("DST"));
        param[jss::HookParameter][jss::HookParameterValue] = strHex(carol.id());
        params.append(param);

        env(pay(bob, alice, XRP(100)),
            fee(XRP(1)),
            json(jss::HookParameters, params),
            ter(tecHOOK_REJECTED));
        env.close();

        // Verify no ltEXPORTED_TXN was created
        auto const exportedDirKey = keylet::exportedDir();
        BEAST_EXPECT(dirIsEmpty(*env.current(), exportedDirKey));
    }

    // Build a minimal unsigned Payment STObject suitable for sfExportedTxn.
    static STObject
    buildExportedPayment(
        AccountID const& src,
        AccountID const& dst,
        std::uint32_t fls,
        std::uint32_t lls)
    {
        STObject obj(sfExportedTxn);
        obj.setFieldU16(sfTransactionType, ttPAYMENT);
        obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
        obj.setFieldU32(sfSequence, 0);
        obj.setFieldU32(sfFirstLedgerSequence, fls);
        obj.setFieldU32(sfLastLedgerSequence, lls);
        obj.setFieldAmount(sfAmount, XRPAmount{1000000});
        obj.setFieldAmount(sfFee, XRPAmount{10});
        obj.setFieldVL(sfSigningPubKey, Blob{});
        obj.setAccountID(sfAccount, src);
        obj.setAccountID(sfDestination, dst);
        return obj;
    }

    void
    testExportTxn(FeatureBitset features)
    {
        testcase("ttEXPORT_USER creates ltEXPORTED_TXN");

        using namespace jtx;

        Env env{*this, exportTestConfig(), features};

        Account const alice{"alice"};
        Account const carol{"carol"};

        env.fund(XRP(10000), alice, carol);
        env.close();

        auto const seq = env.current()->seq();
        auto innerObj =
            buildExportedPayment(alice.id(), carol.id(), seq + 1, seq + 5);

        // Submit ttEXPORT_USER with inner payment as STObject
        Json::Value jv;
        jv[jss::TransactionType] = jss::Export;
        jv[jss::Account] = alice.human();
        jv[sfExportedTxn.jsonName] = innerObj.getJson(JsonOptions::none);

        env(jv, fee(XRP(1)), ter(tesSUCCESS));
        env.close();

        // Verify ltEXPORTED_TXN was created
        auto const exportedDirKey = keylet::exportedDir();
        BEAST_EXPECT(!dirIsEmpty(*env.current(), exportedDirKey));
    }

    void
    testStaleSignatureCleanup(FeatureBitset features)
    {
        testcase("Stale Export Signature Cleanup");

        using namespace jtx;

        Env env{*this, envconfig(jtx::validator, ""), features};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);
        std::string const staleTag = "stale-export-signature";
        auto const txHash = sha512Half(makeSlice(staleTag));

        STObject signer(sfSigner);
        signer.setAccountID(sfAccount, validatorAcc);
        signer.setFieldVL(sfSigningPubKey, validator.first.slice());
        signer.setFieldVL(sfTxnSignature, Blob{0x01, 0x02, 0x03, 0x04});

        auto& collector = env.app().getExportSignatureCollector();
        collector.addSignature(
            txHash, validator.first, signer, env.current()->seq());
        BEAST_EXPECT(collector.signatureCount(txHash) == 1);

        // cleanupStale uses default maxAge=256 and removes when currentSeq is
        // strictly greater than firstSeen+maxAge.
        for (int i = 0; i < 260; ++i)
            env.close();

        BEAST_EXPECT(collector.signatureCount(txHash) == 0);
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        FeatureBitset const allWithExport{all | featureExport};
        testXportPaymentWithValidator(allWithExport);
        testXportRejectsLocalNetworkID(allWithExport);
        testXportRejectsUnconfiguredNetworkID(allWithExport);
        testExportTxn(allWithExport);
        testStaleSignatureCleanup(allWithExport);
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
