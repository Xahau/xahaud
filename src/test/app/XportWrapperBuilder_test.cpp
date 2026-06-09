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
#include <xrpl/protocol/STAmount.h>
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
    std::optional<std::uint32_t> networkID = std::nullopt)
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
    if (networkID)
        obj.setFieldU32(sfNetworkID, *networkID);
    return makeSTTx(obj);
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
        BEAST_EXPECT(result->innerTxHash == innerTx.getTransactionID());
        BEAST_EXPECT(wrapper.getTxnType() == ttEXPORT);
        BEAST_EXPECT(
            wrapper.getAccountID(sfAccount) == calcAccountID(exporter.first));
        BEAST_EXPECT(wrapper.getFieldU32(sfSequence) == 0);
        BEAST_EXPECT(wrapper.getFieldU32(sfFirstLedgerSequence) == 11);
        BEAST_EXPECT(wrapper.getFieldU32(sfLastLedgerSequence) == 15);
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
        testRejectsInvalidInputs();
        testMapsNonceFailureToInternalError();
        testRejectsFeeFailure();
    }
};

BEAST_DEFINE_TESTSUITE(XportWrapperBuilder, app, ripple);

}  // namespace test
}  // namespace ripple
