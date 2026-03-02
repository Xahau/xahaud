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

public:
    void
    run() override
    {
        testDuplicateCanReplaceUnverified();
        testVerifiedIsNotReplaced();
        testRejectsSignerIdentityMismatch();
    }
};

BEAST_DEFINE_TESTSUITE(ExportSignatureCollector, app, ripple);

}  // namespace test
}  // namespace ripple
