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

#include <xrpld/app/consensus/ExportSignatureHarvester.h>
#include <xrpl/basics/StringUtilities.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <cstring>
#include <memory>

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

STObject
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
    return obj;
}

std::shared_ptr<STTx const>
makeExportTx(STObject const& inner, AccountID const& account)
{
    STObject exportObj(sfGeneric);
    exportObj.setFieldU16(sfTransactionType, ttEXPORT);
    exportObj.setAccountID(sfAccount, account);
    exportObj.setFieldU32(sfSequence, 0);
    exportObj.setFieldVL(sfSigningPubKey, Blob{});
    exportObj.setFieldU32(sfFirstLedgerSequence, 2);
    exportObj.setFieldU32(sfLastLedgerSequence, 6);
    exportObj.setFieldAmount(sfFee, XRPAmount{0});
    exportObj.set(std::make_unique<STObject>(inner));

    return std::make_shared<STTx const>(makeSTTx(exportObj));
}

std::string
makeBlob(uint256 const& txHash, PublicKey const& pk, Slice sig)
{
    std::string blob;
    blob.append(reinterpret_cast<char const*>(txHash.data()), txHash.size());
    blob.append(reinterpret_cast<char const*>(pk.data()), pk.size());
    blob.append(reinterpret_cast<char const*>(sig.data()), sig.size());
    return blob;
}

std::string
makeBlob(uint256 const& txHash, PublicKey const& pk, Buffer const& sig)
{
    return makeBlob(txHash, pk, Slice(sig.data(), sig.size()));
}

beast::Journal
journal()
{
    return beast::Journal{beast::Journal::getNullSink()};
}

}  // namespace

class ExportSignatureHarvester_test : public beast::unit_test::suite
{
    std::pair<PublicKey, SecretKey> const sender_ =
        randomKeyPair(KeyType::secp256k1);
    std::pair<PublicKey, SecretKey> const other_ =
        randomKeyPair(KeyType::secp256k1);
    uint256 const prevLedger_ = makeHash("export-harvester-prev-ledger");
    char const* source_ = "unit-test";

    ExportSignatureHarvestInput
    makeInput(
        std::vector<std::string> const& blobs,
        ExportTxnLookup const& exportTxns,
        bool active = true,
        std::optional<uint256> sourceLedgerHash = std::nullopt,
        PublicKey const* sender = nullptr) const
    {
        return ExportSignatureHarvestInput{
            sender ? *sender : sender_.first,
            prevLedger_,
            blobs,
            sourceLedgerHash,
            [active](PublicKey const&) { return active; },
            exportTxns,
            42,
            source_,
            2};
    }

public:
    void
    testRejectsTooManyEntries()
    {
        testcase("rejects too many entries");

        auto const txHash = makeHash("too-many");
        auto const blob = makeBlob(txHash, sender_.first, Slice("sig", 3));
        std::vector<std::string> const blobs{blob, blob, blob};
        ExportTxnLookup lookup;
        ExportSigCollector collector;

        auto input = makeInput(blobs, lookup);
        BEAST_EXPECT(harvestExportSignatures(input, collector, journal()) == 0);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(txHash) == 0);
    }

    void
    testRejectsInactiveOrWrongParent()
    {
        testcase("rejects inactive or wrong-parent senders");

        auto const txHash = makeHash("inactive");
        std::vector<std::string> const blobs{
            makeBlob(txHash, sender_.first, Slice("sig", 3))};
        ExportTxnLookup lookup;
        ExportSigCollector collector;

        auto inactive = makeInput(blobs, lookup, false);
        BEAST_EXPECT(
            harvestExportSignatures(inactive, collector, journal()) == 0);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());

        auto wrongParent =
            makeInput(blobs, lookup, true, makeHash("different-parent"));
        BEAST_EXPECT(
            harvestExportSignatures(wrongParent, collector, journal()) == 0);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
    }

    void
    testRejectsPubkeyMismatchAtomically()
    {
        testcase("rejects embedded pubkey mismatch atomically");

        auto const txHash = makeHash("mismatch");
        std::vector<std::string> const blobs{
            makeBlob(txHash, sender_.first, Slice("sig-a", 5)),
            makeBlob(txHash, other_.first, Slice("sig-b", 5))};
        ExportTxnLookup lookup;
        ExportSigCollector collector;

        auto input = makeInput(blobs, lookup, true, prevLedger_);
        BEAST_EXPECT(harvestExportSignatures(input, collector, journal()) == 0);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(txHash) == 0);
    }

    void
    testMissingTxStoresUnverified()
    {
        testcase("missing tx stores unverified");

        auto const txHash = makeHash("missing-tx");
        std::vector<std::string> const blobs{
            makeBlob(txHash, sender_.first, Slice("sig", 3))};
        ExportTxnLookup lookup;
        ExportSigCollector collector;

        auto input = makeInput(blobs, lookup, true, prevLedger_);
        BEAST_EXPECT(harvestExportSignatures(input, collector, journal()) == 1);
        BEAST_EXPECT(collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(txHash) == 0);
    }

    void
    testOpenLedgerTxStoresVerifiedAndSkipsDuplicate()
    {
        testcase("open-ledger tx stores verified and skips duplicate");

        auto const senderAccount = calcAccountID(sender_.first);
        auto const dstAccount = calcAccountID(other_.first);
        auto const innerObj = makeExportedPayment(senderAccount, dstAccount);
        auto const innerTx = makeSTTx(innerObj);
        auto const sigData = buildMultiSigningData(innerTx, senderAccount);
        auto const sig = sign(sender_.first, sender_.second, sigData.slice());
        auto const exportTx = makeExportTx(innerObj, senderAccount);
        auto const txHash = exportTx->getTransactionID();

        ExportTxnLookup lookup;
        lookup.emplace(txHash, exportTx);
        std::vector<std::string> const blobs{
            makeBlob(txHash, sender_.first, sig)};
        ExportSigCollector collector;

        auto input = makeInput(blobs, lookup, true, prevLedger_);
        BEAST_EXPECT(harvestExportSignatures(input, collector, journal()) == 1);
        BEAST_EXPECT(!collector.hasUnverifiedSignatures());
        BEAST_EXPECT(collector.signatureCount(txHash) == 1);

        BEAST_EXPECT(harvestExportSignatures(input, collector, journal()) == 0);
        BEAST_EXPECT(collector.signatureCount(txHash) == 1);
    }

    void
    run() override
    {
        testRejectsTooManyEntries();
        testRejectsInactiveOrWrongParent();
        testRejectsPubkeyMismatchAtomically();
        testMissingTxStoresUnverified();
        testOpenLedgerTxStoresVerifiedAndSkipsDuplicate();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSignatureHarvester, app, ripple);

}  // namespace test
}  // namespace ripple
