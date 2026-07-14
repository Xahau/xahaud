//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled

    Permission to use, copy, modify, and/or distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
    ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
    IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/
//==============================================================================

#include <xrpl/basics/StringUtilities.h>
#include <xrpl/basics/strHex.h>
#include <xrpl/beast/unit_test.h>
#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/STAmount.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/TxFlags.h>

namespace ripple::test {
namespace {

STTx
makePayment(Blob userMemo = {})
{
    STObject object{sfGeneric};
    object.setFieldU16(sfTransactionType, ttPAYMENT);
    object.setFieldU32(sfFlags, tfFullyCanonicalSig);
    object.setFieldU32(sfSequence, 0);
    object.setFieldU32(sfTicketSequence, 1);
    object.setFieldAmount(sfAmount, XRPAmount{1});
    object.setFieldAmount(sfFee, XRPAmount{10});
    object.setFieldVL(sfSigningPubKey, Blob{});
    object.setAccountID(sfAccount, AccountID{1});
    object.setAccountID(sfDestination, AccountID{2});

    if (!userMemo.empty())
    {
        STArray memos{sfMemos};
        STObject memo{sfMemo};
        memo.setFieldVL(sfMemoType, Blob{'u', 's', 'e', 'r'});
        memo.setFieldVL(sfMemoData, userMemo);
        memos.emplace_back(std::move(memo));
        object.setFieldArray(sfMemos, memos);
    }
    return STTx{std::move(object)};
}

STTx
replaceReservedData(STTx const& tx, Blob const& data)
{
    Serializer serializer;
    tx.add(serializer);
    SerialIter sit{serializer.slice()};
    STObject object{sfGeneric};
    object.set(sit);
    object.peekFieldArray(sfMemos).back().setFieldVL(sfMemoData, data);
    return STTx{std::move(object)};
}

Blob
reservedData(STTx const& tx)
{
    return tx.getFieldArray(sfMemos).back().getFieldVL(sfMemoData);
}

STObject
cloneObject(STTx const& tx)
{
    Serializer serializer;
    tx.add(serializer);
    SerialIter sit{serializer.slice()};
    STObject object{sfGeneric};
    object.set(sit);
    return object;
}

STTx
reparse(STTx const& tx)
{
    return STTx{cloneObject(tx)};
}

std::size_t
memoBytes(STArray const& memos)
{
    Serializer serializer;
    memos.add(serializer);
    return serializer.getDataLength();
}

Blob
serializeMemos(STArray const& memos)
{
    Serializer serializer;
    memos.add(serializer);
    return serializer.getData();
}

std::size_t
memoBytes(STTx const& tx)
{
    return memoBytes(tx.getFieldArray(sfMemos));
}

}  // namespace

class ExportOriginMemo_test : public beast::unit_test::suite
{
    ExportOriginMemo::Origin const origin_{21337, 0, uint256{2}};
    ExportOriginMemo::Anchor const anchor_{4123200, uint256{3}};

public:
    void
    testExactEncodingAndProjection()
    {
        testcase("exact encoding and projection");

        auto const identity =
            ExportOriginMemo::identityForm(makePayment(), origin_);
        auto const release =
            ExportOriginMemo::releaseForm(makePayment(), origin_, anchor_);
        BEAST_EXPECT(identity.has_value());
        BEAST_EXPECT(release.has_value());
        if (!identity || !release)
            return;

        auto const expectedIdentity = strUnHex(
            "0100000053590000000000000000000000000000000000000000000000000000"
            "00000000000000000002");
        auto const expectedRelease = strUnHex(
            "0100000053590000000000000000000000000000000000000000000000000000"
            "00000000000000000002003EEA40000000000000000000000000000000000000"
            "0000000000000000000000000003");
        BEAST_EXPECT(expectedIdentity);
        BEAST_EXPECT(expectedRelease);
        if (expectedIdentity && expectedRelease)
        {
            BEAST_EXPECT(reservedData(identity.value()) == *expectedIdentity);
            BEAST_EXPECTS(
                reservedData(release.value()) == *expectedRelease,
                strHex(reservedData(release.value())) +
                    " != " + strHex(*expectedRelease));
        }

        auto const parsedIdentity = ExportOriginMemo::parse(identity.value());
        auto const parsedRelease = ExportOriginMemo::parse(release.value());
        BEAST_EXPECT(parsedIdentity.has_value());
        BEAST_EXPECT(parsedRelease.has_value());
        if (parsedIdentity && parsedRelease)
        {
            BEAST_EXPECT(parsedIdentity.value().origin == origin_);
            BEAST_EXPECT(!parsedIdentity.value().anchor);
            BEAST_EXPECT(parsedRelease.value().origin == origin_);
            BEAST_EXPECT(parsedRelease.value().anchor == anchor_);
        }

        auto const projected =
            ExportOriginMemo::projectIdentity(release.value());
        BEAST_EXPECT(projected.has_value());
        if (projected)
        {
            BEAST_EXPECT(
                projected.value().getSerializer().peekData() ==
                identity.value().getSerializer().peekData());
            BEAST_EXPECT(
                projected.value().getTransactionID() ==
                reparse(projected.value()).getTransactionID());

            auto const projectedAgain =
                ExportOriginMemo::projectIdentity(projected.value());
            BEAST_EXPECT(projectedAgain.has_value());
            if (projectedAgain)
                BEAST_EXPECT(
                    projectedAgain.value().getSerializer().peekData() ==
                    projected.value().getSerializer().peekData());
        }
    }

    void
    testPreservesUserMemos()
    {
        testcase("preserves user Memos");

        Blob const userData{0xde, 0xad, 0xbe, 0xef};
        auto const base = makePayment(userData);
        auto const baseBytes = base.getSerializer().peekData();
        auto const release =
            ExportOriginMemo::releaseForm(base, origin_, anchor_);
        BEAST_EXPECT(release.has_value());
        BEAST_EXPECT(base.getSerializer().peekData() == baseBytes);
        if (!release)
            return;

        auto const& memos = release.value().getFieldArray(sfMemos);
        BEAST_EXPECT(memos.size() == 2);
        BEAST_EXPECT(
            memos[0].getFieldVL(sfMemoType) == Blob({'u', 's', 'e', 'r'}));
        BEAST_EXPECT(memos[0].getFieldVL(sfMemoData) == userData);
        BEAST_EXPECT(
            memos[1].getFieldVL(sfMemoType) ==
            Blob(
                ExportOriginMemo::memoType.begin(),
                ExportOriginMemo::memoType.end()));

        auto object = cloneObject(makePayment());
        STArray heterogeneous{sfMemos};
        STObject first{sfMemo};
        first.setFieldVL(sfMemoType, Blob{'a'});
        first.setFieldVL(sfMemoData, Blob{1});
        first.setFieldVL(sfMemoFormat, Blob{'t', 'e', 'x', 't'});
        heterogeneous.emplace_back(std::move(first));
        STObject second{sfMemo};
        second.setFieldVL(sfMemoData, Blob{2, 3});
        heterogeneous.emplace_back(std::move(second));
        object.setFieldArray(sfMemos, heterogeneous);
        auto const heterogeneousBase = STTx{std::move(object)};
        auto const heterogeneousBytes =
            serializeMemos(heterogeneousBase.getFieldArray(sfMemos));
        auto const heterogeneousRelease =
            ExportOriginMemo::releaseForm(heterogeneousBase, origin_, anchor_);
        BEAST_EXPECT(heterogeneousRelease.has_value());
        if (heterogeneousRelease)
        {
            auto preserved =
                heterogeneousRelease.value().getFieldArray(sfMemos);
            preserved.erase(std::prev(preserved.end()));
            BEAST_EXPECT(serializeMemos(preserved) == heterogeneousBytes);
        }
    }

    void
    testRejectsNonCanonicalReservedMemo()
    {
        testcase("rejects non-canonical reserved Memo");

        auto const identity =
            ExportOriginMemo::identityForm(makePayment(), origin_);
        BEAST_EXPECT(identity.has_value());
        if (!identity)
            return;

        auto const duplicate =
            ExportOriginMemo::identityForm(identity.value(), origin_);
        BEAST_EXPECT(!duplicate);
        if (!duplicate)
            BEAST_EXPECT(
                duplicate.error() ==
                ExportOriginMemo::Error::reservedMemoPresent);

        {
            auto object = cloneObject(identity.value());
            auto memos = object.getFieldArray(sfMemos);
            memos.emplace_back(memos.back());
            object.setFieldArray(sfMemos, memos);
            auto const parsedDuplicate =
                ExportOriginMemo::parse(STTx{std::move(object)});
            BEAST_EXPECT(
                !parsedDuplicate &&
                parsedDuplicate.error() ==
                    ExportOriginMemo::Error::reservedMemoPosition);
        }

        {
            auto const withUser =
                ExportOriginMemo::identityForm(makePayment(Blob{1}), origin_);
            BEAST_EXPECT(withUser.has_value());
            if (withUser)
            {
                auto object = cloneObject(withUser.value());
                auto memos = object.getFieldArray(sfMemos);
                std::swap(memos[0], memos.back());
                object.setFieldArray(sfMemos, memos);
                auto const parsedMisplaced =
                    ExportOriginMemo::parse(STTx{std::move(object)});
                BEAST_EXPECT(
                    !parsedMisplaced &&
                    parsedMisplaced.error() ==
                        ExportOriginMemo::Error::reservedMemoPosition);
            }
        }

        {
            auto object = cloneObject(identity.value());
            object.peekFieldArray(sfMemos).back().setFieldVL(
                sfMemoFormat, Blob{'b', 'i', 'n'});
            auto const parsedFormat =
                ExportOriginMemo::parse(STTx{std::move(object)});
            BEAST_EXPECT(
                !parsedFormat &&
                parsedFormat.error() == ExportOriginMemo::Error::malformedMemo);
        }

        auto malformed = reservedData(identity.value());
        malformed[0] = 2;
        auto parsed = ExportOriginMemo::parse(
            replaceReservedData(identity.value(), malformed));
        BEAST_EXPECT(
            !parsed &&
            parsed.error() == ExportOriginMemo::Error::unsupportedVersion);

        malformed[0] = ExportOriginMemo::version;
        malformed[1] = 1;
        parsed = ExportOriginMemo::parse(
            replaceReservedData(identity.value(), malformed));
        BEAST_EXPECT(
            !parsed &&
            parsed.error() == ExportOriginMemo::Error::unsupportedFlags);

        malformed.pop_back();
        parsed = ExportOriginMemo::parse(
            replaceReservedData(identity.value(), malformed));
        BEAST_EXPECT(
            !parsed &&
            parsed.error() == ExportOriginMemo::Error::malformedMemo);
    }

    void
    testProjectedMemoSize()
    {
        testcase("projected Memo size");

        auto const base = makePayment(Blob(930, 0xa5));
        std::string reason;
        BEAST_EXPECT(passesLocalChecks(base, reason));

        auto const identity = ExportOriginMemo::identityForm(base, origin_);
        auto const release =
            ExportOriginMemo::releaseForm(base, origin_, anchor_);
        BEAST_EXPECT(identity.has_value());
        BEAST_EXPECT(
            !release &&
            release.error() == ExportOriginMemo::Error::localChecks);

        auto testExactBoundary = [&](auto project) {
            std::optional<std::size_t> lastAcceptedBytes;
            std::size_t firstRejected = 0;
            for (std::size_t size = 1; size <= 1024; ++size)
            {
                auto candidate = project(makePayment(Blob(size, 0xa5)));
                if (candidate)
                {
                    lastAcceptedBytes = memoBytes(candidate.value());
                    continue;
                }
                firstRejected = size;
                BEAST_EXPECT(
                    candidate.error() == ExportOriginMemo::Error::localChecks);
                break;
            }

            BEAST_EXPECT(lastAcceptedBytes.has_value());
            BEAST_EXPECT(firstRejected != 0);
            if (lastAcceptedBytes)
                BEAST_EXPECT(*lastAcceptedBytes == 1024);
        };

        testExactBoundary([&](STTx const& tx) {
            return ExportOriginMemo::identityForm(tx, origin_);
        });
        testExactBoundary([&](STTx const& tx) {
            return ExportOriginMemo::releaseForm(tx, origin_, anchor_);
        });
    }

    void
    run() override
    {
        testExactEncodingAndProjection();
        testPreservesUserMemos();
        testRejectsNonCanonicalReservedMemo();
        testProjectedMemoSize();
    }
};

BEAST_DEFINE_TESTSUITE(ExportOriginMemo, protocol, ripple);

}  // namespace ripple::test
