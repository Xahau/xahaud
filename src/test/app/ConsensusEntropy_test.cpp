//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2024 XRPL Labs

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

#include <ripple/app/hook/Enum.h>
#include <ripple/beast/unit_test.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/SField.h>
#include <ripple/protocol/TxFlags.h>
#include <ripple/protocol/jss.h>
#include <test/app/ConsensusEntropy_test_hooks.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>

namespace ripple {
namespace test {

using TestHook = std::vector<uint8_t> const&;

#define BEAST_REQUIRE(x)     \
    {                        \
        BEAST_EXPECT(!!(x)); \
        if (!(x))            \
            return;          \
    }

#define HSFEE fee(100'000'000)
#define M(m) memo(m, "", "")

class ConsensusEntropy_test : public beast::unit_test::suite
{
    static void
    overrideFlag(Json::Value& jv)
    {
        jv[jss::Flags] = hsfOVERRIDE;
    }

    void
    testSLECreated()
    {
        testcase("SLE created on ledger close");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        BEAST_EXPECT(!env.le(keylet::consensusEntropy()));

        env.close();

        auto const sle = env.le(keylet::consensusEntropy());
        BEAST_REQUIRE(sle);

        auto const digest = sle->getFieldH256(sfDigest);
        BEAST_EXPECT(digest != uint256{});

        auto const count = sle->getFieldU16(sfEntropyCount);
        BEAST_EXPECT(count >= 5);

        auto const sleSeq = sle->getFieldU32(sfLedgerSequence);
        BEAST_EXPECT(sleSeq == env.closed()->seq());
    }

    void
    testSLEUpdatedOnSubsequentClose()
    {
        testcase("SLE updated on subsequent ledger close");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        env.close();
        auto const sle1 = env.le(keylet::consensusEntropy());
        BEAST_REQUIRE(sle1);

        auto const digest1 = sle1->getFieldH256(sfDigest);
        auto const seq1 = sle1->getFieldU32(sfLedgerSequence);

        env.close();

        auto const sle2 = env.le(keylet::consensusEntropy());
        BEAST_REQUIRE(sle2);

        auto const digest2 = sle2->getFieldH256(sfDigest);
        auto const seq2 = sle2->getFieldU32(sfLedgerSequence);

        BEAST_EXPECT(digest2 != digest1);
        BEAST_EXPECT(seq2 == seq1 + 1);
    }

    void
    testNoSLEWithoutAmendment()
    {
        testcase("No SLE without amendment");
        using namespace jtx;

        Env env{*this};

        env.close();
        env.close();

        BEAST_EXPECT(!env.le(keylet::consensusEntropy()));
    }

    void
    testDice()
    {
        testcase("Hook dice() API");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Entropy SLE must exist before hook can use dice()
        BEAST_REQUIRE(env.le(keylet::consensusEntropy()));

        // Set the hook
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t dice(uint32_t sides);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)

            int64_t cbak(uint32_t r) { return 0; }

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                // dice(6) should return 0..5
                int64_t result = dice(6);

                // negative means error
                if (result < 0)
                    rollback(0, 0, result);

                if (result >= 6)
                    rollback(0, 0, -1);

                // return the dice result as the accept code
                return accept(0, 0, result);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set dice hook"),
            HSFEE);
        env.close();

        // Invoke the hook
        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test dice"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        auto const returnCode = hookExecutions[0].getFieldU64(sfHookReturnCode);
        std::cerr << "  dice(6) returnCode = " << returnCode << " (hex 0x"
                  << std::hex << returnCode << std::dec << ")\n";
        // dice(6) returns 0..5
        BEAST_EXPECT(returnCode <= 5);

        // Result should be 3 (accept)
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testRandom()
    {
        testcase("Hook random() API");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(env.le(keylet::consensusEntropy()));

        // Hook calls random() to fill a 32-byte buffer, then checks
        // the buffer is not all zeroes.
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t random(uint32_t write_ptr, uint32_t write_len);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)

            int64_t cbak(uint32_t r) { return 0; }

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                uint8_t buf[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    buf[i] = 0;

                int64_t result = random((uint32_t)buf, 32);

                // Should return 32 (bytes written)
                if (result != 32)
                    rollback(0, 0, result);

                // Verify buffer is not all zeroes
                int nonzero = 0;
                for (int i = 0; GUARD(32), i < 32; ++i)
                    if (buf[i] != 0) nonzero = 1;

                if (!nonzero)
                    rollback(0, 0, -2);

                return accept(0, 0, 0);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set random hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test random"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        // Return code 0 = all checks passed in the hook
        BEAST_EXPECT(hookExecutions[0].getFieldU64(sfHookReturnCode) == 0);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testDiceConsecutiveCallsDiffer()
    {
        testcase("Hook dice() consecutive calls return different values");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(env.le(keylet::consensusEntropy()));

        // dice(1000000) twice — large range makes collision near-impossible
        // encode r1 in low 20 bits, r2 in high bits
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t dice(uint32_t sides);

            int64_t cbak(uint32_t r) { return 0; }

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t r1 = dice(1000000);
                if (r1 < 0)
                    rollback(0, 0, r1);

                int64_t r2 = dice(1000000);
                if (r2 < 0)
                    rollback(0, 0, r2);

                // consecutive calls should differ (rngCallCounter)
                if (r1 == r2)
                    rollback(0, 0, -1);

                return accept(0, 0, r1 | (r2 << 20));
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set dice hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test dice consecutive"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        auto const rc = hookExecutions[0].getFieldU64(sfHookReturnCode);
        auto const r1 = rc & 0xFFFFF;
        auto const r2 = (rc >> 20) & 0xFFFFF;

        std::cerr << "  two-call dice(1000000): returnCode=" << rc << " hex=0x"
                  << std::hex << rc << std::dec << " r1=" << r1 << " r2=" << r2
                  << "\n";

        // hookResult 3 = accept (would be 1 if r1==r2 triggered rollback)
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
        BEAST_EXPECT(r1 < 1000000);
        BEAST_EXPECT(r2 < 1000000);
        BEAST_EXPECT(r1 != r2);
    }

    void
    run() override
    {
        testSLECreated();
        testSLEUpdatedOnSubsequentClose();
        testNoSLEWithoutAmendment();
        testDice();
        testRandom();
        testDiceConsecutiveCallsDiffer();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusEntropy, app, ripple);

}  // namespace test
}  // namespace ripple
