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

#include <xrpld/app/hook/detail/XportWrapperBuilder.h>
#include <xrpl/basics/Expected.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ExportLimits.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/STObject.h>
#include <xrpl/protocol/STTx.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFlags.h>
#include <xrpl/protocol/TxFormats.h>
#include <xrpl/protocol/digest.h>

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

Blob
serialize(STTx const& tx)
{
    Serializer s;
    tx.add(s);
    return {s.begin(), s.end()};
}

STTx
makeExportedPayment(
    AccountID const& src,
    AccountID const& dst,
    std::optional<std::uint32_t> networkID = std::nullopt,
    std::optional<std::uint32_t> ticketSequence = 1,
    std::uint32_t sequence = 0)
{
    STObject obj(sfExportedTxn);
    obj.setFieldU16(sfTransactionType, ttPAYMENT);
    obj.setFieldU32(sfFlags, tfFullyCanonicalSig);
    obj.setFieldU32(sfSequence, sequence);
    if (ticketSequence)
        obj.setFieldU32(sfTicketSequence, *ticketSequence);
    obj.setFieldU32(sfFirstLedgerSequence, 2);
    obj.setFieldU32(sfLastLedgerSequence, 6);
    obj.setFieldAmount(sfAmount, XRPAmount{1000000});
    obj.setFieldAmount(sfFee, XRPAmount{10});
    obj.setFieldVL(sfSigningPubKey, Blob{});
    obj.setAccountID(sfAccount, src);
    obj.setAccountID(sfDestination, dst);
    if (networkID)
        obj.setFieldU32(sfNetworkID, *networkID);
    return makeSTTx(obj);
}

STTx
withMemo(STTx tx, Blob type, Blob data)
{
    STArray memos = tx.isFieldPresent(sfMemos) ? tx.getFieldArray(sfMemos)
                                               : STArray{sfMemos};
    STObject memo{sfMemo};
    memo.setFieldVL(sfMemoType, std::move(type));
    memo.setFieldVL(sfMemoData, std::move(data));
    memos.emplace_back(std::move(memo));
    tx.setFieldArray(sfMemos, memos);
    return tx;
}

beast::Journal
nullJournal()
{
    return beast::Journal{beast::Journal::getNullSink()};
}

hook::XportWrapperBuilder::Input
makeInput(
    Slice innerTxBlob,
    AccountID const& exporter,
    std::uint32_t networkID = 21337,
    hook::XportWrapperBuilder::NonceGenerator generateNonce =
        [] {
            return Expected<uint256, ::hook_api::hook_return_code>{
                makeHash("nonce")};
        },
    hook::XportWrapperBuilder::FeeCalculator calculateFee =
        [](Slice const&) {
            return Expected<std::uint64_t, ::hook_api::hook_return_code>{12345};
        })
{
    return hook::XportWrapperBuilder::Input{
        innerTxBlob,
        exporter,
        networkID,
        10,
        makeHash("parent-tx"),
        makeHash("hook-hash"),
        true,
        3,
        7,
        std::move(generateNonce),
        std::move(calculateFee),
        nullJournal()};
}

}  // namespace

class XportWrapperBuilder_test : public beast::unit_test::suite
{
public:
    void
    testBuildsWrapper()
    {
        testcase("builds xport wrapper");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(exporter.first), calcAccountID(dst.first));
        auto const serialized = serialize(innerTx);

        auto const result = hook::XportWrapperBuilder::build(makeInput(
            Slice(serialized.data(), serialized.size()),
            calcAccountID(exporter.first)));

        BEAST_EXPECT(result);
        if (!result)
            return;

        auto const& wrapper = result->wrapperTx;
        BEAST_EXPECT(wrapper.getTransactionID() != innerTx.getTransactionID());
        BEAST_EXPECT(wrapper.getTxnType() == ttEXPORT);
        BEAST_EXPECT(
            wrapper.getAccountID(sfAccount) == calcAccountID(exporter.first));
        BEAST_EXPECT(wrapper.getFieldU32(sfSequence) == 0);
        BEAST_EXPECT(wrapper.getFieldU32(sfFirstLedgerSequence) == 11);
        BEAST_EXPECT(
            wrapper.getFieldU32(sfLastLedgerSequence) ==
            10 + ExportLimits::maxRetryLedgers);
        BEAST_EXPECT(wrapper.getFieldAmount(sfFee) == STAmount{12345});
        BEAST_EXPECT(wrapper.getFieldVL(sfSigningPubKey).empty());

        auto const& exported =
            wrapper.peekAtField(sfExportedTxn).downcast<STObject>();
        Serializer exportedSer;
        exported.add(exportedSer);
        STTx parsedInner{SerialIter{exportedSer.slice()}};
        BEAST_EXPECT(
            parsedInner.getTransactionID() == innerTx.getTransactionID());

        auto const& emitDetails =
            wrapper.peekAtField(sfEmitDetails).downcast<STObject>();
        BEAST_EXPECT(emitDetails.getFieldU32(sfEmitGeneration) == 3);
        BEAST_EXPECT(emitDetails.getFieldU64(sfEmitBurden) == 7);
        BEAST_EXPECT(
            emitDetails.getFieldH256(sfEmitParentTxnID) ==
            makeHash("parent-tx"));
        BEAST_EXPECT(
            emitDetails.getFieldH256(sfEmitNonce) == makeHash("nonce"));
        BEAST_EXPECT(
            emitDetails.getFieldH256(sfEmitHookHash) == makeHash("hook-hash"));
        BEAST_EXPECT(
            emitDetails.getAccountID(sfEmitCallback) ==
            calcAccountID(exporter.first));
    }

    void
    testBuildsWrapperWithoutCallback()
    {
        testcase("builds xport wrapper without callback");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(exporter.first), calcAccountID(dst.first));
        auto const serialized = serialize(innerTx);

        auto input = makeInput(
            Slice(serialized.data(), serialized.size()),
            calcAccountID(exporter.first));
        input.hasCallback = false;

        auto const result = hook::XportWrapperBuilder::build(input);
        BEAST_EXPECT(result);
        if (!result)
            return;

        auto const& emitDetails =
            result->wrapperTx.peekAtField(sfEmitDetails).downcast<STObject>();
        BEAST_EXPECT(!emitDetails.isFieldPresent(sfEmitCallback));
    }

    void
    testPreservesUserMemos()
    {
        testcase("preserves user memos");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = withMemo(
            makeExportedPayment(
                calcAccountID(exporter.first), calcAccountID(dst.first)),
            Blob{'u', 's', 'e', 'r'},
            Blob{1, 2, 3, 4});
        auto const serialized = serialize(innerTx);

        auto const result = hook::XportWrapperBuilder::build(makeInput(
            Slice(serialized.data(), serialized.size()),
            calcAccountID(exporter.first)));

        BEAST_EXPECT(result);
        if (!result)
            return;

        auto const& exported =
            result->wrapperTx.peekAtField(sfExportedTxn).downcast<STObject>();
        Serializer exportedSer;
        exported.add(exportedSer);
        STTx parsedInner{SerialIter{exportedSer.slice()}};
        BEAST_EXPECT(serialize(parsedInner) == serialized);
        BEAST_EXPECT(!ExportOriginMemo::hasReservedMemo(parsedInner));
    }

    void
    testRejectsInvalidInputs()
    {
        testcase("rejects invalid inputs");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const other = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(exporter.first), calcAccountID(dst.first));
        auto const serialized = serialize(innerTx);

        {
            Blob malformed{1, 2, 3};
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(malformed.data(), malformed.size()),
                calcAccountID(exporter.first),
                21337,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        }

        auto expectSigningEnvelopeRejected = [&](STTx tx) {
            auto const blob = serialize(tx);
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(blob.data(), blob.size()),
                calcAccountID(exporter.first),
                21337,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        };

        {
            auto signedInner = innerTx;
            signedInner.setFieldVL(sfSigningPubKey, exporter.first.slice());
            expectSigningEnvelopeRejected(std::move(signedInner));
        }

        {
            auto signedInner = innerTx;
            signedInner.setFieldVL(sfTxnSignature, Blob{1, 2, 3});
            expectSigningEnvelopeRejected(std::move(signedInner));
        }

        {
            auto signedInner = innerTx;
            STArray signers(sfSigners);
            STObject signer(sfSigner);
            signer.setAccountID(sfAccount, calcAccountID(exporter.first));
            signer.setFieldVL(sfSigningPubKey, exporter.first.slice());
            signer.setFieldVL(sfTxnSignature, Blob{1, 2, 3});
            signers.push_back(std::move(signer));
            signedInner.setFieldArray(sfSigners, signers);
            expectSigningEnvelopeRejected(std::move(signedInner));
        }

        {
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(serialized.data(), serialized.size()),
                calcAccountID(other.first),
                21337,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        }

        {
            auto const networkTx = makeExportedPayment(
                calcAccountID(exporter.first), calcAccountID(dst.first), 21337);
            auto const serializedNetwork = serialize(networkTx);
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(serializedNetwork.data(), serializedNetwork.size()),
                calcAccountID(exporter.first),
                21337,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        }

        {
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(serialized.data(), serialized.size()),
                calcAccountID(exporter.first),
                0,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        }

        {
            auto const noTicketTx = makeExportedPayment(
                calcAccountID(exporter.first),
                calcAccountID(dst.first),
                std::nullopt,
                std::nullopt);
            auto const serializedNoTicket = serialize(noTicketTx);
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(serializedNoTicket.data(), serializedNoTicket.size()),
                calcAccountID(exporter.first),
                21337,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        }

        {
            auto const sequencedTicketTx = makeExportedPayment(
                calcAccountID(exporter.first),
                calcAccountID(dst.first),
                std::nullopt,
                1,
                9);
            auto const serializedSequencedTicket = serialize(sequencedTicketTx);
            bool nonceCalled = false;
            auto const result = hook::XportWrapperBuilder::build(makeInput(
                Slice(
                    serializedSequencedTicket.data(),
                    serializedSequencedTicket.size()),
                calcAccountID(exporter.first),
                21337,
                [&nonceCalled] {
                    nonceCalled = true;
                    return Expected<uint256, ::hook_api::hook_return_code>{
                        makeHash("nonce")};
                }));
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
            BEAST_EXPECT(!nonceCalled);
        }

        {
            auto const reservedType = Blob(
                ExportOriginMemo::memoType.begin(),
                ExportOriginMemo::memoType.end());
            auto const reservedMemoTx = withMemo(
                innerTx, reservedType, Blob(ExportOriginMemo::identityBytes));
            expectSigningEnvelopeRejected(reservedMemoTx);
        }

        {
            auto const fullMemoTx =
                withMemo(innerTx, Blob{'u', 's', 'e', 'r'}, Blob(930, 0x5A));
            std::string reason;
            BEAST_EXPECT(passesLocalChecks(fullMemoTx, reason));
            expectSigningEnvelopeRejected(fullMemoTx);
        }
    }

    void
    testRejectsMissingCallbacks()
    {
        testcase("rejects missing callbacks");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(exporter.first), calcAccountID(dst.first));
        auto const serialized = serialize(innerTx);

        {
            auto input = makeInput(
                Slice(serialized.data(), serialized.size()),
                calcAccountID(exporter.first));
            input.generateNonce = {};

            auto const result = hook::XportWrapperBuilder::build(input);
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::INTERNAL_ERROR);
        }

        {
            auto input = makeInput(
                Slice(serialized.data(), serialized.size()),
                calcAccountID(exporter.first));
            input.calculateFee = {};

            auto const result = hook::XportWrapperBuilder::build(input);
            BEAST_EXPECT(!result);
            BEAST_EXPECT(
                result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
        }
    }

    void
    testMapsNonceFailureToInternalError()
    {
        testcase("maps nonce failure to internal error");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(exporter.first), calcAccountID(dst.first));
        auto const serialized = serialize(innerTx);

        auto const result = hook::XportWrapperBuilder::build(makeInput(
            Slice(serialized.data(), serialized.size()),
            calcAccountID(exporter.first),
            21337,
            [] {
                return Expected<uint256, ::hook_api::hook_return_code>{
                    Unexpected(::hook_api::hook_return_code::TOO_MANY_NONCES)};
            }));

        BEAST_EXPECT(!result);
        BEAST_EXPECT(
            result.error() == ::hook_api::hook_return_code::INTERNAL_ERROR);
    }

    void
    testRejectsFeeFailure()
    {
        testcase("rejects fee failure");

        auto const exporter = randomKeyPair(KeyType::secp256k1);
        auto const dst = randomKeyPair(KeyType::secp256k1);
        auto const innerTx = makeExportedPayment(
            calcAccountID(exporter.first), calcAccountID(dst.first));
        auto const serialized = serialize(innerTx);

        auto const result = hook::XportWrapperBuilder::build(makeInput(
            Slice(serialized.data(), serialized.size()),
            calcAccountID(exporter.first),
            21337,
            [] {
                return Expected<uint256, ::hook_api::hook_return_code>{
                    makeHash("nonce")};
            },
            [](Slice const&) {
                return Expected<std::uint64_t, ::hook_api::hook_return_code>{
                    Unexpected(::hook_api::hook_return_code::EXPORT_FAILURE)};
            }));

        BEAST_EXPECT(!result);
        BEAST_EXPECT(
            result.error() == ::hook_api::hook_return_code::EXPORT_FAILURE);
    }

    void
    run() override
    {
        testBuildsWrapper();
        testBuildsWrapperWithoutCallback();
        testPreservesUserMemos();
        testRejectsInvalidInputs();
        testRejectsMissingCallbacks();
        testMapsNonceFailureToInternalError();
        testRejectsFeeFailure();
    }
};

BEAST_DEFINE_TESTSUITE(XportWrapperBuilder, app, ripple);

}  // namespace test
}  // namespace ripple
