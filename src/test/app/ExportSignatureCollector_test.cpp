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

#include <test/jtx.h>
#include <xrpld/app/misc/ExportSignatureCollector.h>
#include <xrpl/protocol/Sign.h>

namespace ripple {
namespace test {

class ExportSignatureCollector_test : public beast::unit_test::suite
{
    static STTx
    makeUnsignedTx()
    {
        auto const txKey = randomKeyPair(KeyType::secp256k1);
        auto const txAccount = calcAccountID(txKey.first);

        return STTx(ttACCOUNT_SET, [&txAccount, &txKey](auto& obj) {
            obj.setAccountID(sfAccount, txAccount);
            obj.setFieldVL(sfMessageKey, txKey.first.slice());
            obj.setFieldVL(sfSigningPubKey, Slice{});
        });
    }

    static STObject
    makeSigner(PublicKey const& pk, AccountID const& acc, Blob const& sig)
    {
        STObject signer(sfSigner);
        signer.setAccountID(sfAccount, acc);
        signer.setFieldVL(sfSigningPubKey, pk.slice());
        signer.setFieldVL(sfTxnSignature, sig);
        return signer;
    }

    void
    testDuplicateCanReplaceUnverified()
    {
        testcase("duplicate replaces unverified");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);

        Serializer sigData = buildMultiSigningData(tx, validatorAcc);
        auto const goodSigBuf =
            sign(validator.first, validator.second, sigData.slice());

        Blob goodSig(goodSigBuf.begin(), goodSigBuf.end());
        Blob badSig = goodSig;
        badSig.back() ^= 0x01;

        auto badSigner = makeSigner(validator.first, validatorAcc, badSig);
        auto goodSigner = makeSigner(validator.first, validatorAcc, goodSig);

        BEAST_EXPECT(collector.verifyAndAddSignature(
            txnHash, validator.first, badSigner, 100));
        BEAST_EXPECT(!collector.isSignatureVerified(txnHash, validator.first));

        collector.stashTxnData(txnHash, txData);

        BEAST_EXPECT(collector.verifyAndAddSignature(
            txnHash, validator.first, goodSigner, 101));
        BEAST_EXPECT(collector.isSignatureVerified(txnHash, validator.first));
        BEAST_EXPECT(collector.verifySignature(txnHash, validator.first));

        auto const stored =
            collector.getSignatureFrom(txnHash, validator.first);
        BEAST_EXPECT(stored);
        if (stored)
            BEAST_EXPECT(stored->getFieldVL(sfTxnSignature) == goodSig);
    }

    void
    testVerifiedIsNotReplaced()
    {
        testcase("verified is stable");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);
        collector.stashTxnData(txnHash, txData);

        Serializer sigData = buildMultiSigningData(tx, validatorAcc);
        auto const goodSigBuf =
            sign(validator.first, validator.second, sigData.slice());

        Blob goodSig(goodSigBuf.begin(), goodSigBuf.end());
        Blob badSig = goodSig;
        badSig.back() ^= 0x01;

        auto goodSigner = makeSigner(validator.first, validatorAcc, goodSig);
        auto badSigner = makeSigner(validator.first, validatorAcc, badSig);

        BEAST_EXPECT(collector.verifyAndAddSignature(
            txnHash, validator.first, goodSigner, 200));
        BEAST_EXPECT(collector.isSignatureVerified(txnHash, validator.first));

        BEAST_EXPECT(collector.verifyAndAddSignature(
            txnHash, validator.first, badSigner, 201));
        BEAST_EXPECT(collector.verifySignature(txnHash, validator.first));

        auto const stored =
            collector.getSignatureFrom(txnHash, validator.first);
        BEAST_EXPECT(stored);
        if (stored)
            BEAST_EXPECT(stored->getFieldVL(sfTxnSignature) == goodSig);
    }

    void
    testRejectsSignerIdentityMismatch()
    {
        testcase("reject signer identity mismatch");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validatorA = randomKeyPair(KeyType::secp256k1);
        auto const validatorB = randomKeyPair(KeyType::secp256k1);
        auto const validatorBAcc = calcAccountID(validatorB.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);
        collector.stashTxnData(txnHash, txData);

        Serializer sigData = buildMultiSigningData(tx, validatorBAcc);
        auto const sigBuf =
            sign(validatorB.first, validatorB.second, sigData.slice());
        Blob sig(sigBuf.begin(), sigBuf.end());

        auto mismatchedSigner =
            makeSigner(validatorB.first, validatorBAcc, sig);

        BEAST_EXPECT(!collector.verifyAndAddSignature(
            txnHash, validatorA.first, mismatchedSigner, 300));
        BEAST_EXPECT(!collector.hasSignatureFrom(txnHash, validatorA.first));
        BEAST_EXPECT(!collector.hasSignatureFrom(txnHash, validatorB.first));
    }

    void
    testStashPrunesInvalidUnverified()
    {
        testcase("stash prunes invalid unverified");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);

        Serializer sigData = buildMultiSigningData(tx, validatorAcc);
        auto const goodSigBuf =
            sign(validator.first, validator.second, sigData.slice());

        Blob badSig(goodSigBuf.begin(), goodSigBuf.end());
        badSig.back() ^= 0x01;

        auto badSigner = makeSigner(validator.first, validatorAcc, badSig);

        BEAST_EXPECT(collector.verifyAndAddSignature(
            txnHash, validator.first, badSigner, 400));
        BEAST_EXPECT(collector.signatureCount(txnHash) == 1);

        collector.stashTxnData(txnHash, txData);

        BEAST_EXPECT(collector.signatureCount(txnHash) == 0);
        BEAST_EXPECT(!collector.hasSignatureFrom(txnHash, validator.first));
        BEAST_EXPECT(!collector.isSignatureVerified(txnHash, validator.first));
    }

    void
    testInvalidEarlyDataDoesNotAgeOutValidLater()
    {
        testcase("invalid early data does not age out valid later");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);
        std::string const tag = "first-seen-ageing-bug";
        auto const txnHash = sha512Half(makeSlice(tag));

        STObject malformed(sfSigner);
        malformed.setFieldVL(sfSigningPubKey, validator.first.slice());
        malformed.setFieldVL(sfTxnSignature, Blob{0x01});
        BEAST_EXPECT(!collector.verifyAndAddSignature(
            txnHash, validator.first, malformed, 1));
        BEAST_EXPECT(collector.signatureCount(txnHash) == 0);

        STObject laterValid(sfSigner);
        laterValid.setAccountID(sfAccount, validatorAcc);
        laterValid.setFieldVL(sfSigningPubKey, validator.first.slice());
        laterValid.setFieldVL(sfTxnSignature, Blob{0x02});
        BEAST_EXPECT(collector.verifyAndAddSignature(
            txnHash, validator.first, laterValid, 200));
        BEAST_EXPECT(collector.signatureCount(txnHash) == 1);

        // If firstSeen came from the rejected signature (seq=1), this cleanup
        // wrongly evicts the valid signature added at seq=200.
        collector.cleanupStale(260, 256);
        BEAST_EXPECT(collector.signatureCount(txnHash) == 1);
        BEAST_EXPECT(collector.hasSignatureFrom(txnHash, validator.first));
    }

    void
    testAddSignatureUpdatesExisting()
    {
        testcase("addSignature updates existing");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);
        std::string const tag = "add-signature-update";
        auto const txnHash = sha512Half(makeSlice(tag));

        STObject signerA(sfSigner);
        signerA.setAccountID(sfAccount, validatorAcc);
        signerA.setFieldVL(sfSigningPubKey, validator.first.slice());
        Blob sigA{0xAA};
        signerA.setFieldVL(sfTxnSignature, sigA);

        STObject signerB(sfSigner);
        signerB.setAccountID(sfAccount, validatorAcc);
        signerB.setFieldVL(sfSigningPubKey, validator.first.slice());
        Blob sigB{0xBB};
        signerB.setFieldVL(sfTxnSignature, sigB);

        collector.addSignature(txnHash, validator.first, signerA, 10);
        collector.addSignature(txnHash, validator.first, signerB, 20);

        BEAST_EXPECT(collector.signatureCount(txnHash) == 1);
        auto const stored =
            collector.getSignatureFrom(txnHash, validator.first);
        BEAST_EXPECT(stored);
        if (stored)
            BEAST_EXPECT(stored->getFieldVL(sfTxnSignature) == sigB);
    }

    void
    testRejectedSignatureDoesNotCreatePendingEntry()
    {
        testcase("reject does not create pending entry");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);
        collector.stashTxnData(txnHash, txData);

        Serializer sigData = buildMultiSigningData(tx, validatorAcc);
        auto const goodSigBuf =
            sign(validator.first, validator.second, sigData.slice());
        Blob badSig(goodSigBuf.begin(), goodSigBuf.end());
        badSig.back() ^= 0x01;

        auto badSigner = makeSigner(validator.first, validatorAcc, badSig);
        BEAST_EXPECT(!collector.verifyAndAddSignature(
            txnHash, validator.first, badSigner, 500));
        BEAST_EXPECT(collector.signatureCount(txnHash) == 0);
        BEAST_EXPECT(collector.getPendingExports().empty());
    }

    void
    testAddSignatureRejectsIdentityMismatch()
    {
        testcase("addSignature rejects identity mismatch");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validatorA = randomKeyPair(KeyType::secp256k1);
        auto const validatorB = randomKeyPair(KeyType::secp256k1);
        auto const validatorBAcc = calcAccountID(validatorB.first);
        std::string const tag = "add-signature-identity-mismatch";
        auto const txnHash = sha512Half(makeSlice(tag));

        STObject signer(sfSigner);
        signer.setAccountID(sfAccount, validatorBAcc);
        signer.setFieldVL(sfSigningPubKey, validatorB.first.slice());
        signer.setFieldVL(sfTxnSignature, Blob{0x01, 0x02});

        collector.addSignature(txnHash, validatorA.first, signer, 600);

        BEAST_EXPECT(!collector.hasSignatureFrom(txnHash, validatorA.first));
        BEAST_EXPECT(collector.signatureCount(txnHash) == 0);
    }

    void
    testAddSignatureVerifiesWhenTxnDataPresent()
    {
        testcase("addSignature verifies when txn data present");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);
        collector.stashTxnData(txnHash, txData);

        Serializer sigData = buildMultiSigningData(tx, validatorAcc);
        auto const goodSigBuf =
            sign(validator.first, validator.second, sigData.slice());
        Blob goodSig(goodSigBuf.begin(), goodSigBuf.end());
        auto signer = makeSigner(validator.first, validatorAcc, goodSig);

        collector.addSignature(txnHash, validator.first, signer, 601);

        BEAST_EXPECT(collector.hasSignatureFrom(txnHash, validator.first));
        BEAST_EXPECT(collector.isSignatureVerified(txnHash, validator.first));
    }

    void
    testAddSignatureRejectsInvalidReplacementWhenVerified()
    {
        testcase("addSignature rejects invalid replacement when verified");

        beast::Journal journal{beast::Journal::getNullSink()};
        ExportSignatureCollector collector{journal};

        auto const validator = randomKeyPair(KeyType::secp256k1);
        auto const validatorAcc = calcAccountID(validator.first);

        auto tx = makeUnsignedTx();
        auto const txnHash = tx.getTransactionID();

        Serializer txData;
        tx.add(txData);
        collector.stashTxnData(txnHash, txData);

        Serializer sigData = buildMultiSigningData(tx, validatorAcc);
        auto const goodSigBuf =
            sign(validator.first, validator.second, sigData.slice());
        Blob goodSig(goodSigBuf.begin(), goodSigBuf.end());
        auto goodSigner = makeSigner(validator.first, validatorAcc, goodSig);
        collector.addSignature(txnHash, validator.first, goodSigner, 602);

        Blob badSig = goodSig;
        badSig.back() ^= 0x01;
        auto badSigner = makeSigner(validator.first, validatorAcc, badSig);
        collector.addSignature(txnHash, validator.first, badSigner, 603);

        auto const stored =
            collector.getSignatureFrom(txnHash, validator.first);
        BEAST_EXPECT(stored);
        if (stored)
            BEAST_EXPECT(stored->getFieldVL(sfTxnSignature) == goodSig);
        BEAST_EXPECT(collector.isSignatureVerified(txnHash, validator.first));
    }

public:
    void
    run() override
    {
        testDuplicateCanReplaceUnverified();
        testVerifiedIsNotReplaced();
        testRejectsSignerIdentityMismatch();
        testStashPrunesInvalidUnverified();
        testInvalidEarlyDataDoesNotAgeOutValidLater();
        testAddSignatureUpdatesExisting();
        testRejectedSignatureDoesNotCreatePendingEntry();
        testAddSignatureRejectsIdentityMismatch();
        testAddSignatureVerifiesWhenTxnDataPresent();
        testAddSignatureRejectsInvalidReplacementWhenVerified();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSignatureCollector, app, ripple);

}  // namespace test
}  // namespace ripple
