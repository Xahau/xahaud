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

#include <ripple/beast/unit_test.h>
#include <ripple/protocol/Feature.h>
#include <ripple/protocol/Indexes.h>
#include <ripple/protocol/SField.h>
#include <test/jtx.h>

namespace ripple {
namespace test {

class ConsensusEntropy_test : public beast::unit_test::suite
{
    void
    testSLECreated()
    {
        testcase("SLE created on ledger close");
        using namespace jtx;

        // Enable featureConsensusEntropy (excluded from default test set)
        Env env{
            *this,
            envconfig(),
            supported_amendments() | featureConsensusEntropy,
            nullptr};

        // Before any ledger close, the SLE should not exist
        BEAST_EXPECT(!env.le(keylet::consensusEntropy()));

        // Close one ledger — standalone injects synthetic entropy
        env.close();

        auto const sle = env.le(keylet::consensusEntropy());
        BEAST_EXPECT(sle != nullptr);
        if (!sle)
            return;

        // sfDigest should be non-zero (standalone synthetic entropy)
        auto const digest = sle->getFieldH256(sfDigest);
        BEAST_EXPECT(digest != uint256{});

        // sfEntropyCount should be >= 5 (standalone sets 20)
        auto const count = sle->getFieldU16(sfEntropyCount);
        BEAST_EXPECT(count >= 5);

        // sfLedgerSequence should match current closed ledger
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
        BEAST_EXPECT(sle1 != nullptr);
        if (!sle1)
            return;

        auto const digest1 = sle1->getFieldH256(sfDigest);
        auto const seq1 = sle1->getFieldU32(sfLedgerSequence);

        // Close another ledger
        env.close();

        auto const sle2 = env.le(keylet::consensusEntropy());
        BEAST_EXPECT(sle2 != nullptr);
        if (!sle2)
            return;

        auto const digest2 = sle2->getFieldH256(sfDigest);
        auto const seq2 = sle2->getFieldU32(sfLedgerSequence);

        // Entropy should change each ledger (seq is part of the hash input)
        BEAST_EXPECT(digest2 != digest1);
        BEAST_EXPECT(seq2 == seq1 + 1);
    }

    void
    testNoSLEWithoutAmendment()
    {
        testcase("No SLE without amendment");
        using namespace jtx;

        // Default test amendments (featureConsensusEntropy excluded)
        Env env{*this};

        env.close();
        env.close();

        BEAST_EXPECT(!env.le(keylet::consensusEntropy()));
    }

    void
    run() override
    {
        testSLECreated();
        testSLEUpdatedOnSubsequentClose();
        testNoSLEWithoutAmendment();
    }
};

BEAST_DEFINE_TESTSUITE(ConsensusEntropy, app, ripple);

}  // namespace test
}  // namespace ripple
