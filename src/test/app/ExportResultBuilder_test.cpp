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
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
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

}  // namespace

class ExportResultBuilder_test : public beast::unit_test::suite
{
public:
    void
    testAssemblesSignedMetadata()
    {
        testcase("assembles signed metadata");

        auto const signerA = randomKeyPair(KeyType::secp256k1);
        auto const signerB = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(signerA.first), calcAccountID(signerB.first));
        auto const exportTxHash = makeHash("outer-export-tx");

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(
            signerA.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerA.first, signerA.second));
        signatures.emplace(
            signerB.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerB.first, signerB.second));

        auto assembled = ExportResultBuilder::assembleDirect(
            innerTx, signatures, 123, exportTxHash);

        BEAST_EXPECT(assembled.signerCount == 2);
        BEAST_EXPECT(assembled.metadata.getFieldU32(sfLedgerSequence) == 123);
        BEAST_EXPECT(
            assembled.metadata.getFieldH256(sfTransactionHash) == exportTxHash);

        auto const& multiSigned =
            assembled.metadata.peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(multiSigned.getFieldVL(sfSigningPubKey).empty());
        BEAST_EXPECT(multiSigned.isFieldPresent(sfSigners));

        auto const& signers = multiSigned.getFieldArray(sfSigners);
        BEAST_EXPECT(signers.size() == 2);
        if (signers.size() == 2)
        {
            BEAST_EXPECT(
                signers[0].getAccountID(sfAccount) <
                signers[1].getAccountID(sfAccount));
        }

        for (auto const& signer : signers)
        {
            auto const pkVL = signer.getFieldVL(sfSigningPubKey);
            PublicKey const pk{makeSlice(pkVL)};
            auto const sigVL = signer.getFieldVL(sfTxnSignature);
            auto const signerAcctID = signer.getAccountID(sfAccount);
            auto const sigData = buildMultiSigningData(innerTx, signerAcctID);
            BEAST_EXPECT(verify(pk, sigData.slice(), makeSlice(sigVL)));
        }

        BEAST_EXPECT(
            assembled.signedTxHash ==
            multiSigned.getHash(HashPrefix::transactionID));

        Serializer serialized;
        multiSigned.add(serialized);
        SerialIter sit(serialized.slice());
        STTx signedTx{std::ref(sit)};
        BEAST_EXPECT(signedTx.getTransactionID() == assembled.signedTxHash);
    }

    void
    testSkipsEmptySignatures()
    {
        testcase("skips empty signatures");

        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(signer.first), calcAccountID(dst.first));

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(signer.first, Buffer{});

        auto assembled = ExportResultBuilder::assembleDirect(
            innerTx, signatures, 456, makeHash("empty-sig-export"));

        BEAST_EXPECT(assembled.signerCount == 0);

        auto const& multiSigned =
            assembled.metadata.peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(multiSigned.getFieldVL(sfSigningPubKey).empty());
        BEAST_EXPECT(!multiSigned.isFieldPresent(sfSigners));
        BEAST_EXPECT(
            assembled.signedTxHash ==
            multiSigned.getHash(HashPrefix::transactionID));
    }

    void
    testBuildMultiSignedExportedTxnDirect()
    {
        testcase("builds multisigned exported transaction directly");

        auto const signerA = randomKeyPair(KeyType::secp256k1);
        auto const signerB = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(signerA.first), calcAccountID(dst.first));

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(signerB.first, Buffer{});
        signatures.emplace(
            signerA.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerA.first, signerA.second));

        auto multiSigned = ExportResultBuilder::buildMultiSignedExportedTxn(
            innerTx, signatures);
        BEAST_EXPECT(multiSigned.getFieldVL(sfSigningPubKey).empty());
        BEAST_EXPECT(multiSigned.isFieldPresent(sfSigners));

        auto const& signers = multiSigned.getFieldArray(sfSigners);
        BEAST_EXPECT(signers.size() == 1);
        if (signers.size() == 1)
        {
            BEAST_EXPECT(
                signers[0].getAccountID(sfAccount) ==
                calcAccountID(signerA.first));
            BEAST_EXPECT(
                makeSlice(signers[0].getFieldVL(sfSigningPubKey)) ==
                signerA.first.slice());
        }

        ExportResultBuilder::SignatureSnapshot none;
        auto unsignedMulti =
            ExportResultBuilder::buildMultiSignedExportedTxn(innerTx, none);
        BEAST_EXPECT(unsignedMulti.getFieldVL(sfSigningPubKey).empty());
        BEAST_EXPECT(!unsignedMulti.isFieldPresent(sfSigners));
    }

    void
    testCapsSignerArray()
    {
        testcase("caps exported signer array");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));

        ExportResultBuilder::SignatureSnapshot signatures;
        while (signatures.size() < STTx::maxMultiSigners() + 5)
        {
            auto const signer = randomKeyPair(KeyType::secp256k1);
            signatures.emplace(
                signer.first,
                ExportResultBuilder::signExportedTxn(
                    innerTx, signer.first, signer.second));
        }

        auto assembled = ExportResultBuilder::assembleDirect(
            innerTx, signatures, 789, makeHash("many-sig-export"));

        BEAST_EXPECT(assembled.signerCount == STTx::maxMultiSigners());

        auto const& multiSigned =
            assembled.metadata.peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(multiSigned.isFieldPresent(sfSigners));

        if (multiSigned.isFieldPresent(sfSigners))
        {
            auto const& signers = multiSigned.getFieldArray(sfSigners);
            BEAST_EXPECT(signers.size() == STTx::maxMultiSigners());
            for (std::size_t i = 1; i < signers.size(); ++i)
            {
                BEAST_EXPECT(
                    signers[i - 1].getAccountID(sfAccount) <
                    signers[i].getAccountID(sfAccount));
            }
        }

        BEAST_EXPECT(
            assembled.signedTxHash ==
            multiSigned.getHash(HashPrefix::transactionID));
    }

    void
    testAssemblesWitnessReferenceMetadata()
    {
        testcase("assembles witness-reference metadata");

        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(signer.first), calcAccountID(dst.first));
        auto const exportTxHash = makeHash("outer-export-reference");
        auto const witnessHash = makeHash("export-signature-witness");

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(
            signer.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signer.first, signer.second));

        auto assembled = ExportResultBuilder::assembleClosedLedger(
            innerTx, signatures, 321, exportTxHash, witnessHash);

        BEAST_EXPECT(assembled.signerCount == 1);
        BEAST_EXPECT(assembled.metadata.getFieldU32(sfLedgerSequence) == 321);
        BEAST_EXPECT(
            assembled.metadata.getFieldH256(sfTransactionHash) == exportTxHash);
        BEAST_EXPECT(
            assembled.metadata.getFieldH256(sfExportSignatureHash) ==
            witnessHash);
        BEAST_EXPECT(!assembled.metadata.isFieldPresent(sfExportedTxn));
    }

    void
    testSignatureWitnessRoundTrip()
    {
        testcase("signature witness round trip");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));
        auto const exportTxHash = makeHash("outer-export-witness");

        ExportResultBuilder::SignatureSnapshot signatures;
        while (signatures.size() < STTx::maxMultiSigners() + 5)
        {
            auto const signer = randomKeyPair(KeyType::secp256k1);
            signatures.emplace(
                signer.first,
                ExportResultBuilder::signExportedTxn(
                    innerTx, signer.first, signer.second));
        }

        auto witness = ExportResultBuilder::buildSignatureWitness(
            exportTxHash, signatures, 654);
        BEAST_EXPECT(witness.getTxnType() == ttEXPORT_SIGNATURES);
        BEAST_EXPECT(witness.getFieldU32(sfLedgerSequence) == 654);
        BEAST_EXPECT(witness.getFieldH256(sfTransactionHash) == exportTxHash);
        BEAST_EXPECT(
            witness.getFieldArray(sfSigners).size() == signatures.size());

        auto decoded = ExportResultBuilder::signaturesFromWitness(witness);
        BEAST_EXPECT(decoded);
        if (decoded)
        {
            BEAST_EXPECT(decoded->size() == signatures.size());
            for (auto const& [pk, sig] : signatures)
            {
                auto const it = decoded->find(pk);
                BEAST_EXPECT(it != decoded->end());
                if (it != decoded->end())
                    BEAST_EXPECT(it->second == sig);
            }
        }
    }

    void
    testRejectsNonCanonicalWitnessSigner()
    {
        testcase("signature witness validates signer account");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto const wrongAccount = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));
        auto const exportTxHash = makeHash("bad-witness-signer");

        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(
            signer.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signer.first, signer.second));

        auto witness = ExportResultBuilder::buildSignatureWitness(
            exportTxHash, signatures, 654);
        auto signers = witness.getFieldArray(sfSigners);
        signers[0].setAccountID(sfAccount, calcAccountID(wrongAccount.first));
        witness.setFieldArray(sfSigners, signers);

        BEAST_EXPECT(!ExportResultBuilder::signaturesFromWitness(witness));
    }

    void
    run() override
    {
        testAssemblesSignedMetadata();
        testSkipsEmptySignatures();
        testBuildMultiSignedExportedTxnDirect();
        testCapsSignerArray();
        testAssemblesWitnessReferenceMetadata();
        testSignatureWitnessRoundTrip();
        testRejectsNonCanonicalWitnessSigner();
    }
};

BEAST_DEFINE_TESTSUITE(ExportResultBuilder, app, ripple);

}  // namespace test
}  // namespace ripple
