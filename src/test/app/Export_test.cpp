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
    // Hook that exports a payment using xport (for cross-chain export)
    // xport APIs are gated by featureExportRNG amendment, not sfHookApiVersion
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
    // (meaning ttEXPORT cleaned up the entry)
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
        env.close();  // N+2: ttEXPORT created (rawTxInsert)
        env.close();  // N+3: does ttEXPORT get applied here?

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
        // N+2: ttEXPORT cleans up
        runXportTest(
            features,
            []() { return jtx::envconfig(jtx::validator, ""); },
            true);
    }

    void
    run() override
    {
        using namespace test::jtx;
        FeatureBitset const all{supported_amendments()};
        FeatureBitset const allWithExport{all | featureExportRNG};
        testXportPaymentWithValidator(allWithExport);
    }
};

BEAST_DEFINE_TESTSUITE(Export, app, ripple);

}  // namespace test
}  // namespace ripple
