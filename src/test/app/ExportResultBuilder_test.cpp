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
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Seed.h>
#include <xrpl/protocol/Sign.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
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

std::optional<STTx>
makeExportedPaymentWithSize(
    AccountID const& src,
    AccountID const& dst,
    std::size_t const serializedSize)
{
    auto const base = makeExportedPayment(src, dst);
    for (std::size_t payloadBytes = 0; payloadBytes <= serializedSize;
         ++payloadBytes)
    {
        auto candidate = base;
        STArray memos(sfMemos);
        STObject memo(sfMemo);
        memo.setFieldVL(sfMemoData, Blob(payloadBytes, 0xCC));
        memos.emplace_back(std::move(memo));
        candidate.setFieldArray(sfMemos, std::move(memos));

        auto const size = candidate.getSerializer().size();
        if (size == serializedSize)
            return candidate;
        if (size > serializedSize)
            return std::nullopt;
    }
    return std::nullopt;
}

STTx
makeExportedPaymentChannelClaim(
    AccountID const& src,
    Blob const& channelSignature)
{
    STObject obj(sfExportedTxn);
    obj.setFieldU16(sfTransactionType, ttPAYCHAN_CLAIM);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setFieldU32(sfSequence, 0);
    obj.setFieldU32(sfTicketSequence, 1);
    obj.setFieldU32(sfFirstLedgerSequence, 2);
    obj.setFieldU32(sfLastLedgerSequence, 6);
    obj.setFieldAmount(sfFee, XRPAmount{10});
    obj.setFieldVL(sfSigningPubKey, Blob{});
    obj.setAccountID(sfAccount, src);
    obj.setFieldH256(sfChannel, makeHash("payment-channel"));
    obj.setFieldVL(sfSignature, channelSignature);
    return makeSTTx(obj);
}

ExportResultBuilder::PositionedSignatureSnapshot
positionedSignatures(ExportResultBuilder::SignatureSnapshot const& signatures)
{
    ExportResultBuilder::PositionedSignatureSnapshot result;
    std::uint16_t position = 0;
    for (auto const& [key, signature] : signatures)
    {
        result.emplace(
            position++,
            ExportResultBuilder::PositionedSignature{key, signature});
    }
    return result;
}

std::pair<PublicKey, SecretKey>
deterministicKeyPair(char const* label)
{
    return generateKeyPair(KeyType::secp256k1, generateSeed(label));
}

}  // namespace

class ExportResultBuilder_test : public beast::unit_test::suite
{
public:
    void
    testAssemblesMultiSignedTransaction()
    {
        testcase("assembles multisigned transaction");

        auto const signerA = randomKeyPair(KeyType::secp256k1);
        auto const signerB = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(signerA.first), calcAccountID(signerB.first));
        ExportResultBuilder::SignatureSnapshot signatures;
        signatures.emplace(
            signerA.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerA.first, signerA.second));
        signatures.emplace(
            signerB.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerB.first, signerB.second));

        auto const multiSigned =
            ExportResultBuilder::buildMultiSignedExportedTxn(
                innerTx, signatures);
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

        auto const signedTxHash =
            multiSigned.getHash(HashPrefix::transactionID);

        Serializer serialized;
        multiSigned.add(serialized);
        SerialIter sit(serialized.slice());
        STTx signedTx{std::ref(sit)};
        BEAST_EXPECT(signedTx.getTransactionID() == signedTxHash);
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
    testExportIntentHashIgnoresSignerSubset()
    {
        testcase("export intent hash ignores signer subset");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const otherDst = randomKeyPair(KeyType::secp256k1);
        auto const signerA = randomKeyPair(KeyType::secp256k1);
        auto const signerB = randomKeyPair(KeyType::secp256k1);
        auto const signerC = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));

        ExportResultBuilder::SignatureSnapshot subsetAB;
        subsetAB.emplace(
            signerA.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerA.first, signerA.second));
        subsetAB.emplace(
            signerB.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerB.first, signerB.second));

        ExportResultBuilder::SignatureSnapshot subsetBC;
        subsetBC.emplace(
            signerB.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerB.first, signerB.second));
        subsetBC.emplace(
            signerC.first,
            ExportResultBuilder::signExportedTxn(
                innerTx, signerC.first, signerC.second));

        auto const signedAB =
            makeSTTx(ExportResultBuilder::buildMultiSignedExportedTxn(
                innerTx, subsetAB));
        auto const signedBC =
            makeSTTx(ExportResultBuilder::buildMultiSignedExportedTxn(
                innerTx, subsetBC));

        BEAST_EXPECT(
            signedAB.getTransactionID() != signedBC.getTransactionID());
        auto const intentHash = ExportResultBuilder::exportIntentHash(innerTx);
        BEAST_EXPECT(
            ExportResultBuilder::exportIntentHash(signedAB) == intentHash);
        BEAST_EXPECT(
            ExportResultBuilder::exportIntentHash(signedBC) == intentHash);

        auto singleAuthorized = innerTx;
        singleAuthorized.setFieldVL(sfSigningPubKey, signerA.first.slice());
        singleAuthorized.setFieldVL(sfTxnSignature, Blob{1, 2, 3});
        BEAST_EXPECT(
            ExportResultBuilder::exportIntentHash(singleAuthorized) ==
            intentHash);

        auto const otherIntent = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(otherDst.first));
        BEAST_EXPECT(
            ExportResultBuilder::exportIntentHash(otherIntent) != intentHash);

        Blob const claimSignatureA{1, 2, 3};
        auto const claimA = makeExportedPaymentChannelClaim(
            calcAccountID(src.first), claimSignatureA);
        auto const claimB = makeExportedPaymentChannelClaim(
            calcAccountID(src.first), Blob{4, 5, 6});
        BEAST_EXPECT(
            ExportResultBuilder::exportIntentHash(claimA) ==
            ExportResultBuilder::exportIntentHash(claimB));

        auto const normalizedClaim =
            ExportResultBuilder::buildMultiSignedExportedTxn(claimA, {});
        BEAST_EXPECT(
            normalizedClaim.getFieldVL(sfSignature) == claimSignatureA);
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
        while (signatures.size() < STTx::maxMultiSigners() + 4)
        {
            auto const signer = randomKeyPair(KeyType::secp256k1);
            signatures.emplace(
                signer.first,
                ExportResultBuilder::signExportedTxn(
                    innerTx, signer.first, signer.second));
        }

        auto const multiSigned =
            ExportResultBuilder::buildMultiSignedExportedTxn(
                innerTx, signatures);
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
        while (signatures.size() < STTx::maxMultiSigners())
        {
            auto const signer = randomKeyPair(KeyType::secp256k1);
            signatures.emplace(
                signer.first,
                ExportResultBuilder::signExportedTxn(
                    innerTx, signer.first, signer.second));
        }

        auto const positioned = positionedSignatures(signatures);
        Blob const contributors(
            STTx::maxMultiSigners() / 8, std::uint8_t{0xFF});
        auto witness = ExportResultBuilder::buildSignatureWitness(
            exportTxHash, innerTx, positioned, STTx::maxMultiSigners(), 654);
        BEAST_EXPECT(witness.getTxnType() == ttEXPORT_SIGNATURES);
        BEAST_EXPECT(witness.getFieldU32(sfLedgerSequence) == 654);
        BEAST_EXPECT(witness.getFieldH256(sfTransactionHash) == exportTxHash);
        BEAST_EXPECT(witness.getFieldVL(sfExportContributors) == contributors);
        BEAST_EXPECT(!witness.isFieldPresent(sfSigners));

        auto const& exported =
            witness.peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(exported.getFieldU16(sfTransactionType) == ttPAYMENT);
        BEAST_EXPECT(!exported.isFieldPresent(sfSigners));
        BEAST_EXPECT(
            witness.getFieldArray(sfExportSigners).size() ==
            STTx::maxMultiSigners());

        auto decoded = ExportResultBuilder::signaturesFromWitness(witness);
        BEAST_EXPECT(decoded);
        if (decoded)
        {
            BEAST_EXPECT(decoded->size() == STTx::maxMultiSigners());
            std::uint16_t expectedPosition = 0;
            for (auto const& [position, signature] : *decoded)
            {
                BEAST_EXPECT(position == expectedPosition++);
                auto const it = signatures.find(signature.signingKey);
                BEAST_EXPECT(it != signatures.end());
                if (it != signatures.end())
                    BEAST_EXPECT(it->second == signature.signature);
            }
        }
    }

    void
    testSparseSignatureWitnessRoundTrip()
    {
        testcase("sparse cross-byte signature witness round trip");

        auto const src = deterministicKeyPair("d-attrib-source");
        auto const dst = deterministicKeyPair("d-attrib-destination");
        auto const signer0 = deterministicKeyPair("d-attrib-signer-0");
        auto const signer7 = deterministicKeyPair("d-attrib-signer-7");
        auto const signer9 = deterministicKeyPair("d-attrib-signer-9");
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));

        ExportResultBuilder::PositionedSignatureSnapshot signatures;
        for (auto const& [position, signer] : std::array{
                 std::pair{std::uint16_t{0}, std::cref(signer0)},
                 std::pair{std::uint16_t{7}, std::cref(signer7)},
                 std::pair{std::uint16_t{9}, std::cref(signer9)}})
        {
            signatures.emplace(
                position,
                ExportResultBuilder::PositionedSignature{
                    signer.get().first,
                    ExportResultBuilder::signExportedTxn(
                        innerTx, signer.get().first, signer.get().second)});
        }

        auto const witness = ExportResultBuilder::buildSignatureWitness(
            makeHash("d-attrib-export"), innerTx, signatures, 10, 654);
        Blob const expectedContributors{0x81, 0x02};
        BEAST_EXPECT(
            witness.getFieldVL(sfExportContributors) == expectedContributors);

        auto const serialized = witness.getSerializer();
        BEAST_EXPECT(
            strHex(serialized.slice()) ==
            "12006A2400000000260000028E5316E18499CA512AC0B68C103C249FF20029D4"
            "6CEFCCD8E6882A481768546DCBF368400000000000000073007023028102811400"
            "00000000000000000000000000000000000000E05A1200002280000000240000"
            "0000201A00000002201B000000062029000000016140000000000F424068400000"
            "000000000A7300811431C972A33313AC474C4996CF7463C8D5648AA1B283143A9"
            "D59046AB0269024A1C12118FB57F514A891BEE1F017E011732102006E9F20ADB30"
            "B695D059FEA08BF342FA6BD712433B769FD8D1A40098F4780F674463044022033"
            "A39198530AD920FB0F21A041E4CFD2C9509C085B541BD1058F2B71F8D3F01A02"
            "206B203D44035D8D653F5F146CF05EA769FB60ABC6246D3EC42CD5B14C541B583"
            "3E1E0117321030D2368AFD1505BCFDCF91815E073871917B2E276CF1999E0B781"
            "E9EC681B68D774473045022100824DDF6B8F117F53F11B35EFACC608676B3A0C2"
            "19591A01E446BFAE747DAE6AC02205955CB02DAA217DE0E3617E0B75E66FE5D09"
            "6326C4E754DDC46DFCEB992CC394E1E0117321028B8C98F69CFF73B9D66CC108"
            "8161E10E9B31AE2C075BADC4656C27D10EDE23D774473045022100C4F6FC146A"
            "2406E20E527572957265FB9EAA3C2F22D40D3B5924880D8E70797602202E54B5"
            "C0FDDD58D45909F7B47FB5B90E9DE3DD57A1B167E4FDA10CF48E2BFD0FE1F1");
        BEAST_EXPECT(
            to_string(witness.getTransactionID()) ==
            "C530A5D5E117E04A75C90A1E0F83CEB7C19CCA7AEBA0B190FC82F82BEF740FB6");
        SerialIter iter{serialized.slice()};
        STTx roundTripped{std::ref(iter)};
        BEAST_EXPECT(
            roundTripped.getFieldVL(sfExportContributors) ==
            expectedContributors);
        BEAST_EXPECT(
            roundTripped.getTransactionID() == witness.getTransactionID());

        auto const decoded =
            ExportResultBuilder::signaturesFromWitness(roundTripped);
        BEAST_EXPECT(decoded);
        if (decoded)
        {
            BEAST_EXPECT(decoded->size() == signatures.size());
            auto expected = signatures.begin();
            for (auto const& [position, signature] : *decoded)
            {
                BEAST_EXPECT(expected != signatures.end());
                if (expected == signatures.end())
                    break;
                BEAST_EXPECT(position == expected->first);
                BEAST_EXPECT(
                    signature.signingKey == expected->second.signingKey);
                BEAST_EXPECT(signature.signature == expected->second.signature);
                ++expected;
            }
            BEAST_EXPECT(expected == signatures.end());
        }
    }

    void
    testDestinationSignerOrderIsIndependent()
    {
        testcase("destination signer order is independent of witness order");

        auto const src = deterministicKeyPair("d-attrib-order-source");
        auto const dst = deterministicKeyPair("d-attrib-order-destination");
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));
        std::array signers{
            deterministicKeyPair("d-attrib-order-signer-0"),
            deterministicKeyPair("d-attrib-order-signer-1"),
            deterministicKeyPair("d-attrib-order-signer-2")};
        std::sort(
            signers.begin(),
            signers.end(),
            [](auto const& lhs, auto const& rhs) {
                return calcAccountID(lhs.first) < calcAccountID(rhs.first);
            });

        ExportResultBuilder::PositionedSignatureSnapshot positioned;
        std::array<std::uint16_t, 3> const positions{0, 7, 9};
        for (std::size_t i = 0; i < signers.size(); ++i)
        {
            auto const& signer = signers[signers.size() - i - 1];
            positioned.emplace(
                positions[i],
                ExportResultBuilder::PositionedSignature{
                    signer.first,
                    ExportResultBuilder::signExportedTxn(
                        innerTx, signer.first, signer.second)});
        }

        auto const witness = ExportResultBuilder::buildSignatureWitness(
            makeHash("d-attrib-order-export"), innerTx, positioned, 10, 654);
        auto const& witnessEntries = witness.getFieldArray(sfExportSigners);
        BEAST_EXPECT(witnessEntries.size() == signers.size());
        if (witnessEntries.size() == signers.size())
        {
            for (std::size_t i = 0; i < witnessEntries.size(); ++i)
            {
                auto const key = PublicKey{
                    makeSlice(witnessEntries[i].getFieldVL(sfSigningPubKey))};
                BEAST_EXPECT(
                    calcAccountID(key) ==
                    calcAccountID(signers[signers.size() - i - 1].first));
            }
        }

        auto const decoded =
            ExportResultBuilder::signaturesFromWitness(witness);
        BEAST_EXPECT(decoded);
        if (!decoded)
            return;

        ExportResultBuilder::SignatureSnapshot destinationSignatures;
        for (auto const& [_, signature] : *decoded)
            destinationSignatures.emplace(
                signature.signingKey, signature.signature);
        auto const assembled = ExportResultBuilder::buildMultiSignedExportedTxn(
            innerTx, destinationSignatures);
        auto const& destinationEntries = assembled.getFieldArray(sfSigners);
        BEAST_EXPECT(destinationEntries.size() == signers.size());
        if (destinationEntries.size() == signers.size())
        {
            for (std::size_t i = 0; i < destinationEntries.size(); ++i)
            {
                BEAST_EXPECT(
                    destinationEntries[i].getAccountID(sfAccount) ==
                    calcAccountID(signers[i].first));
            }
        }
    }

    void
    testRejectsMalformedWitnessSigners()
    {
        testcase("signature witness rejects malformed signer entries");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const signer0 = randomKeyPair(KeyType::secp256k1);
        auto const signer1 = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));
        auto const exportTxHash = makeHash("bad-witness-signer");

        ExportResultBuilder::PositionedSignatureSnapshot signatures;
        signatures.emplace(
            0,
            ExportResultBuilder::PositionedSignature{
                signer0.first,
                ExportResultBuilder::signExportedTxn(
                    innerTx, signer0.first, signer0.second)});
        signatures.emplace(
            1,
            ExportResultBuilder::PositionedSignature{
                signer1.first,
                ExportResultBuilder::signExportedTxn(
                    innerTx, signer1.first, signer1.second)});

        auto const witness = ExportResultBuilder::buildSignatureWitness(
            exportTxHash, innerTx, signatures, 2, 654);
        BEAST_EXPECT(ExportResultBuilder::signaturesFromWitness(witness));

        using Mutator = void (*)(STArray&);
        struct MalformedCase
        {
            char const* name;
            Mutator mutate;
        };
        std::array<MalformedCase, 8> const malformedCases{{
            {"missing public key",
             [](STArray& entries) { entries[0].delField(sfSigningPubKey); }},
            {"missing signature",
             [](STArray& entries) { entries[0].delField(sfTxnSignature); }},
            {"extra account",
             [](STArray& entries) {
                 entries[0].setAccountID(sfAccount, AccountID{});
             }},
            {"wrong entry type",
             [](STArray& entries) { entries[0].setFName(sfSigner); }},
            {"malformed public key",
             [](STArray& entries) {
                 entries[0].setFieldVL(sfSigningPubKey, Blob{0x02});
             }},
            {"empty signature",
             [](STArray& entries) {
                 entries[0].setFieldVL(sfTxnSignature, Blob{});
             }},
            {"oversized signature",
             [](STArray& entries) {
                 entries[0].setFieldVL(
                     sfTxnSignature,
                     Blob(
                         ExportLimits::maxCanonicalExportSignatureBytes + 1,
                         0x30));
             }},
            {"duplicate key",
             [](STArray& entries) {
                 entries[1].setFieldVL(
                     sfSigningPubKey, entries[0].getFieldVL(sfSigningPubKey));
             }},
        }};

        for (auto const& malformedCase : malformedCases)
        {
            auto malformed = witness;
            auto& entries = malformed.peekFieldArray(sfExportSigners);
            malformedCase.mutate(entries);
            BEAST_EXPECTS(
                !ExportResultBuilder::signaturesFromWitness(malformed),
                malformedCase.name);
        }

        auto malformedBitmap = ExportResultBuilder::buildSignatureWitness(
            exportTxHash, innerTx, signatures, 2, 654);
        malformedBitmap.setFieldVL(sfExportContributors, Blob{0x00});
        BEAST_EXPECT(
            !ExportResultBuilder::signaturesFromWitness(malformedBitmap));

        ExportResultBuilder::PositionedSignatureSnapshot oversized;
        for (std::uint16_t position = 0;
             position <= ExportLimits::maxCommitteeMembers;
             ++position)
        {
            auto const key = randomKeyPair(KeyType::secp256k1).first;
            oversized.emplace(
                position,
                ExportResultBuilder::PositionedSignature{key, Buffer{0x01}});
        }
        except([&] {
            ExportResultBuilder::buildSignatureWitness(
                exportTxHash, innerTx, oversized, oversized.size(), 654);
        });
    }

    void
    testSerializedSizeInventory()
    {
        //@@start export-serialized-size-inventory
        testcase("serialized size inventory");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto innerTx = makeExportedPayment(
            calcAccountID(src.first), calcAccountID(dst.first));

        innerTx.setFieldArray(sfMemos, STArray(sfMemos, 1));
        STObject memo{sfMemo};
        memo.setFieldVL(
            sfMemoType,
            Blob{
                'x',
                'a',
                'h',
                'a',
                'u',
                '-',
                'e',
                'x',
                'p',
                'o',
                'r',
                't',
                '-',
                'v',
                '1'});
        memo.setFieldVL(sfMemoData, Blob(45, 0xA5));
        innerTx.peekFieldArray(sfMemos).emplace_back(std::move(memo));

        std::array<std::uint8_t, 72> signatureBytes;
        signatureBytes.fill(0x5A);
        ExportResultBuilder::SignatureSnapshot signatures;
        while (signatures.size() < STTx::maxMultiSigners())
        {
            auto const signer = randomKeyPair(KeyType::secp256k1);
            signatures.emplace(
                signer.first,
                Buffer{signatureBytes.data(), signatureBytes.size()});
        }

        auto const multiSigned =
            ExportResultBuilder::buildMultiSignedExportedTxn(
                innerTx, signatures);
        auto const positioned = positionedSignatures(signatures);
        auto const witness = ExportResultBuilder::buildSignatureWitness(
            makeHash("size-inventory-export"),
            innerTx,
            positioned,
            STTx::maxMultiSigners(),
            654);
        auto const release = ExportOriginMemo::releaseForm(
            innerTx,
            ExportOriginMemo::Origin{21337, 0, makeHash("size-origin")},
            ExportOriginMemo::Anchor{653, makeHash("size-ledger")});
        BEAST_EXPECT(release);
        if (!release)
            return;
        auto const releaseWitness = ExportResultBuilder::buildSignatureWitness(
            makeHash("size-inventory-export"),
            release.value(),
            positioned,
            STTx::maxMultiSigners(),
            654);

        auto const innerBytes = innerTx.getSerializer().size();
        auto const multiSignedBytes = multiSigned.getSerializer().size();
        auto const selfContainedWitnessBytes = witness.getSerializer().size();
        auto const releaseWitnessBytes = releaseWitness.getSerializer().size();
        auto const pricedWitnessBytes = innerBytes +
            ExportLimits::feeWitnessFixedAllowanceBytes +
            STTx::maxMultiSigners() *
                ExportLimits::feeWitnessSignerAllowanceBytes;
        constexpr std::size_t legacyShareBytes = 32 + 33 + 72;

        log << "Export serialized-size inventory:\n"
            << "  unsigned target + issuance Memo: " << innerBytes << "\n"
            << "  32-signer target: " << multiSignedBytes << "\n"
            << "  self-contained 32-signer witness: "
            << selfContainedWitnessBytes << "\n"
            << "  stamped self-contained witness: " << releaseWitnessBytes
            << "\n"
            << "  one legacy share blob: " << legacyShareBytes << "\n"
            << "  32 legacy share blobs: "
            << legacyShareBytes * STTx::maxMultiSigners() << std::endl;

        BEAST_EXPECT(signatures.size() == STTx::maxMultiSigners());
        BEAST_EXPECT(!witness.isFieldPresent(sfSigners));
        auto const& exported =
            witness.peekAtField(sfExportedTxn).downcast<STObject>();
        BEAST_EXPECT(!exported.isFieldPresent(sfSigners));
        BEAST_EXPECT(
            witness.getFieldArray(sfExportSigners).size() ==
            STTx::maxMultiSigners());
        Blob const contributors(
            STTx::maxMultiSigners() / 8, std::uint8_t{0xFF});
        BEAST_EXPECT(witness.getFieldVL(sfExportContributors) == contributors);
        BEAST_EXPECT(innerBytes == 163);
        BEAST_EXPECT(multiSignedBytes == 4453);
        BEAST_EXPECT(selfContainedWitnessBytes == 3839);
        BEAST_EXPECT(pricedWitnessBytes == 4643);
        BEAST_EXPECT(releaseWitnessBytes > selfContainedWitnessBytes);
        BEAST_EXPECT(pricedWitnessBytes >= releaseWitnessBytes);
        BEAST_EXPECT(pricedWitnessBytes <= ExportLimits::maxExportWitnessBytes);
        BEAST_EXPECT(legacyShareBytes * STTx::maxMultiSigners() == 4384);

        STArray oversizedMemos(sfMemos);
        STObject oversizedMemo(sfMemo);
        oversizedMemo.setFieldVL(sfMemoData, Blob(6'000, 0xCC));
        oversizedMemos.emplace_back(std::move(oversizedMemo));

        auto oversizedTarget = innerTx;
        oversizedTarget.setFieldArray(sfMemos, oversizedMemos);
        except([&] {
            ExportResultBuilder::buildSignatureWitness(
                makeHash("oversized-witness"),
                oversizedTarget,
                positioned,
                STTx::maxMultiSigners(),
                654);
        });

        auto oversizedWitness = witness;
        auto& oversizedExported = const_cast<STObject&>(
            oversizedWitness.peekAtField(sfExportedTxn).downcast<STObject>());
        oversizedExported.setFieldArray(sfMemos, std::move(oversizedMemos));
        BEAST_EXPECT(
            !ExportResultBuilder::signaturesFromWitness(oversizedWitness));
        //@@end export-serialized-size-inventory
    }

    void
    testReleaseTargetSizeBoundary()
    {
        testcase("release target serialized size boundary");

        auto const src = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const signer = randomKeyPair(KeyType::secp256k1);
        auto const atLimit = makeExportedPaymentWithSize(
            calcAccountID(src.first),
            calcAccountID(dst.first),
            ExportLimits::maxExportReleaseTargetBytes);
        auto const overLimit = makeExportedPaymentWithSize(
            calcAccountID(src.first),
            calcAccountID(dst.first),
            ExportLimits::maxExportReleaseTargetBytes + 1);
        BEAST_EXPECT(atLimit);
        BEAST_EXPECT(overLimit);
        if (!atLimit || !overLimit)
            return;

        ExportResultBuilder::PositionedSignatureSnapshot signatures;
        signatures.emplace(
            0,
            ExportResultBuilder::PositionedSignature{
                signer.first,
                ExportResultBuilder::signExportedTxn(
                    *atLimit, signer.first, signer.second)});

        auto const witness = ExportResultBuilder::buildSignatureWitness(
            makeHash("target-at-limit"), *atLimit, signatures, 1, 654);
        BEAST_EXPECT(ExportResultBuilder::signaturesFromWitness(witness));

        except([&] {
            ExportResultBuilder::buildSignatureWitness(
                makeHash("target-over-limit"), *overLimit, signatures, 1, 654);
        });

        auto oversizedWitness = witness;
        auto& embedded = const_cast<STObject&>(
            oversizedWitness.peekAtField(sfExportedTxn).downcast<STObject>());
        embedded.setFieldArray(sfMemos, overLimit->getFieldArray(sfMemos));
        Serializer embeddedBytes;
        embedded.add(embeddedBytes);
        BEAST_EXPECT(
            embeddedBytes.size() ==
            ExportLimits::maxExportReleaseTargetBytes + 1);
        BEAST_EXPECT(
            !ExportResultBuilder::signaturesFromWitness(oversizedWitness));
    }

    void
    run() override
    {
        testAssemblesMultiSignedTransaction();
        testBuildMultiSignedExportedTxnDirect();
        testExportIntentHashIgnoresSignerSubset();
        testCapsSignerArray();
        testSignatureWitnessRoundTrip();
        testSparseSignatureWitnessRoundTrip();
        testDestinationSignerOrderIsIndependent();
        testRejectsMalformedWitnessSigners();
        testSerializedSizeInventory();
        testReleaseTargetSizeBoundary();
    }
};

BEAST_DEFINE_TESTSUITE(ExportResultBuilder, app, ripple);

}  // namespace test
}  // namespace ripple
