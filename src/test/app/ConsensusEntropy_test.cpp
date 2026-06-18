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

#include <test/app/ConsensusEntropy_test_hooks.h>
#include <test/jtx.h>
#include <test/jtx/hook.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>

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

    static int64_t
    hookReturnCode(STObject const& hookExecution)
    {
        auto const rawCode = hookExecution.getFieldU64(sfHookReturnCode);
        return (rawCode & 0x8000000000000000ULL)
            ? -static_cast<int64_t>(rawCode & 0x7FFFFFFFFFFFFFFFULL)
            : static_cast<int64_t>(rawCode);
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
            extern int64_t dice(uint32_t sides, uint32_t min_tier, uint32_t min_count);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                // dice(6) should return 0..5
                int64_t result = dice(6, 3, 5);

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
            extern int64_t random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t min_count);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                uint8_t buf[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    buf[i] = 0;

                int64_t result = random((uint32_t)buf, 32, 3, 5);

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
            extern int64_t dice(uint32_t sides, uint32_t min_tier, uint32_t min_count);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t r1 = dice(1000000, 3, 5);
                if (r1 < 0)
                    rollback(0, 0, r1);

                int64_t r2 = dice(1000000, 3, 5);
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
    testDiceZeroSides()
    {
        testcase("Hook dice(0) returns INVALID_ARGUMENT");
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

        // Hook calls dice(0) and returns whatever dice returns.
        // dice(0) should return INVALID_ARGUMENT (-7).
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t dice(uint32_t sides, uint32_t min_tier, uint32_t min_count);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t result = dice(0, 3, 5);
                // dice(0) should return negative error code, pass it through
                return accept(0, 0, result);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set dice0 hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test dice(0)"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        // INVALID_ARGUMENT = -7, encoded as 0x8000000000000000 + abs(code)
        // (see applyHook.cpp unsigned_exit_code encoding)
        auto const rawCode = hookExecutions[0].getFieldU64(sfHookReturnCode);
        int64_t returnCode = (rawCode & 0x8000000000000000ULL)
            ? -static_cast<int64_t>(rawCode & 0x7FFFFFFFFFFFFFFFULL)
            : static_cast<int64_t>(rawCode);
        std::cerr << "  dice(0) returnCode = " << returnCode << " (raw 0x"
                  << std::hex << rawCode << std::dec << ")\n";
        BEAST_EXPECT(returnCode == -7);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testDiceRequirementNotMet()
    {
        testcase("Hook dice() returns TOO_LITTLE_ENTROPY below requirement");
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

        // Standalone entropy carries EntropyCount=20 / tier validator_quorum.
        // A hook demanding min_count=21 states a requirement this ledger
        // cannot meet, so dice must fail closed with TOO_LITTLE_ENTROPY (-48)
        // rather than silently serving weaker entropy.
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t dice(uint32_t sides, uint32_t min_tier, uint32_t min_count);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t result = dice(6, 3, 21);
                // requirement unmet: pass the error code through
                return accept(0, 0, result);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set dice-requirement hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test dice min_count unmet"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        auto const rawCode = hookExecutions[0].getFieldU64(sfHookReturnCode);
        int64_t returnCode = (rawCode & 0x8000000000000000ULL)
            ? -static_cast<int64_t>(rawCode & 0x7FFFFFFFFFFFFFFFULL)
            : static_cast<int64_t>(rawCode);
        std::cerr << "  dice(6,3,21) returnCode = " << returnCode << " (raw 0x"
                  << std::hex << rawCode << std::dec << ")\n";
        BEAST_EXPECT(returnCode == -48);  // TOO_LITTLE_ENTROPY
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testInvalidEntropyRequirements()
    {
        testcase("Hook dice/random reject invalid entropy requirements");
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

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t dice(uint32_t sides, uint32_t min_tier, uint32_t min_count);
            extern int64_t random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t min_count);
            #define INVALID_ARGUMENT (-7)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t buf[32];

                int64_t bad_min_tier = dice(6, 0, 0);
                if (bad_min_tier != INVALID_ARGUMENT)
                    return accept(0, 0, bad_min_tier);

                int64_t bad_high_tier = dice(6, 4, 0);
                if (bad_high_tier != INVALID_ARGUMENT)
                    return accept(0, 0, bad_high_tier);

                int64_t bad_min_count = dice(6, 3, 70000);
                if (bad_min_count != INVALID_ARGUMENT)
                    return accept(0, 0, bad_min_count);

                int64_t bad_random_tier = random((uint32_t)buf, 32, 4, 0);
                if (bad_random_tier != INVALID_ARGUMENT)
                    return accept(0, 0, bad_random_tier);

                // Sentinel distinct from any dice (0..5) / random result and
                // from INVALID_ARGUMENT, so a regression that lets a bad
                // requirement through returns its own code, not this one.
                return accept(0, 0, 42);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set invalid entropy requirement hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test invalid entropy requirements"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        // 42 only if all four invalid requirements were rejected; any bad
        // requirement leaking through returns its own (non-42) code.
        BEAST_EXPECT(hookReturnCode(hookExecutions[0]) == 42);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    run() override
    {
        testSLECreated();
        testSLEUpdatedOnSubsequentClose();
        testNoSLEWithoutAmendment();
        testDice();
        testDiceZeroSides();
        testDiceRequirementNotMet();
        testInvalidEntropyRequirements();
        testRandom();
        testDiceConsecutiveCallsDiffer();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusEntropy, app, ripple);

}  // namespace test
}  // namespace ripple
