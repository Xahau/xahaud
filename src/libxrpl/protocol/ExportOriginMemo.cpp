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

#include <xrpl/protocol/ExportOriginMemo.h>
#include <xrpl/protocol/STArray.h>
#include <xrpl/protocol/Serializer.h>

#include <algorithm>
#include <exception>
#include <iterator>
#include <string>

namespace ripple::ExportOriginMemo {
namespace {

Blob const&
memoTypeBytes()
{
    static Blob const value(memoType.begin(), memoType.end());
    return value;
}

bool
isReserved(STObject const& memo)
{
    return memo.getFName() == sfMemo && memo.isFieldPresent(sfMemoType) &&
        memo.getFieldVL(sfMemoType) == memoTypeBytes();
}

STObject
cloneObject(STTx const& tx)
{
    Serializer serializer;
    tx.add(serializer);
    SerialIter sit{serializer.slice()};
    STObject result{sfGeneric};
    result.set(sit);
    return result;
}

Blob
serialize(Origin const& origin, std::optional<Anchor> const& anchor)
{
    //@@start export-origin-memo-wire-format
    Serializer serializer;
    serializer.add8(version);
    serializer.add8(allowedFlags);
    serializer.add32(origin.sourceDomain);
    serializer.add32(origin.targetDomain);
    serializer.addBitString(origin.transactionHash);
    if (anchor)
    {
        serializer.add32(anchor->ledgerSequence);
        serializer.addBitString(anchor->ledgerHash);
    }
    return serializer.getData();
    //@@end export-origin-memo-wire-format
}

Expected<STTx, Error>
append(
    STTx const& base,
    Origin const& origin,
    std::optional<Anchor> const& anchor)
{
    if (hasReservedMemo(base))
        return Unexpected(Error::reservedMemoPresent);

    //@@start export-origin-memo-canonical-append
    auto object = cloneObject(base);
    STArray memos = object.isFieldPresent(sfMemos)
        ? object.getFieldArray(sfMemos)
        : STArray{sfMemos};

    STObject memo{sfMemo};
    memo.setFieldVL(sfMemoType, memoTypeBytes());
    memo.setFieldVL(sfMemoData, serialize(origin, anchor));
    memos.emplace_back(std::move(memo));
    object.setFieldArray(sfMemos, memos);

    STTx result{std::move(object)};
    std::string reason;
    if (!passesLocalChecks(result, reason))
        return Unexpected(Error::localChecks);
    return result;
    //@@end export-origin-memo-canonical-append
}

Expected<Stamp, Error>
parseData(Blob const& data)
{
    if (data.size() != identityBytes && data.size() != releaseBytes)
        return Unexpected(Error::malformedMemo);

    try
    {
        SerialIter sit{makeSlice(data)};
        if (sit.get8() != version)
            return Unexpected(Error::unsupportedVersion);
        if (sit.get8() != allowedFlags)
            return Unexpected(Error::unsupportedFlags);

        Stamp stamp{
            .origin =
                Origin{
                    .sourceDomain = sit.get32(),
                    .targetDomain = sit.get32(),
                    .transactionHash = sit.get256()},
            .anchor = std::nullopt};

        if (data.size() == releaseBytes)
            stamp.anchor = Anchor{sit.get32(), sit.get256()};
        if (!sit.empty())
            return Unexpected(Error::malformedMemo);
        return stamp;
    }
    catch (std::exception const&)
    {
        return Unexpected(Error::malformedMemo);
    }
}

}  // namespace

bool
hasReservedMemo(STTx const& tx)
{
    if (!tx.isFieldPresent(sfMemos))
        return false;

    auto const& memos = tx.getFieldArray(sfMemos);
    return std::any_of(memos.begin(), memos.end(), isReserved);
}

Expected<STTx, Error>
identityForm(STTx const& base, Origin const& origin)
{
    return append(base, origin, std::nullopt);
}

Expected<STTx, Error>
releaseForm(STTx const& base, Origin const& origin, Anchor const& anchor)
{
    return append(base, origin, anchor);
}

Expected<Stamp, Error>
parse(STTx const& tx)
{
    std::string reason;
    if (!passesLocalChecks(tx, reason))
        return Unexpected(Error::localChecks);
    if (!tx.isFieldPresent(sfMemos))
        return Unexpected(Error::reservedMemoMissing);

    auto const& memos = tx.getFieldArray(sfMemos);
    auto found = memos.end();
    for (auto it = memos.begin(); it != memos.end(); ++it)
    {
        if (!isReserved(*it))
            continue;
        if (found != memos.end())
            return Unexpected(Error::reservedMemoPosition);
        found = it;
    }

    if (found == memos.end())
        return Unexpected(Error::reservedMemoMissing);
    if (found != std::prev(memos.end()))
        return Unexpected(Error::reservedMemoPosition);
    if (found->isFieldPresent(sfMemoFormat) ||
        !found->isFieldPresent(sfMemoData))
        return Unexpected(Error::malformedMemo);

    return parseData(found->getFieldVL(sfMemoData));
}

Expected<STTx, Error>
projectIdentity(STTx const& stamped)
{
    auto parsed = parse(stamped);
    if (!parsed)
        return Unexpected(parsed.error());

    auto object = cloneObject(stamped);
    auto memos = object.getFieldArray(sfMemos);
    memos.erase(std::prev(memos.end()));
    if (memos.empty())
        object.delField(sfMemos);
    else
        object.setFieldArray(sfMemos, memos);

    return identityForm(STTx{std::move(object)}, parsed.value().origin);
}

}  // namespace ripple::ExportOriginMemo
