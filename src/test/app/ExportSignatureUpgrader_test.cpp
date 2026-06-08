//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose  with  or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE  SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH  REGARD  TO  THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpld/app/tx/detail/ExportResultBuilder.h>
#include <xrpld/app/tx/detail/ExportSignatureUpgrader.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <cstdint>
#include <cstring>
#include <set>

namespace ripple {
namespace test {
namespace {

uint256
makeHash(char const* label)
{
    return sha512Half(Slice(label, std::strlen(label)));
}

STTx
makeSTTx(STObject const& obj)
{
    Serializer s;
    obj.add(s);
    SerialIter sit{s.slice()};
    return STTx{std::ref(sit)};
}

STTx
makeExportedPayment(AccountID const& src, AccountID const& dst)
{
    STObject obj(sfExportedTxn);
    obj.setFieldU16(sfTransactionType, ttPAYMENT);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, 1);
    obj.setFieldU32(sfFirstLedgerSequence, 2);
    obj.setFieldU32(sfLastLedgerSequence, 6);
    obj.setFieldAmount(sfAmount, XRPAmount{1000000});
    obj.setFieldAmount(sfFee, XRPAmount{10});
    obj.setFieldVL(sfSigningPubKey, Blob{});
    obj.setAccountID(sfAccount, src);
    obj.setAccountID(sfDestination, dst);
    return makeSTTx(obj);
}

Buffer
makeInvalidSignature(std::uint8_t first = 1)
{
    std::uint8_t bytes[] = {
        first,
        static_cast<std::uint8_t>(first + 1),
        static_cast<std::uint8_t>(first + 2),
        static_cast<std::uint8_t>(first + 3),
        static_cast<std::uint8_t>(first + 4)};
    return Buffer(bytes, sizeof(bytes));
}

beast::Journal
nullJournal()
{
    return beast::Journal{beast::Journal::getNullSink()};
}

}  // namespace

class ExportSignatureUpgrader_test : public beast::unit_test::suite
{
public:
    void
    testUpgradeFiltersAndRemovesInvalid()
    {
        testcase("upgrade filters and removes invalid signatures");

        auto const validSigner = randomKeyPair(KeyType::secp256k1);
        auto const invalidSigner = randomKeyPair(KeyType::secp256k1);
        auto const inactiveSigner = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(validSigner.first), calcAccountID(dst.first));
        auto const txHash = makeHash("export-upgrade");

        auto validSig = ExportResultBuilder::signExportedTxn(
            innerTx, validSigner.first, validSigner.second);
        auto invalidSig = makeInvalidSignature();
        auto inactiveSig = ExportResultBuilder::signExportedTxn(
            innerTx, inactiveSigner.first, inactiveSigner.second);

        ExportSigCollector collector;
        collector.addUnverifiedSignature(
            txHash, validSigner.first, validSig, 7);
        collector.addUnverifiedSignature(
            txHash, invalidSigner.first, invalidSig, 7);
        collector.addUnverifiedSignature(
            txHash, inactiveSigner.first, inactiveSig, 7);

        std::set<PublicKey> active{
            validSigner.first,
            invalidSigner.first,
        };
        auto stats = ExportSignatureUpgrader::upgradeUnverifiedSignatures(
            collector,
            innerTx,
            txHash,
            12,
            [&active](PublicKey const& pk) { return active.count(pk) > 0; },
            nullJournal());

        BEAST_EXPECT(stats.inspected == 3);
        BEAST_EXPECT(stats.inactiveSkipped == 1);
        BEAST_EXPECT(stats.upgraded == 1);
        BEAST_EXPECT(stats.removedInvalid == 1);

        BEAST_EXPECT(collector.hasVerifiedSignature(txHash, validSigner.first));
        BEAST_EXPECT(
            !collector.hasVerifiedSignature(txHash, invalidSigner.first));
        BEAST_EXPECT(
            !collector.hasVerifiedSignature(txHash, inactiveSigner.first));

        auto const unverified = collector.unverifiedSignatures(txHash);
        BEAST_EXPECT(!unverified.contains(invalidSigner.first));
        BEAST_EXPECT(unverified.contains(inactiveSigner.first));
        BEAST_EXPECT(collector.signatureCount(txHash) == 1);
    }

    void
    testInvalidRemovalRequiresStoredBufferMatch()
    {
        testcase("invalid removal requires stored buffer match");

        auto const invalidSigner = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(invalidSigner.first), calcAccountID(dst.first));
        auto const txHash = makeHash("export-upgrade-race");

        auto invalidSig = makeInvalidSignature();
        auto replacementSig = makeInvalidSignature(20);

        ExportSigCollector collector;
        collector.addUnverifiedSignature(
            txHash, invalidSigner.first, invalidSig, 7);

        bool mutated = false;
        auto stats = ExportSignatureUpgrader::upgradeUnverifiedSignatures(
            collector,
            innerTx,
            txHash,
            12,
            [&](PublicKey const& pk) {
                if (pk == invalidSigner.first && !mutated)
                {
                    mutated = true;
                    collector.addUnverifiedSignature(
                        txHash, invalidSigner.first, replacementSig, 12);
                }
                return true;
            },
            nullJournal());

        BEAST_EXPECT(mutated);
        BEAST_EXPECT(stats.inspected == 1);
        BEAST_EXPECT(stats.upgraded == 0);
        BEAST_EXPECT(stats.removedInvalid == 0);
        BEAST_EXPECT(
            !collector.hasVerifiedSignature(txHash, invalidSigner.first));

        auto const unverified = collector.unverifiedSignatures(txHash);
        auto const it = unverified.find(invalidSigner.first);
        BEAST_EXPECT(it != unverified.end());
        if (it != unverified.end())
            BEAST_EXPECT(it->second == replacementSig);
    }

    void
    run() override
    {
        testUpgradeFiltersAndRemovesInvalid();
        testInvalidRemovalRequiresStoredBufferMatch();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSignatureUpgrader, app, ripple);

}  // namespace test
}  // namespace ripple
