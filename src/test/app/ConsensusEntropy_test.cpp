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
#include <xrpld/app/hook/applyHook.h>
#include <xrpld/app/ledger/BuildLedger.h>
#include <xrpld/app/ledger/LedgerMaster.h>
#include <xrpld/app/ledger/LedgerReplay.h>
#include <xrpld/app/misc/RuntimeConfig.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/hook/Enum.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/Feature.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/SField.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/jss.h>
#include <algorithm>
#include <array>
#include <limits>

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

namespace {

Blob
standaloneContributorMask(std::uint16_t denominator, std::uint16_t count)
{
    Blob mask((denominator + 7) / 8, 0);
    for (std::uint16_t i = 0; i < std::min(denominator, count); ++i)
        mask[i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
    return mask;
}

std::uint32_t
expectedDice(uint256 block, std::uint32_t sides)
{
    auto const sampleRange =
        std::uint64_t{std::numeric_limits<std::uint32_t>::max()} + 1;
    auto const acceptLimit = sampleRange - (sampleRange % sides);

    for (;;)
    {
        for (std::size_t i = 0; i < block.size(); i += sizeof(std::uint32_t))
        {
            auto const* candidate = block.data() + i;
            std::uint32_t const value = (std::uint32_t{candidate[0]} << 24U) |
                (std::uint32_t{candidate[1]} << 16U) |
                (std::uint32_t{candidate[2]} << 8U) |
                std::uint32_t{candidate[3]};
            if (value < acceptLimit)
                return value % sides;
        }
        block = sha512Half(Slice{block.data(), block.size()});
    }
}

}  // namespace

class ConsensusEntropy_test : public beast::unit_test::suite
{
    static std::shared_ptr<STTx const>
    recordedEntropy(jtx::Env& env)
    {
        // Independent oracle: recover the input from committed transactions.
        return env.app()
            .getLedgerMaster()
            .getClosedLedger()
            ->readConsensusEntropyFromTransactions();
    }

    void
    checkEntropyRecovery(jtx::Env& env)
    {
        auto const closed = env.app().getLedgerMaster().getClosedLedger();
        auto const recorded = recordedEntropy(env);
        BEAST_REQUIRE(recorded);
        bool loaded = false;
        auto cold = std::make_shared<Ledger>(
            closed->info(),
            loaded,
            false,
            env.app().config(),
            env.app().getNodeFamily(),
            env.journal);
        BEAST_REQUIRE(loaded);
        auto const recovered = cold->consensusEntropy();
        BEAST_REQUIRE(recovered);
        BEAST_EXPECT(
            recovered->getTransactionID() == recorded->getTransactionID());

        // A restarted node can preview N+1 from the loaded N input, while
        // actual construction of N+1 starts empty until its own pseudo applies.
        OpenView preview{open_ledger, closed->rules(), cold};
        OpenView copy{preview};
        BEAST_EXPECT(copy.consensusEntropy() == recovered);
        auto successor =
            std::make_shared<Ledger>(*cold, closed->info().closeTime);
        BEAST_EXPECT(!successor->consensusEntropy());

        auto const parent = env.app().getLedgerMaster().getLedgerByHash(
            closed->info().parentHash);
        BEAST_REQUIRE(parent);
        auto const replay = buildLedger(
            LedgerReplay(parent, closed), tapNONE, env.app(), env.journal);
        BEAST_EXPECT(replay->info().hash == closed->info().hash);
        auto const replayInput = replay->consensusEntropy();
        BEAST_REQUIRE(replayInput);
        BEAST_EXPECT(
            replayInput->getTransactionID() == recorded->getTransactionID());
    }

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

    static std::string
    hookReturnString(STObject const& hookExecution)
    {
        auto const ret = hookExecution.getFieldVL(sfHookReturnString);
        return std::string(ret.begin(), ret.end());
    }

    void
    testContextCreated()
    {
        testcase("Entropy context created by ledger pseudo");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        BEAST_EXPECT(!recordedEntropy(env));

        env.close();

        auto const input = recordedEntropy(env);
        BEAST_REQUIRE(input);

        auto const digest = input->getFieldH256(sfDigest);
        BEAST_EXPECT(digest != uint256{});

        auto const count = input->getFieldU16(sfEntropyCount);
        BEAST_EXPECT(count >= 5);
        BEAST_EXPECT(input->getFieldU16(sfEntropyDenominator) == count);
        BEAST_EXPECT(
            input->getFieldVL(sfEntropyContributors) ==
            standaloneContributorMask(count, count));
        BEAST_EXPECT(
            input->getFieldU8(sfEntropyTier) == entropyTierValidatorFull);

        auto const inputSeq = input->getFieldU32(sfLedgerSequence);
        BEAST_EXPECT(inputSeq == env.closed()->seq());
        BEAST_EXPECT(
            env.closed()->consensusEntropy()->getTransactionID() ==
            input->getTransactionID());
        BEAST_EXPECT(!env.closed()->read(
            keylet::unchecked(sha512Half(std::uint16_t{'X'}))));
        checkEntropyRecovery(env);
    }

    void
    testContextUpdatedOnSubsequentClose()
    {
        testcase("Fresh entropy context on subsequent ledger close");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        env.close();
        auto const input1 = recordedEntropy(env);
        BEAST_REQUIRE(input1);

        auto const digest1 = input1->getFieldH256(sfDigest);
        auto const seq1 = input1->getFieldU32(sfLedgerSequence);

        env.close();

        auto const input2 = recordedEntropy(env);
        BEAST_REQUIRE(input2);

        auto const digest2 = input2->getFieldH256(sfDigest);
        auto const seq2 = input2->getFieldU32(sfLedgerSequence);

        BEAST_EXPECT(digest2 != digest1);
        BEAST_EXPECT(seq2 == seq1 + 1);
    }

    void
    testNoContextWithoutAmendment()
    {
        testcase("No entropy context without amendment");
        using namespace jtx;

        Env env{*this, supported_amendments()};

        env.close();
        env.close();

        BEAST_EXPECT(!recordedEntropy(env));
    }

    void
    testDice()
    {
        testcase("Hook entropy_cr_dice() API");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        // Recorded entropy input must exist before hook can use
        // entropy_cr_dice()
        BEAST_REQUIRE(recordedEntropy(env));

        // Set the hook
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                // A wide range makes this a useful byte-order known answer.
                int64_t result = entropy_cr_dice(1000000, 3, 0);

                // negative means error
                if (result < 0)
                    rollback(0, 0, result);

                if (result >= 1000000)
                    rollback(0, 0, -1);

                // return the entropy_cr_dice result as the accept code
                return accept(0, 0, result);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set entropy_cr_dice hook"),
            HSFEE);
        env.close();

        // Invoke the hook
        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test entropy_cr_dice"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        auto const entropy = recordedEntropy(env);
        BEAST_REQUIRE(entropy);
        auto const entropyDigest = entropy->getFieldH256(sfDigest);
        auto const drawLedgerSeq = entropy->getFieldU32(sfLedgerSequence);
        auto const returnCode = hookExecutions[0].getFieldU64(sfHookReturnCode);
        auto const firstBlock = sha512Half(
            drawLedgerSeq,
            env.tx()->getTransactionID(),
            alice.id(),
            hookExecutions[0].getFieldH256(sfHookHash),
            alice.id(),
            std::uint8_t{0},
            std::string{"strong"},
            std::string{"direct"},
            entropyDigest,
            std::uint64_t{0});
        auto const expected = expectedDice(firstBlock, 1000000);
        std::cerr << "  entropy_cr_dice(1000000) returnCode = " << returnCode
                  << " (hex 0x" << std::hex << returnCode << std::dec << ")\n";
        BEAST_EXPECT(returnCode == expected);

        // Result should be 3 (accept)
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
        checkEntropyRecovery(env);
    }

    void
    testRandom()
    {
        testcase("Hook entropy_cr_random() API");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(recordedEntropy(env));

        // Hook calls entropy_cr_random() to fill a 32-byte buffer, then checks
        // the buffer is not all zeroes.
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t flags);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                uint8_t buf[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    buf[i] = 0;

                int64_t result = entropy_cr_random((uint32_t)buf, 32, 3, 0);

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
            M("set entropy_cr_random hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test entropy_cr_random"), fee(XRP(1)));

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
        testcase(
            "Hook entropy_cr_dice() consecutive calls return different values");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(recordedEntropy(env));

        // entropy_cr_dice(1000000) twice — large range makes collision
        // near-impossible encode r1 in low 20 bits, r2 in high bits
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t r1 = entropy_cr_dice(1000000, 3, 0);
                if (r1 < 0)
                    rollback(0, 0, r1);

                int64_t r2 = entropy_cr_dice(1000000, 3, 0);
                if (r2 < 0)
                    rollback(0, 0, r2);

                // consecutive calls should differ (rngCallCounter)
                if (r1 == r2)
                    rollback(0, 0, -1);

                return accept(0, 0, r1 | (r2 << 20));
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set entropy_cr_dice hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test entropy_cr_dice consecutive"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        auto const rc = hookExecutions[0].getFieldU64(sfHookReturnCode);
        auto const r1 = rc & 0xFFFFF;
        auto const r2 = (rc >> 20) & 0xFFFFF;

        std::cerr << "  two-call entropy_cr_dice(1000000): returnCode=" << rc
                  << " hex=0x" << std::hex << rc << std::dec << " r1=" << r1
                  << " r2=" << r2 << "\n";

        // hookResult 3 = accept (would be 1 if r1==r2 triggered rollback)
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
        BEAST_EXPECT(r1 < 1000000);
        BEAST_EXPECT(r2 < 1000000);
        BEAST_EXPECT(r1 != r2);
    }

    void
    testDiceZeroSides()
    {
        testcase("Hook entropy_cr_dice(0) returns INVALID_ARGUMENT");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(recordedEntropy(env));

        // Hook calls entropy_cr_dice(0) and returns whatever entropy_cr_dice
        // returns. entropy_cr_dice(0) should return INVALID_ARGUMENT (-7).
        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t result = entropy_cr_dice(0, 3, 0);
                // Zero sides should return a negative error code.
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
        env(invoke, M("test entropy_cr_dice(0)"), fee(XRP(1)));

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
        std::cerr << "  entropy_cr_dice(0) returnCode = " << returnCode
                  << " (raw 0x" << std::hex << rawCode << std::dec << ")\n";
        BEAST_EXPECT(returnCode == -7);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testEntropyStatus()
    {
        testcase("Hook entropy_cr_status() metadata and policy recipes");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        ConsensusTestConfig cfg;
        cfg.standaloneEntropyTier = entropyTierValidatorQuorum;
        cfg.standaloneEntropyCount = 19;
        cfg.standaloneEntropyDenominator = 20;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const input = recordedEntropy(env);
        BEAST_REQUIRE(input);
        BEAST_EXPECT(
            input->getFieldU8(sfEntropyTier) == entropyTierValidatorQuorum);
        BEAST_EXPECT(input->getFieldU16(sfEntropyCount) == 19);
        BEAST_EXPECT(input->getFieldU16(sfEntropyDenominator) == 20);

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_status(void);
            #define ENTROPY_TIER(x) (((uint64_t)(x) >> 32U) & 0xFFU)
            #define ENTROPY_COUNT(x) (((uint64_t)(x) >> 16U) & 0xFFFFU)
            #define ENTROPY_DENOMINATOR(x) ((uint64_t)(x) & 0xFFFFU)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t status = entropy_cr_status();
                if (status < 0)
                    return accept(0, 0, 13);

                uint64_t expected =
                    ((uint64_t)3 << 32U) | ((uint64_t)19 << 16U) | 20U;
                if ((uint64_t)status != expected)
                    return accept(0, 0, 14);

                uint32_t tier = ENTROPY_TIER(status);
                uint32_t count = ENTROPY_COUNT(status);
                uint32_t denominator = ENTROPY_DENOMINATOR(status);
                if (tier != 3 || count != 19 || denominator != 20)
                    return accept(0, 0, 15);

                // Common caller-side policies: tolerate one absent, require
                // 4/5 participation, and require an absolute floor of 19.
                if (tier < 2 || denominator - count > 1)
                    return accept(0, 0, 16);
                if ((uint64_t)5 * count < (uint64_t)4 * denominator)
                    return accept(0, 0, 17);
                if (count < 19)
                    return accept(0, 0, 18);

                return accept(0, 0, 0);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set entropy-status hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test entropy status"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        BEAST_EXPECT(hookReturnCode(hookExecutions[0]) == 0);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testEntropyStatusFallback()
    {
        testcase(
            "Hook entropy_cr_status() classifies fallback before arithmetic");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        ConsensusTestConfig cfg;
        cfg.standaloneEntropyTier = entropyTierConsensusFallback;
        cfg.standaloneEntropyCount = 0;
        cfg.standaloneEntropyDenominator = 0;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const input = recordedEntropy(env);
        BEAST_REQUIRE(input);
        BEAST_EXPECT(
            input->getFieldU8(sfEntropyTier) == entropyTierConsensusFallback);
        BEAST_EXPECT(input->getFieldU16(sfEntropyCount) == 0);
        BEAST_EXPECT(input->getFieldU16(sfEntropyDenominator) == 0);

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            extern int64_t entropy_cr_status(void);
            #define ENTROPY_TIER(x) (((uint64_t)(x) >> 32U) & 0xFFU)
            #define ENTROPY_COUNT(x) (((uint64_t)(x) >> 16U) & 0xFFFFU)
            #define ENTROPY_DENOMINATOR(x) ((uint64_t)(x) & 0xFFFFU)
            #define TOO_LITTLE_ENTROPY (-48)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t status = entropy_cr_status();
                if (status < 0)
                    return accept(0, 0, 20);
                if (ENTROPY_TIER(status) != 1 || ENTROPY_COUNT(status) != 0 ||
                    ENTROPY_DENOMINATOR(status) != 0)
                    return accept(0, 0, 21);

                int64_t allowed = entropy_cr_dice(6, 1, 0);
                if (allowed < 0 || allowed > 5)
                    return accept(0, 0, 22);
                if (entropy_cr_dice(6, 2, 0) != TOO_LITTLE_ENTROPY ||
                    entropy_cr_dice(6, 2, 1) != TOO_LITTLE_ENTROPY)
                    return accept(0, 0, 23);

                return accept(0, 0, 0);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set fallback entropy-status hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test fallback entropy status"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);
        BEAST_EXPECT(hookReturnCode(hookExecutions[0]) == 0);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testStaleEntropyStatus()
    {
        testcase(
            "Hook entropy_cr_status() observes stale metadata while draws "
            "fail");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        ConsensusTestConfig cfg;
        cfg.standaloneEntropyTier = entropyTierValidatorQuorum;
        cfg.standaloneEntropyCount = 19;
        cfg.standaloneEntropyDenominator = 20;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            extern int64_t entropy_cr_random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t flags);
            extern int64_t entropy_cr_status(void);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
            #define TOO_LITTLE_ENTROPY (-48)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint64_t expected =
                    ((uint64_t)3 << 32U) | ((uint64_t)19 << 16U) | 20U;
                if ((uint64_t)entropy_cr_status() != expected)
                    return accept(0, 0, 40);

                int64_t dice_result = entropy_cr_dice(6, 1, 0);
                if (dice_result != TOO_LITTLE_ENTROPY)
                    return accept(0, 0, 41);
                if (entropy_cr_dice(6, 1, 1) != TOO_LITTLE_ENTROPY)
                    return accept(0, 0, 44);

                uint8_t buf[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    buf[i] = 0xA5;
                if (entropy_cr_random((uint32_t)buf, 32, 1, 0) != TOO_LITTLE_ENTROPY)
                    return accept(0, 0, 42);
                if (entropy_cr_random((uint32_t)buf, 32, 1, 1) != TOO_LITTLE_ENTROPY)
                    return accept(0, 0, 45);
                for (int i = 0; GUARD(32), i < 32; ++i)
                    if (buf[i] != 0xA5)
                        return accept(0, 0, 43);

                return accept(0, 0, 0);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set stale entropy hook"),
            HSFEE);
        env.close();

        OpenView view{*env.current()};
        auto const openSeq = view.info().seq;
        BEAST_REQUIRE(openSeq > 2);
        auto const entropy = view.consensusEntropy();
        BEAST_REQUIRE(entropy);
        auto replacement = std::make_shared<STTx>(*entropy);
        replacement->setFieldU32(sfLedgerSequence, openSeq - 2);
        view.rawSetConsensusEntropy(replacement);

        auto const hookArray = view.read(keylet::hook(alice.id()));
        BEAST_REQUIRE(hookArray);
        auto const& hooks = hookArray->getFieldArray(sfHooks);
        BEAST_REQUIRE(hooks.size() == 1);
        auto const& hookObj = hooks[0];
        auto const hookHash = hookObj.getFieldH256(sfHookHash);
        auto const hookDefView = view.read(keylet::hookDefinition(hookHash));
        BEAST_REQUIRE(hookDefView);
        auto const hookDef =
            std::make_shared<SLE>(*hookDefView, hookDefView->key());

        STTx const invokeTx = STTx(ttINVOKE, [&](STObject& obj) {
            obj.setAccountID(sfAccount, alice.id());
        });
        ApplyContext applyCtx{
            env.app(),
            view,
            invokeTx,
            tesSUCCESS,
            env.current()->fees().base,
            tapNONE,
            env.journal};
        hook::HookStateMap stateMap;
        std::map<std::vector<uint8_t>, std::vector<uint8_t>> parameters;
        BEAST_REQUIRE(!hook::gatherHookParameters(
            hookDef, hookObj, parameters, env.journal));
        auto const hookNamespace = hookObj.isFieldPresent(sfHookNamespace)
            ? hookObj.getFieldH256(sfHookNamespace)
            : hookDef->getFieldH256(sfHookNamespace);
        auto result = hook::apply(
            hookDef->getFieldH256(sfHookSetTxnID),
            hookHash,
            hook::getHookCanEmit(hookObj, hookDef),
            hookNamespace,
            hookDef->getFieldVL(sfCreateCode),
            parameters,
            {},
            stateMap,
            applyCtx,
            alice.id(),
            hookDef->isFieldPresent(sfHookCallbackFee),
            false,
            true,
            0,
            0,
            {},
            true);

        BEAST_EXPECT(result.exitType == hook_api::ExitType::ACCEPT);
        BEAST_EXPECT(result.exitCode == 0);
        BEAST_EXPECT(result.rngCallCounter == 0);
    }

    void
    testDiceTierRequirementNotMet()
    {
        testcase("Hook entropy_cr_dice() fails closed below min_tier");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        ConsensusTestConfig cfg;
        cfg.standaloneEntropyTier = entropyTierValidatorQuorum;
        cfg.standaloneEntropyCount = 19;
        cfg.standaloneEntropyDenominator = 20;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        auto const input = recordedEntropy(env);
        BEAST_REQUIRE(input);
        BEAST_EXPECT(
            input->getFieldU8(sfEntropyTier) == entropyTierValidatorQuorum);
        BEAST_EXPECT(input->getFieldU16(sfEntropyCount) == 19);
        BEAST_EXPECT(input->getFieldU16(sfEntropyDenominator) == 20);
        BEAST_EXPECT(
            input->getFieldVL(sfEntropyContributors) ==
            standaloneContributorMask(20, 19));

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t result = entropy_cr_dice(6, 4, 0);
                return accept(0, 0, result);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set entropy_cr_dice-tier-requirement hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test entropy_cr_dice min_tier unmet"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));

        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);

        BEAST_EXPECT(hookReturnCode(hookExecutions[0]) == -48);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testDiceWithoutAmendment()
    {
        testcase("Hook entropy imports independently require amendment");
        using namespace jtx;

        Env env{*this, supported_amendments()};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_EXPECT(!recordedEntropy(env));

        TestHook diceHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                return accept(0, 0, entropy_cr_dice(6, 3, 0));
            }
        )[test.hook]"];

        TestHook randomHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t flags);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t buf[32];
                return accept(0, 0, entropy_cr_random((uint32_t)buf, 32, 3, 0));
            }
        )[test.hook]"];

        TestHook statusHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_status(void);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                return accept(0, 0, entropy_cr_status());
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(diceHook, overrideFlag)}}, 0),
            M("set entropy_cr_dice-no-amendment hook"),
            HSFEE,
            ter(temMALFORMED));
        env(ripple::test::jtx::hook(
                alice, {{hso(randomHook, overrideFlag)}}, 0),
            M("set entropy_cr_random-no-amendment hook"),
            HSFEE,
            ter(temMALFORMED));
        env(ripple::test::jtx::hook(
                alice, {{hso(statusHook, overrideFlag)}}, 0),
            M("set entropy-status-no-amendment hook"),
            HSFEE,
            ter(temMALFORMED));
    }

    void
    testRetiredImportNamesRejected()
    {
        testcase("Retired generic entropy import names remain unavailable");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook diceHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t dice(uint32_t sides, uint32_t min_tier);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                return accept(0, 0, dice(6, 3));
            }
        )[test.hook]"];

        TestHook randomHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t buf[32];
                return accept(0, 0, random((uint32_t)buf, 32, 3));
            }
        )[test.hook]"];

        TestHook statusHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_status(void);

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                return accept(0, 0, entropy_status());
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(diceHook, overrideFlag)}}, 0),
            M("reject retired dice import"),
            HSFEE,
            ter(temMALFORMED));
        env(ripple::test::jtx::hook(
                alice, {{hso(randomHook, overrideFlag)}}, 0),
            M("reject retired random import"),
            HSFEE,
            ter(temMALFORMED));
        env(ripple::test::jtx::hook(
                alice, {{hso(statusHook, overrideFlag)}}, 0),
            M("reject retired entropy_status import"),
            HSFEE,
            ter(temMALFORMED));
    }

    void
    testRandomTierRequirementNotMet()
    {
        testcase("Hook entropy_cr_random() fails before write below min_tier");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        ConsensusTestConfig cfg;
        cfg.standaloneEntropyTier = entropyTierValidatorQuorum;
        cfg.standaloneEntropyCount = 19;
        cfg.standaloneEntropyDenominator = 20;
        env.app().getRuntimeConfig().setGlobalConfig(cfg);

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t flags);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
            #define TOO_LITTLE_ENTROPY (-48)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t buf[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    buf[i] = 0xA5;

                for (uint32_t flags = 0; GUARD(4), flags < 4; ++flags)
                    if (entropy_cr_random((uint32_t)buf, 32, 4, flags) != TOO_LITTLE_ENTROPY)
                        return accept(0, 0, 30 + flags);
                for (int i = 0; GUARD(32), i < 32; ++i)
                    if (buf[i] != 0xA5)
                        return accept(0, 0, 31);

                return accept(0, 0, 0);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set entropy_cr_random-tier-requirement hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("test entropy_cr_random min_tier unmet"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);
        BEAST_EXPECT(hookReturnCode(hookExecutions[0]) == 0);
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testInvalidEntropyRequirements()
    {
        testcase(
            "Hook entropy_cr_dice/entropy_cr_random reject invalid entropy "
            "requirements");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"alice"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(recordedEntropy(env));

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            extern int64_t entropy_cr_random(uint32_t write_ptr, uint32_t write_len, uint32_t min_tier, uint32_t flags);
            #define INVALID_ARGUMENT (-7)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t buf[32];

                int64_t bad_min_tier = entropy_cr_dice(6, 0, 0);
                if (bad_min_tier != INVALID_ARGUMENT)
                    return accept(0, 0, 100);

                int64_t bad_high_tier = entropy_cr_dice(6, 5, 0);
                if (bad_high_tier != INVALID_ARGUMENT)
                    return accept(0, 0, 101);

                int64_t bad_random_low = entropy_cr_random((uint32_t)buf, 32, 0, 0);
                if (bad_random_low != INVALID_ARGUMENT)
                    return accept(0, 0, 102);

                int64_t bad_random_high = entropy_cr_random((uint32_t)buf, 32, 5, 0);
                if (bad_random_high != INVALID_ARGUMENT)
                    return accept(0, 0, 103);

                if (entropy_cr_dice(6, 3, 4) != INVALID_ARGUMENT ||
                    entropy_cr_dice(6, 3, 0xFFFFFFFFU) != INVALID_ARGUMENT ||
                    entropy_cr_random((uint32_t)buf, 32, 3, 4) != INVALID_ARGUMENT ||
                    entropy_cr_random((uint32_t)buf, 32, 3, 0xFFFFFFFFU) != INVALID_ARGUMENT)
                    return accept(0, 0, 104);

                if (entropy_cr_dice(6, 0, 1) != INVALID_ARGUMENT ||
                    entropy_cr_dice(6, 5, 1) != INVALID_ARGUMENT ||
                    entropy_cr_random((uint32_t)buf, 32, 0, 1) != INVALID_ARGUMENT ||
                    entropy_cr_random((uint32_t)buf, 32, 5, 1) != INVALID_ARGUMENT)
                    return accept(0, 0, 105);

                // Failed calls must not consume the shared RNG call counter.
                // The test pins the first valid draw as a known-answer vector.
                return accept(0, 0, entropy_cr_dice(1000000, 4, 0));
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

        auto const entropy = recordedEntropy(env);
        BEAST_REQUIRE(entropy);
        auto const firstBlock = sha512Half(
            entropy->getFieldU32(sfLedgerSequence),
            env.tx()->getTransactionID(),
            alice.id(),
            hookExecutions[0].getFieldH256(sfHookHash),
            alice.id(),
            std::uint8_t{0},
            std::string{"strong"},
            std::string{"direct"},
            entropy->getFieldH256(sfDigest),
            std::uint64_t{0});
        auto const actual = hookReturnCode(hookExecutions[0]);
        BEAST_EXPECT(actual == expectedDice(firstBlock, 1000000));
        BEAST_EXPECT(hookExecutions[0].getFieldU8(sfHookResult) == 3);
    }

    void
    testEntropyDrawPoliciesAcrossStakeholders()
    {
        testcase(
            "Guarded refusal and permissive draws preserve stakeholder vetoes");
        using namespace jtx;

        TestHook gameHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            extern int64_t otxn_field(uint32_t, uint32_t, uint32_t);
            extern int64_t state_set(uint32_t read_ptr, uint32_t read_len, uint32_t kread_ptr, uint32_t kread_len);
            #define SBUF(x) (uint32_t)(x), sizeof(x)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                // Test-only selector: exercise all application policies.
                uint8_t tag[4] = {0};
                otxn_field((uint32_t)tag, sizeof(tag), (2U << 16U) + 3U);
                int64_t roll = entropy_cr_dice(6, 3, tag[3]);
                if (roll < 0)
                    return rollback(0, 0, roll);

                uint8_t key[32] = {
                    'r','n','g','-','s','t','r','o','n','g','-','v','e','t','o'};
                uint8_t marker[1] = {0xA5U};
                if (state_set(SBUF(marker), SBUF(key)) != 1)
                    return rollback(0, 0, 100);

                return accept(0, 0, roll);
            }
        )[test.hook]"];

        TestHook plainGameHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t state_set(uint32_t read_ptr, uint32_t read_len, uint32_t kread_ptr, uint32_t kread_len);
            #define SBUF(x) (uint32_t)(x), sizeof(x)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                uint8_t key[32] = {
                    'r','n','g','-','s','t','r','o','n','g','-','v','e','t','o'};
                uint8_t marker[1] = {0xA5U};
                if (state_set(SBUF(marker), SBUF(key)) != 1)
                    return rollback(0, 0, 100);

                return accept(0, 0, 77);
            }
        )[test.hook]"];

        TestHook issuerHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t otxn_field(uint32_t write_ptr, uint32_t write_len, uint32_t field_id);
            extern int64_t state_foreign(
                uint32_t write_ptr,
                uint32_t write_len,
                uint32_t kread_ptr,
                uint32_t kread_len,
                uint32_t nread_ptr,
                uint32_t nread_len,
                uint32_t aread_ptr,
                uint32_t aread_len);
            #define GUARD(maxiter) _g((1ULL << 31U) + __LINE__, (maxiter)+1)
            #define SBUF(x) (uint32_t)(x), sizeof(x)
            #define sfDestination ((8U << 16U) + 3U)
            #define DOESNT_EXIST (-5)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                uint8_t destination[20];
                if (otxn_field(SBUF(destination), sfDestination) != 20)
                    return rollback(0, 0, 200);

                uint8_t key[32] = {
                    'r','n','g','-','s','t','r','o','n','g','-','v','e','t','o'};
                uint8_t ns[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    ns[i] = 0;

                uint8_t marker[1] = {0};
                int64_t bytes = state_foreign(
                    SBUF(marker), SBUF(key), SBUF(ns), SBUF(destination));

                // This is the post-selection vector: a later strong
                // burnable-token issuer can see destination pending state
                // created after an entropy draw and tries to veto.
                if (bytes == 1 && marker[0] == 0xA5U)
                    return rollback((uint32_t)"issuer veto", 11, 9001);

                if (bytes != DOESNT_EXIST && bytes != 1)
                    return rollback(0, 0, bytes);

                return accept(0, 0, bytes);
            }
        )[test.hook]"];

        std::array<std::uint8_t, 32> markerKeyBytes{};
        std::string const markerKeyPrefix = "rng-strong-veto";
        std::copy(
            markerKeyPrefix.begin(),
            markerKeyPrefix.end(),
            markerKeyBytes.begin());
        auto const markerKey = uint256::fromVoid(markerKeyBytes.data());

        {
            Env env{
                *this,
                envconfig(),
                supported_amendments() | featureConsensusEntropy,
                nullptr};

            auto const player = Account{"player"};
            auto const game = Account{"game"};
            auto const issuer = Account{"issuer"};
            env.fund(XRP(10000), player, game, issuer);
            env.close();

            std::string const uri = "rng-terminal-veto-control";
            auto const tokenID = uritoken::tokenid(issuer, uri);
            auto const hexTokenID = strHex(tokenID);

            env(uritoken::mint(issuer, uri),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env(uritoken::sell(issuer, hexTokenID),
                uritoken::amt(XRP(1)),
                uritoken::dest(player),
                ter(tesSUCCESS));
            env.close();

            env(uritoken::buy(player, hexTokenID),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(ripple::test::jtx::hook(
                    game, {{hso(plainGameHook, overrideFlag)}}, 0),
                M("set non-rng game hook"),
                HSFEE);
            env.close();

            env(ripple::test::jtx::hook(
                    issuer, {{hso(issuerHook, overrideFlag)}}, 0),
                M("set vetoing issuer hook"),
                HSFEE);
            env.close();

            auto const preToken = env.le(Keylet{ltURI_TOKEN, tokenID});
            BEAST_REQUIRE(preToken);
            BEAST_EXPECT(preToken->getAccountID(sfOwner) == player.id());
            BEAST_EXPECT(preToken->getAccountID(sfIssuer) == issuer.id());
            BEAST_EXPECT(preToken->getFlags() & lsfBurnable);

            env(remit::remit(player, game),
                remit::token_ids({hexTokenID}),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));

            auto meta = env.meta();
            BEAST_REQUIRE(meta);
            BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));
            auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(hookExecutions.size() == 2);
            BEAST_EXPECT(
                hookExecutions[0].getAccountID(sfHookAccount) == game.id());
            BEAST_EXPECT(
                hookExecutions[0].getFieldU8(sfHookResult) ==
                static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
            BEAST_EXPECT(hookReturnCode(hookExecutions[0]) == 77);
            BEAST_EXPECT(
                hookExecutions[1].getAccountID(sfHookAccount) == issuer.id());
            BEAST_EXPECT(
                hookExecutions[1].getFieldU8(sfHookResult) ==
                static_cast<std::uint8_t>(hook_api::ExitType::ROLLBACK));
            BEAST_EXPECT(hookReturnCode(hookExecutions[1]) == 9001);
            BEAST_EXPECT(hookReturnString(hookExecutions[1]) == "issuer veto");

            BEAST_EXPECT(
                !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));

            auto const postToken = env.le(Keylet{ltURI_TOKEN, tokenID});
            BEAST_REQUIRE(postToken);
            BEAST_EXPECT(postToken->getAccountID(sfOwner) == player.id());
        }

        {
            Env env{
                *this,
                envconfig(),
                supported_amendments() | featureConsensusEntropy,
                nullptr};

            auto const player = Account{"player"};
            auto const game = Account{"game"};
            auto const issuer = Account{"issuer"};
            env.fund(XRP(10000), player, game, issuer);
            env.close();

            BEAST_REQUIRE(recordedEntropy(env));

            std::string const uri = "rng-terminal-veto-token";
            auto const tokenID = uritoken::tokenid(issuer, uri);
            auto const hexTokenID = strHex(tokenID);

            env(uritoken::mint(issuer, uri),
                txflags(tfBurnable),
                ter(tesSUCCESS));
            env(uritoken::sell(issuer, hexTokenID),
                uritoken::amt(XRP(1)),
                uritoken::dest(player),
                ter(tesSUCCESS));
            env.close();

            env(uritoken::buy(player, hexTokenID),
                uritoken::amt(XRP(1)),
                ter(tesSUCCESS));
            env.close();

            env(ripple::test::jtx::hook(
                    game, {{hso(gameHook, overrideFlag)}}, 0),
                M("set rng game hook"),
                HSFEE);
            env.close();

            env(ripple::test::jtx::hook(
                    issuer, {{hso(issuerHook, overrideFlag)}}, 0),
                M("set vetoing issuer hook"),
                HSFEE);
            env.close();

            auto const preToken = env.le(Keylet{ltURI_TOKEN, tokenID});
            BEAST_REQUIRE(preToken);
            BEAST_EXPECT(preToken->getAccountID(sfOwner) == player.id());
            BEAST_EXPECT(preToken->getAccountID(sfIssuer) == issuer.id());
            BEAST_EXPECT(preToken->getFlags() & lsfBurnable);

            BEAST_EXPECT(
                !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));

            env(remit::remit(player, game),
                remit::token_ids({hexTokenID}),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));

            auto meta = env.meta();
            BEAST_REQUIRE(meta);
            BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));
            auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(hookExecutions.size() == 1);
            BEAST_EXPECT(
                hookExecutions[0].getAccountID(sfHookAccount) == game.id());
            BEAST_EXPECT(
                hookExecutions[0].getFieldU8(sfHookResult) ==
                static_cast<std::uint8_t>(hook_api::ExitType::ROLLBACK));
            BEAST_EXPECT(
                hookReturnCode(hookExecutions[0]) ==
                static_cast<int64_t>(
                    hook_api::hook_return_code::LATER_STRONG_HOOK));

            BEAST_EXPECT(
                !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));

            auto const token = env.le(Keylet{ltURI_TOKEN, tokenID});
            BEAST_REQUIRE(token);
            BEAST_EXPECT(token->getAccountID(sfOwner) == player.id());

            // Same-account permission never grants a foreign issuer a veto.
            auto accountOnly = remit::remit(player, game);
            accountOnly["SourceTag"] =
                hook_api::ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO;
            env(accountOnly,
                remit::token_ids({hexTokenID}),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));
            auto const accountRefused =
                env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(accountRefused.size() == 1);
            BEAST_EXPECT(hookReturnCode(accountRefused[0]) == -49);
            BEAST_EXPECT(
                !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));

            // Explicit opt-in lets the issuer execute and exercise its veto.
            auto permissive = remit::remit(player, game);
            permissive["SourceTag"] = 1;
            env(permissive,
                remit::token_ids({hexTokenID}),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));
            auto vetoes = env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(vetoes.size() == 2);
            BEAST_EXPECT(
                vetoes[0].getFieldU8(sfHookResult) ==
                static_cast<uint8_t>(hook_api::ExitType::ACCEPT));
            BEAST_EXPECT(hookReturnCode(vetoes[1]) == 9001);
            BEAST_EXPECT(
                !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));
            BEAST_EXPECT(
                env.le(Keylet{ltURI_TOKEN, tokenID})->getAccountID(sfOwner) ==
                player.id());

            // ANY includes SAME_ACCOUNT, whether or not both bits are set.
            auto combined = permissive;
            combined["SourceTag"] = hook_api::ENTROPY_ALLOW_ANY_STRONG_VETO |
                hook_api::ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO;
            env(combined,
                remit::token_ids({hexTokenID}),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));
            auto const combinedVeto =
                env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(combinedVeto.size() == 2);
            BEAST_EXPECT(hookReturnCode(combinedVeto[1]) == 9001);

            // Terminal destination draw remains usable when there is no
            // subsequent strong stakeholder Hook.
            env(pay(player, game, XRP(1)), fee(XRP(1)));
            BEAST_EXPECT(
                env.meta()->getFieldArray(sfHookExecutions).size() == 1);
            BEAST_EXPECT(
                env.le(keylet::hookState(game.id(), markerKey, beast::zero)));

            // A sender drawing first cannot bypass the destination's Hook,
            // even when that destination would itself accept the payment.
            env(ripple::test::jtx::hook(
                    player, {{hso(gameHook, overrideFlag)}}, 0),
                HSFEE);
            env.close();
            auto const gameBalance = env.balance(game);
            env(pay(player, game, XRP(1)), fee(XRP(1)), ter(tecHOOK_REJECTED));
            auto const senderExecutions =
                env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(senderExecutions.size() == 1);
            BEAST_EXPECT(
                senderExecutions[0].getAccountID(sfHookAccount) == player.id());
            BEAST_EXPECT(env.balance(game) == gameBalance);
            BEAST_EXPECT(!env.le(
                keylet::hookState(player.id(), markerKey, beast::zero)));
            auto accountPayment = pay(player, game, XRP(1));
            accountPayment["SourceTag"] =
                hook_api::ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO;
            env(accountPayment, fee(XRP(1)), ter(tecHOOK_REJECTED));
            auto const accountSender =
                env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(accountSender.size() == 1);
            BEAST_EXPECT(hookReturnCode(accountSender[0]) == -49);
            BEAST_EXPECT(env.balance(game) == gameBalance);
            // Both sender and destination may opt in, preserving issuer veto.
            env(permissive,
                remit::token_ids({hexTokenID}),
                fee(XRP(1)),
                ter(tecHOOK_REJECTED));
            auto const multiple = env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(multiple.size() == 3);
            BEAST_EXPECT(
                multiple[0].getAccountID(sfHookAccount) == player.id());
            BEAST_EXPECT(multiple[1].getAccountID(sfHookAccount) == game.id());
            BEAST_EXPECT(hookReturnCode(multiple[2]) == 9001);
        }
    }

    void
    testGuardedDrawSameChainRefusal()
    {
        testcase(
            "Guarded entropy draw refuses a later strong same-account Hook");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const game = Account{"samechain"};
        env.fund(XRP(10000), game);
        env.close();

        BEAST_REQUIRE(recordedEntropy(env));

        TestHook gameHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            extern int64_t state_set(uint32_t read_ptr, uint32_t read_len, uint32_t kread_ptr, uint32_t kread_len);
            #define SBUF(x) (uint32_t)(x), sizeof(x)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                int64_t roll = entropy_cr_dice(6, 3, 0);
                if (roll < 0)
                    return rollback(0, 0, roll);

                uint8_t key[32] = {
                    'r','n','g','-','s','a','m','e','-','c','h','a','i','n'};
                uint8_t marker[1] = {0xA5U};
                if (state_set(SBUF(marker), SBUF(key)) != 1)
                    return rollback(0, 0, 100);

                return accept(0, 0, roll);
            }
        )[test.hook]"];

        TestHook vetoHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t state(uint32_t write_ptr, uint32_t write_len, uint32_t kread_ptr, uint32_t kread_len);
            #define SBUF(x) (uint32_t)(x), sizeof(x)
            #define DOESNT_EXIST (-5)

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                uint8_t key[32] = {
                    'r','n','g','-','s','a','m','e','-','c','h','a','i','n'};
                uint8_t marker[1] = {0};
                int64_t bytes = state(SBUF(marker), SBUF(key));
                if (bytes == 1 && marker[0] == 0xA5U)
                    return rollback((uint32_t)"same chain veto", 15, 9101);

                if (bytes != DOESNT_EXIST && bytes != 1)
                    return rollback(0, 0, bytes);

                return accept(0, 0, bytes);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(
                game,
                {{hso(gameHook, overrideFlag), hso(vetoHook, overrideFlag)}},
                0),
            M("set same-chain rng and veto hooks"),
            HSFEE);
        env.close();

        std::array<std::uint8_t, 32> markerKeyBytes{};
        std::string const markerKeyPrefix = "rng-same-chain";
        std::copy(
            markerKeyPrefix.begin(),
            markerKeyPrefix.end(),
            markerKeyBytes.begin());
        auto const markerKey = uint256::fromVoid(markerKeyBytes.data());

        BEAST_EXPECT(
            !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = game.human();
        env(invoke,
            M("same-chain drawing Hook rejects guarded refusal"),
            fee(XRP(1)),
            ter(tecHOOK_REJECTED));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 1);
        BEAST_EXPECT(
            hookExecutions[0].getAccountID(sfHookAccount) == game.id());
        BEAST_EXPECT(
            hookExecutions[0].getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ROLLBACK));
        BEAST_EXPECT(
            hookReturnCode(hookExecutions[0]) ==
            static_cast<int64_t>(
                hook_api::hook_return_code::LATER_STRONG_HOOK));

        BEAST_EXPECT(
            !env.le(keylet::hookState(game.id(), markerKey, beast::zero)));
    }

    void
    testEntropyCompositionFilters()
    {
        testcase("Entropy composition admission, skips, and inactive Hooks");
        using namespace jtx;

        TestHook drawingHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_random(uint32_t, uint32_t, uint32_t, uint32_t flags);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t, uint32_t flags);
            extern int64_t entropy_cr_status(void);
            extern int64_t otxn_field(uint32_t, uint32_t, uint32_t);
            extern int64_t hook_hash(uint32_t, uint32_t, int32_t);
            extern int64_t hook_skip(uint32_t, uint32_t, uint32_t);
            #define SBUF(x) (uint32_t)(x), sizeof(x)
            #define sfSourceTag ((2U << 16U) + 3U)
            #define GUARD(n) _g((1U << 31U) + __LINE__, (n)+1)
            #define LATER_STRONG_HOOK (-49)
            #define ENTROPY_ALLOW_ANY_STRONG_VETO (1U << 0)
            #define ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO (1U << 1)

            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t tag[4];
                if (otxn_field(SBUF(tag), sfSourceTag) != 4)
                    return rollback(0, 0, 100);

                if (tag[3] == 0)
                    return accept(0, 0, entropy_cr_status());
                if (tag[3] == 1)
                    return accept(0, 0, entropy_cr_dice(0, 3, 0));

                // Even a skip requested before the draw cannot change the
                // conservative terminal-position permission.
                if (tag[3] == 5)
                {
                    uint8_t hash[32];
                    if (hook_hash(SBUF(hash), 1) != 32 ||
                        hook_skip(SBUF(hash), 0) != 1)
                        return rollback(0, 0, 104);
                }

                uint8_t bytes[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    bytes[i] = 0xA5U;
                uint32_t flags = (tag[3] == 3 || tag[3] == 4 || tag[3] == 7)
                    ? ENTROPY_ALLOW_ANY_STRONG_VETO : 0;
                if (tag[3] >= 8)
                    flags = ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO;
                int64_t result = entropy_cr_random(SBUF(bytes), 3, flags);
                if (result == LATER_STRONG_HOOK)
                {
                    for (int i = 0; GUARD(32), i < 32; ++i)
                        if (bytes[i] != 0xA5U)
                            return rollback(0, 0, 105);
                }
                else if (result != 32)
                    return rollback(0, 0, result);

                if (tag[3] == 6)
                {
                    if (result != LATER_STRONG_HOOK)
                        return rollback(0, 0, 106);
                    // The refused call must not consume draw index zero.
                    return accept(0, 0, entropy_cr_dice(
                        1000000, 3, ENTROPY_ALLOW_ANY_STRONG_VETO));
                }
                if (tag[3] == 7 || tag[3] == 10)
                    return accept(0, 0, entropy_cr_dice(6, 3, 0));

                if (tag[3] == 3 || tag[3] == 8)
                {
                    uint8_t hash[32];
                    if (hook_hash(SBUF(hash), 1) != 32)
                        return rollback(0, 0, 102);
                    if (hook_skip(SBUF(hash), 0) != 1)
                        return rollback(0, 0, 103);
                }
                return accept(0, 0, result);
            }
        )[test.hook]"];

        TestHook acceptingHook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t result = entropy_cr_dice(1, 3, 0);
                if (result != 0)
                    return rollback(0, 0, result);
                return accept(0, 0, 77);
            }
        )[test.hook]"];

        Env env{*this, supported_amendments() | featureConsensusEntropy};
        auto const alice = Account{"composition"};
        auto const bob = Account{"unhooked"};
        env.fund(XRP(10000), alice, bob);
        env.close();
        env(ripple::test::jtx::hook(
                alice,
                {{hso(drawingHook, overrideFlag),
                  hso(acceptingHook, overrideFlag)}},
                0),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();

        // Refusal is an API error, not mandatory transaction rejection.
        // Permissive calls preserve normal skip/veto semantics and cannot
        // grant permission to a subsequent guarded call in this same Hook.
        for (unsigned mode = 0; mode < 11; ++mode)
        {
            invoke["SourceTag"] = mode;
            env(invoke, fee(XRP(1)));
            auto const meta = env.meta();
            BEAST_REQUIRE(meta);
            auto const executions = meta->getFieldArray(sfHookExecutions);
            bool const skipped = mode == 3 || mode == 5 || mode == 8;
            BEAST_REQUIRE(executions.size() == (skipped ? 1 : 2));
            BEAST_EXPECT(
                executions[0].getFieldU8(sfHookResult) ==
                static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
            if (!skipped)
                BEAST_EXPECT(hookReturnCode(executions[1]) == 77);
            if (mode == 2 || mode == 5 || mode == 7 || mode == 10)
                BEAST_EXPECT(hookReturnCode(executions[0]) == -49);
            if (mode == 3 || mode == 4 || mode == 8 || mode == 9)
                BEAST_EXPECT(hookReturnCode(executions[0]) == 32);
            if (mode == 6)
            {
                auto const entropy = recordedEntropy(env);
                BEAST_REQUIRE(entropy);
                auto const block = sha512Half(
                    entropy->getFieldU32(sfLedgerSequence),
                    env.tx()->getTransactionID(),
                    alice.id(),
                    executions[0].getFieldH256(sfHookHash),
                    alice.id(),
                    std::uint8_t{0},
                    std::string{"strong"},
                    std::string{"direct"},
                    entropy->getFieldH256(sfDigest),
                    std::uint64_t{0});
                BEAST_EXPECT(
                    hookReturnCode(executions[0]) ==
                    expectedDice(block, 1000000));
            }
        }

        // A same-account guard must preserve a later helper's normal ability
        // to skip another Hook on this account (not just the drawing Hook's
        // own skips). The immutable plan still includes the final slot.
        TestHook skippingHelper = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t hook_hash(uint32_t, uint32_t, int32_t);
            extern int64_t hook_skip(uint32_t, uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t hash[32];
                if (hook_hash((uint32_t)hash, 32, 2) != 32 ||
                    hook_skip((uint32_t)hash, 32, 0) != 1)
                    return rollback(0, 0, 100);
                return accept(0, 0, 88);
            }
        )[test.hook]"];
        env(ripple::test::jtx::hook(
                alice,
                {{hso(drawingHook, overrideFlag),
                  hso(skippingHelper, overrideFlag),
                  hso(acceptingHook, overrideFlag)}},
                0),
            HSFEE);
        env.close();
        invoke["SourceTag"] = 9;
        env(invoke, fee(XRP(1)));
        BEAST_REQUIRE(env.meta());
        auto const helperSkipped = env.meta()->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(helperSkipped.size() == 2);
        BEAST_EXPECT(hookReturnCode(helperSkipped[0]) == 32);
        BEAST_EXPECT(hookReturnCode(helperSkipped[1]) == 88);

        // Only eligible later Hooks prohibit the composition. An inactive
        // HookOn, unmatched HookName, or blank slot is not a veto authority.
        for (unsigned filter = 0; filter < 3; ++filter)
        {
            auto tail = hso(acceptingHook, overrideFlag);
            if (filter == 0)
                tail[jss::HookOn] = to_string(UINT256_BIT[ttINVOKE]);
            else if (filter == 1)
                tail[jss::HookName] = "7465726D";
            else
                tail = hso_delete();

            env(ripple::test::jtx::hook(
                    alice,
                    {{hso(drawingHook, overrideFlag), tail, hso_delete()}},
                    0),
                HSFEE);
            env.close();
            invoke["SourceTag"] = 2;
            env(invoke, fee(XRP(1)));
            BEAST_REQUIRE(env.meta());
            auto const executions = env.meta()->getFieldArray(sfHookExecutions);
            BEAST_REQUIRE(executions.size() == 1);
            BEAST_EXPECT(hookReturnCode(executions[0]) == 32);

            if (filter == 1)
            {
                // Matching that name makes the later Hook eligible again.
                invoke[jss::HookName] = "7465726D";
                env(invoke, fee(XRP(1)));
                BEAST_REQUIRE(env.meta());
                auto const matched =
                    env.meta()->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(matched.size() == 2);
                BEAST_EXPECT(hookReturnCode(matched[0]) == -49);
                BEAST_EXPECT(hookReturnCode(matched[1]) == 77);
                invoke.removeMember(jss::HookName);
            }
        }

        // A later strong stakeholder with no Hook is also harmless.
        invoke[jss::Destination] = bob.human();
        env(invoke, fee(XRP(1)));
        BEAST_REQUIRE(env.meta());
        BEAST_EXPECT(env.meta()->getFieldArray(sfHookExecutions).size() == 1);

        // An installed but inactive stakeholder chain has zero execution fee
        // and must not disqualify the sender's guarded draw.
        auto inactive = hso(acceptingHook, overrideFlag);
        inactive[jss::HookOn] = to_string(UINT256_BIT[ttINVOKE]);
        env(ripple::test::jtx::hook(bob, {{inactive}}, 0), HSFEE);
        env.close();
        env(invoke, fee(XRP(1)));
        BEAST_REQUIRE(env.meta());
        auto const inactiveExecutions =
            env.meta()->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(inactiveExecutions.size() == 1);
        BEAST_EXPECT(hookReturnCode(inactiveExecutions[0]) == 32);

        // A Remit Inform account is weak-only. Its eligible collecting Hook
        // may draw after the strong sender without affecting terminality.
        Account const beneficiary{"beneficiary"};
        env.fund(XRP(10000), beneficiary);
        auto collector = hso(acceptingHook, overrideFlag);
        collector[jss::Flags] = hsfOVERRIDE | hsfCOLLECT;
        env(ripple::test::jtx::hook(bob, {{collector}}, 0), HSFEE);
        env(fset(bob, asfTshCollect), fee(XRP(1)));
        env.close();
        auto weakRemit = remit::remit(alice, beneficiary);
        weakRemit["Inform"] = bob.human();
        weakRemit["SourceTag"] = 2;
        env(weakRemit, fee(XRP(1)));
        BEAST_REQUIRE(env.meta());
        auto const weakExecutions = env.meta()->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(weakExecutions.size() == 2);
        BEAST_EXPECT(
            weakExecutions[0].getAccountID(sfHookAccount) == alice.id());
        BEAST_EXPECT(hookReturnCode(weakExecutions[0]) == 32);
        BEAST_EXPECT(weakExecutions[1].getAccountID(sfHookAccount) == bob.id());
        BEAST_EXPECT(hookReturnCode(weakExecutions[1]) == 77);
    }

    void
    testEntropyDrawDoesNotBlockWeakAgainAsWeak()
    {
        testcase("Hook entropy draw does not block weak again-as-weak");
        using namespace jtx;

        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        auto const alice = Account{"weakrng"};
        env.fund(XRP(10000), alice);
        env.close();

        BEAST_REQUIRE(recordedEntropy(env));

        TestHook hook = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t rollback(uint32_t read_ptr, uint32_t read_len, int64_t error_code);
            extern int64_t entropy_cr_dice(uint32_t sides, uint32_t min_tier, uint32_t flags);
            extern int64_t hook_again(void);

            int64_t hook(uint32_t r)
            {
                _g(1,1);

                if (r > 0)
                    return accept((uint32_t)"weak", 4, 42);

                int64_t roll = entropy_cr_dice(6, 3, 0);
                if (roll < 0)
                    return rollback(0, 0, roll);

                if (hook_again() != 1)
                    return rollback((uint32_t)"again failed", 12, 100);

                return accept((uint32_t)"strong", 6, roll);
            }
        )[test.hook]"];

        env(ripple::test::jtx::hook(alice, {{hso(hook, overrideFlag)}}, 0),
            M("set rng hook_again hook"),
            HSFEE);
        env.close();

        Json::Value invoke;
        invoke[jss::TransactionType] = "Invoke";
        invoke[jss::Account] = alice.human();
        env(invoke, M("rng strong plus weak again-as-weak"), fee(XRP(1)));

        auto meta = env.meta();
        BEAST_REQUIRE(meta);
        BEAST_REQUIRE(meta->isFieldPresent(sfHookExecutions));
        auto const hookExecutions = meta->getFieldArray(sfHookExecutions);
        BEAST_REQUIRE(hookExecutions.size() == 2);
        BEAST_EXPECT(
            hookExecutions[0].getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));
        BEAST_EXPECT(
            hookExecutions[1].getFieldU8(sfHookResult) ==
            static_cast<std::uint8_t>(hook_api::ExitType::ACCEPT));

        auto const entropy = recordedEntropy(env);
        BEAST_REQUIRE(entropy);
        auto const firstBlock = sha512Half(
            entropy->getFieldU32(sfLedgerSequence),
            env.tx()->getTransactionID(),
            alice.id(),
            hookExecutions[0].getFieldH256(sfHookHash),
            alice.id(),
            std::uint8_t{0},
            std::string{"strong"},
            std::string{"direct"},
            entropy->getFieldH256(sfDigest),
            std::uint64_t{0});
        BEAST_EXPECT(
            hookReturnCode(hookExecutions[0]) == expectedDice(firstBlock, 6));
        BEAST_EXPECT(hookReturnString(hookExecutions[0]) == "strong");
        BEAST_EXPECT(hookReturnCode(hookExecutions[1]) == 42);
        BEAST_EXPECT(hookReturnString(hookExecutions[1]) == "weak");
    }

    void
    testEntropyHostAdmission()
    {
        testcase("Entropy policy accounting and old-arity runtime failure");
        using namespace jtx;
        Env env{*this, supported_amendments() | featureConsensusEntropy};
        Account const alice{"admission"};
        env.fund(XRP(10000), alice);
        env.close();

        TestHook mixed = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t, uint32_t);
            extern int64_t entropy_cr_random(uint32_t, uint32_t, uint32_t, uint32_t);
            extern int64_t entropy_cr_status(void);
            #define GUARD(n) _g((1U << 31U) + __LINE__, (n)+1)
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                int64_t status = entropy_cr_status();
                int64_t guarded = entropy_cr_dice(1, 3, 0);
                if (guarded != 0 && guarded != -49)
                    return rollback(0, 0, 100);
                int64_t accountGuarded = entropy_cr_dice(1, 3, 2);
                if (accountGuarded != 0 && accountGuarded != -49)
                    return rollback(0, 0, 106);
                if (entropy_cr_dice(1, 3, 1) != 0)
                    return rollback(0, 0, 101);
                uint8_t bytes[32];
                for (int i = 0; GUARD(32), i < 32; ++i)
                    bytes[i] = 0xA5;
                int64_t count = entropy_cr_random((uint32_t)bytes, 32, 3, 0);
                if (count != (guarded == 0 ? 32 : -49))
                    return rollback(0, 0, 102);
                if (count == -49)
                    for (int i = 0; GUARD(32), i < 32; ++i)
                        if (bytes[i] != 0xA5)
                            return rollback(0, 0, 103);
                count = entropy_cr_random((uint32_t)bytes, 32, 3, 2);
                if (count != (accountGuarded == 0 ? 32 : -49))
                    return rollback(0, 0, 107);
                if (count == -49)
                    for (int i = 0; GUARD(32), i < 32; ++i)
                        if (bytes[i] != 0xA5)
                            return rollback(0, 0, 108);
                if (entropy_cr_random((uint32_t)bytes, 32, 3, 1) != 32)
                    return rollback(0, 0, 104);
                // Both bits mean ANY, not a narrower permission.
                if (entropy_cr_dice(1, 3, 3) != 0 ||
                    entropy_cr_random((uint32_t)bytes, 32, 3, 3) != 32)
                    return rollback(0, 0, 109);
                for (uint32_t flags = 4; GUARD(4), flags < 8; ++flags)
                    if (entropy_cr_dice(1, 3, flags) != -7 ||
                        entropy_cr_random((uint32_t)bytes, 32, 3, flags) != -7)
                        return rollback(0, 0, 110);
                if (entropy_cr_status() != status)
                    return rollback(0, 0, 105);
                return accept(0, 0, guarded);
            }
        )[test.hook]"];

        // Deliberately retain the previous import arities in these fixtures.
        TestHook oldDice = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                return accept(0, 0, entropy_cr_dice(6, 3));
            }
        )[test.hook]"];
        TestHook oldRandom = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_random(uint32_t, uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t bytes[32];
                return accept(0, 0, entropy_cr_random((uint32_t)bytes, 32, 3));
            }
        )[test.hook]"];

        auto const execute = [&](TestHook wasm,
                                 bool strong,
                                 bool terminal,
                                 bool terminalAccount,
                                 bool oldArity) {
            OpenView view{*env.current()};
            STTx tx{ttINVOKE, [&](STObject& obj) {
                        obj.setAccountID(sfAccount, alice.id());
                    }};
            ApplyContext ctx{
                env.app(),
                view,
                tx,
                tesSUCCESS,
                env.current()->fees().base,
                tapNONE,
                env.journal};
            hook::HookStateMap stateMap;
            std::map<std::vector<uint8_t>, std::vector<uint8_t>> params;
            auto const result = hook::apply(
                uint256{},
                sha512Half(Slice{wasm.data(), wasm.size()}),
                uint256{},
                uint256{},
                wasm,
                params,
                {},
                stateMap,
                ctx,
                alice.id(),
                false,
                false,
                strong,
                0,
                0,
                {},
                terminal,
                terminalAccount);
            if (oldArity)
            {
                // Bypass SetHook validation to model execution of Wasm
                // already installed under the old host ABI.
                BEAST_EXPECT(result.exitType == hook_api::ExitType::WASM_ERROR);
                BEAST_EXPECT(result.rngCallCounter == 0);
                BEAST_EXPECT(!result.hasGuardedEntropyDraw);
                BEAST_EXPECT(!result.hasAccountGuardedEntropyDraw);
            }
            else
            {
                bool const admitted = !strong || terminal;
                bool const accountAdmitted =
                    !strong || terminal || terminalAccount;
                BEAST_EXPECT(result.exitType == hook_api::ExitType::ACCEPT);
                BEAST_EXPECT(result.exitCode == (admitted ? 0 : -49));
                BEAST_EXPECT(
                    result.rngCallCounter ==
                    4 + (admitted ? 2 : 0) + (accountAdmitted ? 2 : 0));
                // The final permissive call must not clear guarded admission.
                BEAST_EXPECT(
                    result.hasGuardedEntropyDraw == (strong && terminal));
                BEAST_EXPECT(
                    result.hasAccountGuardedEntropyDraw ==
                    (strong && (terminal || terminalAccount)));
            }
        };
        execute(mixed, true, false, false, false);
        execute(mixed, true, false, true, false);
        execute(mixed, true, true, true, false);
        execute(mixed, false, false, false, false);
        for (auto const* old : {&oldDice, &oldRandom})
        {
            env(ripple::test::jtx::hook(alice, {{hso(*old, overrideFlag)}}, 0),
                HSFEE,
                ter(temMALFORMED));
            execute(*old, true, true, true, true);
        }
    }

    void
    testViewEntropyAdmission()
    {
        testcase(
            "Open previews use parent entropy; closed execution needs current "
            "input");
        using namespace jtx;
        Env env{*this, supported_amendments() | featureConsensusEntropy};
        Account const alice{"view-entropy"};
        env.fund(XRP(10000), alice);
        env.close();
        auto const parent = env.app().getLedgerMaster().getClosedLedger();
        auto next = std::make_shared<Ledger>(*parent, parent->info().closeTime);
        OpenView closed{next.get()};
        OpenView preview{open_ledger, parent->rules(), parent};
        OpenView staleClosed{closed};
        staleClosed.rawSetConsensusEntropy(parent->consensusEntropy());

        TestHook wasm = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_status(void);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t, uint32_t);
            extern int64_t entropy_cr_random(uint32_t, uint32_t, uint32_t, uint32_t);
            #define GUARD(n) _g((1U << 31U) + __LINE__, (n)+1)
            int64_t hook(uint32_t mode)
            {
                _g(1,1);
                int64_t status = entropy_cr_status();
                if ((mode == 1 && status != -5) || (mode != 1 && status < 0))
                    return rollback(0, 0, 100);
                for (uint32_t flags = 0; GUARD(4), flags < 4; ++flags)
                {
                    int64_t draw = entropy_cr_dice(1, 1, flags);
                    if (draw != (mode == 0 ? 0 : -48))
                        return rollback(0, 0, 101);
                    uint8_t bytes[32];
                    for (int i = 0; GUARD(132), i < 32; ++i)
                        bytes[i] = 0xA5;
                    int64_t count = entropy_cr_random((uint32_t)bytes, 32, 1, flags);
                    if (count != (mode == 0 ? 32 : -48))
                        return rollback(0, 0, 102);
                    if (mode != 0)
                        for (int i = 0; GUARD(132), i < 32; ++i)
                            if (bytes[i] != 0xA5)
                                return rollback(0, 0, 103);
                }
                return accept(0, 0, 0);
            }
        )[test.hook]"];
        auto const run = [&](OpenView& view, uint32_t mode) {
            STTx tx{ttINVOKE, [&](STObject& obj) {
                        obj.setAccountID(sfAccount, alice.id());
                    }};
            ApplyContext ctx{
                env.app(),
                view,
                tx,
                tesSUCCESS,
                view.fees().base,
                tapNONE,
                env.journal};
            hook::HookStateMap state;
            std::map<std::vector<uint8_t>, std::vector<uint8_t>> params;
            auto const result = hook::apply(
                uint256{},
                sha512Half(Slice{wasm.data(), wasm.size()}),
                uint256{},
                uint256{},
                wasm,
                params,
                {},
                state,
                ctx,
                alice.id(),
                false,
                false,
                true,
                mode,
                0,
                {},
                true);
            BEAST_EXPECT(result.exitType == hook_api::ExitType::ACCEPT);
            BEAST_EXPECT(result.exitCode == 0);
            BEAST_EXPECT(result.rngCallCounter == (mode == 0 ? 8 : 0));
            BEAST_EXPECT(result.hasGuardedEntropyDraw == (mode == 0));
            BEAST_EXPECT(result.hasAccountGuardedEntropyDraw == (mode == 0));
        };
        run(preview, 0);
        run(closed, 1);
        run(staleClosed, 2);
    }

    void
    testAccountOwnerHookOrdering()
    {
        testcase("Owner-installed Hook order and veto policy are explicit");
        using namespace jtx;
        TestHook draw = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t hook_param(uint32_t, uint32_t, uint32_t, uint32_t);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t, uint32_t);
            extern int64_t state_set(uint32_t, uint32_t, uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                // Policy is installed by the account owner, not read from
                // untrusted transaction parameters.
                uint8_t policy = 0;
                if (hook_param((uint32_t)&policy, 1, (uint32_t)"P", 1) != 1)
                    return rollback(0, 0, 1000);
                int64_t result = entropy_cr_dice(6, 3, policy);
                if (result < 0)
                    return accept((uint32_t)"rng", 3, result);
                uint8_t value = (uint8_t)result;
                uint8_t key[32] = {'o','w','n','e','r','-','o','r','d','e','r'};
                if (state_set((uint32_t)&value, 1, (uint32_t)key, 32) != 1)
                    return rollback(0, 0, 1001);
                return accept((uint32_t)"rng", 3, result);
            }
        )[test.hook]"];
        TestHook accounting = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t hook_param(uint32_t, uint32_t, uint32_t, uint32_t);
            extern int64_t state(uint32_t, uint32_t, uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                uint8_t value = 0;
                uint8_t key[32] = {'o','w','n','e','r','-','o','r','d','e','r'};
                int64_t count = state((uint32_t)&value, 1, (uint32_t)key, 32);
                if (count == -5)
                    return accept((uint32_t)"tail", 4, 77);
                if (count != 1)
                    return rollback(0, 0, 1002);
                uint8_t veto = 0;
                if (hook_param((uint32_t)&veto, 1, (uint32_t)"V", 1) != 1)
                    return rollback(0, 0, 1003);
                if (veto)
                    return rollback((uint32_t)"tail", 4, 200 + value);
                return accept((uint32_t)"tail", 4, 100 + value);
            }
        )[test.hook]"];
        TestHook strictLibrary = consensusentropy_test_wasm[R"[test.hook](
            #include <stdint.h>
            extern int32_t _g(uint32_t, uint32_t);
            extern int64_t accept(uint32_t, uint32_t, int64_t);
            extern int64_t rollback(uint32_t, uint32_t, int64_t);
            extern int64_t entropy_cr_dice(uint32_t, uint32_t, uint32_t);
            extern int64_t state_set(uint32_t, uint32_t, uint32_t, uint32_t);
            int64_t hook(uint32_t r)
            {
                _g(1,1);
                // A reusable module fixes its own policy even when installed
                // on an account controlled by a different party.
                int64_t result = entropy_cr_dice(6, 3, 0);
                if (result < 0)
                    return accept((uint32_t)"rng", 3, result);
                uint8_t value = (uint8_t)result;
                uint8_t key[32] = {'o','w','n','e','r','-','o','r','d','e','r'};
                if (state_set((uint32_t)&value, 1, (uint32_t)key, 32) != 1)
                    return rollback(0, 0, 1001);
                return accept((uint32_t)"rng", 3, result);
            }
        )[test.hook]"];
        struct Case
        {
            bool drawFirst;
            uint8_t policy;
            bool veto;
            bool fixedCallerPolicy = false;
        };
        constexpr uint8_t any = hook_api::ENTROPY_ALLOW_ANY_STRONG_VETO;
        constexpr uint8_t same =
            hook_api::ENTROPY_ALLOW_SAME_ACCOUNT_STRONG_VETO;
        // Exercise both origin chains and destination chains: SAME_ACCOUNT
        // refers to the installed Hook's account, never the transaction sender.
        for (bool const incoming : {false, true})
            for (auto const test :
                 {Case{false, 0, false},
                  Case{false, same, false},
                  Case{true, 0, false},
                  Case{true, same, false},
                  Case{true, same, true},
                  Case{true, any, false},
                  Case{true, any, true},
                  Case{true, any | same, false},
                  Case{true, any | same, true},
                  Case{true, any | same, true, true}})
            {
                Env env{
                    *this, supported_amendments() | featureConsensusEntropy};
                Account const owner{
                    test.fixedCallerPolicy ? "player-library"
                                           : "configured-owner"};
                Account const sender{"ordering-sender"};
                env.fund(XRP(10000), owner, sender);
                env.close();
                auto configured =
                    [&](TestHook wasm, std::string const& name, uint8_t value) {
                        auto obj = hso(wasm, overrideFlag);
                        auto& param =
                            obj[jss::HookParameters][0u][jss::HookParameter];
                        param[jss::HookParameterName] = name;
                        param[jss::HookParameterValue] = strHex(Blob{value});
                        return obj;
                    };
                auto rng = configured(
                    test.fixedCallerPolicy ? strictLibrary : draw,
                    "50",
                    test.policy);
                auto tail = configured(accounting, "56", test.veto);
                env(ripple::test::jtx::hook(
                        owner,
                        test.drawFirst ? std::vector<Json::Value>{rng, tail}
                                       : std::vector<Json::Value>{tail, rng},
                        0),
                    HSFEE);
                env.close();
                Json::Value invoke;
                invoke[jss::TransactionType] = "Invoke";
                invoke[jss::Account] =
                    incoming ? sender.human() : owner.human();
                if (incoming)
                    invoke[jss::Destination] = owner.human();
                bool const refused = test.drawFirst &&
                    (test.fixedCallerPolicy || test.policy == 0);
                env(invoke,
                    fee(XRP(1)),
                    (test.veto && !refused) ? ter(tecHOOK_REJECTED)
                                            : ter(tesSUCCESS));
                BEAST_REQUIRE(env.meta());
                auto const executions =
                    env.meta()->getFieldArray(sfHookExecutions);
                BEAST_REQUIRE(executions.size() == 2);
                auto const& rngResult = executions[test.drawFirst ? 0 : 1];
                auto const& tailResult = executions[test.drawFirst ? 1 : 0];
                BEAST_EXPECT(hookReturnString(rngResult) == "rng");
                BEAST_EXPECT(hookReturnString(tailResult) == "tail");
                auto const roll = hookReturnCode(rngResult);
                if (refused)
                    BEAST_EXPECT(roll == -49);
                else
                    BEAST_EXPECT(roll >= 0 && roll < 6);
                BEAST_EXPECT(
                    hookReturnCode(tailResult) ==
                    ((!test.drawFirst || refused)
                         ? 77
                         : (test.veto ? 200 : 100) + roll));
                std::array<uint8_t, 32> key{};
                std::string const prefix = "owner-order";
                std::copy(prefix.begin(), prefix.end(), key.begin());
                auto const result = env.le(keylet::hookState(
                    owner.id(), uint256::fromVoid(key.data()), beast::zero));
                BEAST_EXPECT(!!result == (!refused && !test.veto));
                if (result)
                    BEAST_EXPECT(
                        result->getFieldVL(sfHookStateData) ==
                        Blob{static_cast<uint8_t>(roll)});
            }
    }

    void
    run() override
    {
        testContextCreated();
        testContextUpdatedOnSubsequentClose();
        testNoContextWithoutAmendment();
        testDice();
        testDiceZeroSides();
        testEntropyStatus();
        testEntropyStatusFallback();
        testStaleEntropyStatus();
        testDiceTierRequirementNotMet();
        testDiceWithoutAmendment();
        testRetiredImportNamesRejected();
        testRandomTierRequirementNotMet();
        testInvalidEntropyRequirements();
        testEntropyDrawPoliciesAcrossStakeholders();
        testGuardedDrawSameChainRefusal();
        testEntropyCompositionFilters();
        testEntropyHostAdmission();
        testViewEntropyAdmission();
        testAccountOwnerHookOrdering();
        testEntropyDrawDoesNotBlockWeakAgainAsWeak();
        testRandom();
        testDiceConsecutiveCallsDiffer();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusEntropy, app, ripple);

}  // namespace test
}  // namespace ripple
