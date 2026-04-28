//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/digest.h>
#include <cstring>

namespace ripple {
namespace test {

namespace {

uint256
makeHash(char const* label)
{
    return sha512Half(Slice(label, std::strlen(label)));
}

PublicKey
makePublicKey(char const* hex)
{
    auto const raw = strUnHex(hex);
    return PublicKey{makeSlice(*raw)};
}

Buffer
makeSignature(std::uint8_t seed)
{
    std::uint8_t bytes[] = {
        seed,
        static_cast<std::uint8_t>(seed + 1),
        static_cast<std::uint8_t>(seed + 2)};
    return Buffer(bytes, sizeof(bytes));
}

}  // namespace

class ExportSigCollector_test : public beast::unit_test::suite
{
    PublicKey const validator_ = makePublicKey(
        "0388935426E0D08083314842EDFBB2D517BD47699F9A4527318A8E10468C97C05"
        "2");

public:
    void
    testCleanupUsesFirstSeenSeq()
    {
        testcase("cleanup uses first seen sequence");

        ExportSigCollector collector;
        auto const tx = makeHash("cleanup-verified");
        auto const sig = makeSignature(1);

        collector.addVerifiedSignature(tx, validator_, sig, 10);
        BEAST_EXPECT(collector.signatureCount(tx) == 1);

        collector.cleanupStale(266);
        BEAST_EXPECT(collector.signatureCount(tx) == 1);

        collector.cleanupStale(267);
        BEAST_EXPECT(collector.signatureCount(tx) == 0);
    }

    void
    testUpgradeSetsFirstSeenSeq()
    {
        testcase("upgrade sets first seen sequence");

        ExportSigCollector collector;
        auto const tx = makeHash("cleanup-upgraded");
        auto const sig = makeSignature(5);

        collector.addUnverifiedSignature(tx, validator_, sig);
        BEAST_EXPECT(collector.hasUnverifiedSignatures());

        collector.upgradeSignature(tx, validator_, sig, 10);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(tx) == 1);

        collector.cleanupStale(266);
        BEAST_EXPECT(collector.signatureCount(tx) == 1);

        collector.cleanupStale(267);
        BEAST_EXPECT(collector.signatureCount(tx) == 0);
    }

    void
    testRemoveInvalidUnverifiedSignature()
    {
        testcase("remove invalid unverified signature");

        ExportSigCollector collector;
        auto const tx = makeHash("remove-invalid");
        auto const sig = makeSignature(9);
        auto const otherSig = makeSignature(10);

        collector.addUnverifiedSignature(tx, validator_, sig, 10);
        BEAST_EXPECT(collector.hasUnverifiedSignatures());

        BEAST_EXPECT(!collector.removeSignature(tx, validator_, otherSig));
        BEAST_EXPECT(collector.hasUnverifiedSignatures());

        BEAST_EXPECT(collector.removeSignature(tx, validator_, sig));
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(tx) == 0);
    }

    void
    testClearAll()
    {
        testcase("clear all signatures and round state");

        ExportSigCollector collector;
        auto const verifiedTx = makeHash("clear-all-verified");
        auto const unverifiedTx = makeHash("clear-all-unverified");
        auto const sig = makeSignature(12);

        collector.addVerifiedSignature(verifiedTx, validator_, sig, 10);
        collector.addUnverifiedSignature(unverifiedTx, validator_, sig, 10);
        BEAST_EXPECT(collector.signatureCount(verifiedTx) == 1);
        BEAST_EXPECT(collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.markSent(verifiedTx));
        BEAST_EXPECT(!collector.markSent(verifiedTx));

        collector.clearAll();

        BEAST_EXPECT(collector.signatureCount(verifiedTx) == 0);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.markSent(verifiedTx));
    }

    void
    run() override
    {
        testCleanupUsesFirstSeenSeq();
        testUpgradeSetsFirstSeenSeq();
        testRemoveInvalidUnverifiedSignature();
        testClearAll();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSigCollector, app, ripple);

}  // namespace test
}  // namespace ripple
