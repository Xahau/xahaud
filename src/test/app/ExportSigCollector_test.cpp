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
#include <xrpl/protocol/SecretKey.h>
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
    testSnapshotsAndFilteredCounts()
    {
        testcase("snapshots and filtered counts use verified signatures only");

        auto const other = randomKeyPair(KeyType::secp256k1).first;
        ExportSigCollector collector;
        auto const tx = makeHash("snapshot-filtered");
        auto const verifiedSig = makeSignature(20);
        auto const unverifiedSig = makeSignature(30);

        BEAST_EXPECT(!collector.hasVerifiedSignature(tx, validator_));
        BEAST_EXPECT(collector.unverifiedSignatures(tx).empty());
        BEAST_EXPECT(!collector.checkQuorumAndSnapshot(tx, 1));

        collector.addVerifiedSignature(tx, validator_, verifiedSig, 10);
        collector.addUnverifiedSignature(tx, other, unverifiedSig, 11);

        BEAST_EXPECT(collector.hasVerifiedSignature(tx, validator_));
        BEAST_EXPECT(!collector.hasVerifiedSignature(tx, other));
        BEAST_EXPECT(collector.signatureCount(tx) == 1);
        BEAST_EXPECT(collector.signatureCount(tx, [&](PublicKey const& pk) {
            return pk == validator_;
        }) == 1);
        BEAST_EXPECT(collector.signatureCount(tx, [&](PublicKey const& pk) {
            return pk == other;
        }) == 0);

        auto unverified = collector.unverifiedSignatures(tx);
        BEAST_EXPECT(unverified.size() == 1);
        BEAST_EXPECT(unverified.count(other) == 1);

        auto snapshot = collector.snapshot();
        BEAST_EXPECT(snapshot.size() == 1);
        BEAST_EXPECT(snapshot[tx].count(validator_) == 1);
        BEAST_EXPECT(snapshot[tx].count(other) == 0);

        auto sigSnapshot = collector.snapshotWithSigs();
        BEAST_EXPECT(sigSnapshot[tx].size() == 1);
        BEAST_EXPECT(sigSnapshot[tx][validator_] == verifiedSig);

        auto filteredSnapshot = collector.snapshotWithSigs(
            [&](PublicKey const& pk) { return pk == other; });
        BEAST_EXPECT(filteredSnapshot.empty());

        BEAST_EXPECT(!collector.checkQuorumAndSnapshot(tx, 2));
        auto quorum = collector.checkQuorumAndSnapshot(tx, 1);
        BEAST_EXPECT(quorum.has_value());
        if (quorum)
        {
            BEAST_EXPECT(quorum->size() == 1);
            BEAST_EXPECT((*quorum)[validator_] == verifiedSig);
        }

        collector.upgradeSignature(tx, other, makeSignature(31), 12);
        BEAST_EXPECT(collector.signatureCount(tx) == 1);

        collector.upgradeSignature(tx, other, unverifiedSig, 12);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(tx) == 2);

        auto filteredQuorum = collector.checkQuorumAndSnapshot(
            tx, 1, [&](PublicKey const& pk) { return pk == other; });
        BEAST_EXPECT(filteredQuorum.has_value());
        if (filteredQuorum)
            BEAST_EXPECT((*filteredQuorum)[other] == unverifiedSig);

        collector.clear(tx);
        BEAST_EXPECT(collector.signatureCount(tx) == 0);
        BEAST_EXPECT(collector.snapshot().empty());
    }

    void
    testStandaloneAndRoundState()
    {
        testcase("standalone signatures and round state");

        ExportSigCollector collector;
        auto const tx = makeHash("standalone-round");

        collector.addStandaloneSignature(tx, validator_, 10);
        BEAST_EXPECT(collector.hasVerifiedSignature(tx, validator_));
        BEAST_EXPECT(collector.signatureCount(tx) == 1);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());

        auto snapshot = collector.snapshot();
        BEAST_EXPECT(snapshot.size() == 1);
        BEAST_EXPECT(snapshot[tx].count(validator_) == 1);

        auto sigSnapshot = collector.snapshotWithSigs();
        BEAST_EXPECT(sigSnapshot.size() == 1);
        BEAST_EXPECT(sigSnapshot[tx].count(validator_) == 1);
        BEAST_EXPECT(sigSnapshot[tx][validator_].empty());

        BEAST_EXPECT(collector.markSent(tx));
        BEAST_EXPECT(!collector.markSent(tx));
        collector.clearRound();
        BEAST_EXPECT(collector.markSent(tx));

        auto const tx2 = makeHash("standalone-round-2");
        auto const tx3 = makeHash("standalone-round-3");
        BEAST_EXPECT(collector.markSent(tx2, 2));
        BEAST_EXPECT(!collector.markSent(tx3, 2));
        BEAST_EXPECT(!collector.markSent(tx2, 2));
        collector.clearRound();
        BEAST_EXPECT(collector.markSent(tx3, 2));
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
    testDefensiveNoOps()
    {
        testcase("defensive no-op paths");

        ExportSigCollector collector;
        auto const missingTx = makeHash("missing-defensive");
        auto const standaloneTx = makeHash("standalone-defensive");
        auto const sig = makeSignature(40);

        collector.upgradeSignature(missingTx, validator_, sig, 10);
        BEAST_EXPECT(collector.signatureCount(missingTx) == 0);
        BEAST_EXPECT(!collector.removeSignature(missingTx, validator_, sig));
        BEAST_EXPECT(!collector.checkQuorumAndSnapshot(missingTx, 1));
        BEAST_EXPECT(collector.signatureCount(missingTx, [](PublicKey const&) {
            return true;
        }) == 0);

        collector.addStandaloneSignature(standaloneTx, validator_, 10);
        collector.upgradeSignature(standaloneTx, validator_, Buffer{}, 11);
        BEAST_EXPECT(collector.signatureCount(standaloneTx) == 1);
        BEAST_EXPECT(collector.snapshotWithSigs()
                         .at(standaloneTx)
                         .at(validator_)
                         .empty());

        auto filtered =
            collector.snapshotWithSigs([](PublicKey const&) { return false; });
        BEAST_EXPECT(filtered.empty());
        BEAST_EXPECT(!collector.checkQuorumAndSnapshot(
            standaloneTx, 1, [](PublicKey const&) { return false; }));
    }

    void
    run() override
    {
        testCleanupUsesFirstSeenSeq();
        testUpgradeSetsFirstSeenSeq();
        testRemoveInvalidUnverifiedSignature();
        testSnapshotsAndFilteredCounts();
        testStandaloneAndRoundState();
        testClearAll();
        testDefensiveNoOps();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSigCollector, app, ripple);

}  // namespace test
}  // namespace ripple
