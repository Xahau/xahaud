//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2026 XRPL-Labs

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

#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/HistoryTree.h>
#include <xrpl/protocol/Serializer.h>
#include <xrpl/protocol/digest.h>

#include <bit>

namespace ripple {
namespace history {

uint256
leafHash(std::uint32_t ledgerSeq, std::uint32_t txIndex, Slice tx, Slice meta)
{
    Serializer s(tx.size() + meta.size() + 16);
    s.add32(ledgerSeq);
    s.add32(txIndex);
    s.addVL(tx);
    s.addVL(meta);
    return sha512Half(HashPrefix::historyLeaf, s.slice());
}

uint256
nodeHash(uint256 const& left, uint256 const& right)
{
    return sha512Half(HashPrefix::historyNode, left, right);
}

void
append(std::vector<uint256>& peaks, std::uint64_t& count, uint256 const& leaf)
{
    // The peaks mirror the set bits of count. Adding a leaf is a binary
    // increment: every trailing one-bit is a peak that merges with the carry.
    uint256 carry = leaf;
    std::uint64_t n = count;
    while ((n & 1) && !peaks.empty())
    {
        carry = nodeHash(peaks.back(), carry);
        peaks.pop_back();
        n >>= 1;
    }
    peaks.push_back(carry);
    ++count;
}

bool
peaksValid(std::vector<uint256> const& peaks, std::uint64_t count)
{
    return peaks.size() == static_cast<std::size_t>(std::popcount(count));
}

std::optional<Position>
locate(std::uint64_t count, std::uint64_t leafIndex)
{
    if (leafIndex >= count)
        return std::nullopt;

    std::uint64_t start = 0;
    std::size_t peak = 0;
    for (int h = 63; h >= 0; --h)
    {
        if (!(count & (std::uint64_t{1} << h)))
            continue;

        std::uint64_t const size = std::uint64_t{1} << h;
        if (leafIndex < start + size)
            return Position{
                peak, static_cast<unsigned>(h), leafIndex - start};

        start += size;
        ++peak;
    }

    return std::nullopt;  // unreachable: leafIndex < count
}

std::optional<Proof>
parseProof(Slice blob)
{
    try
    {
        SerialIter sit(blob);
        if (sit.get8() != proofVersion)
            return std::nullopt;

        Proof p;
        p.ledgerSeq = sit.get32();
        p.txIndex = sit.get32();
        p.tx = sit.getSlice(sit.getVLDataLength());
        p.meta = sit.getSlice(sit.getVLDataLength());

        std::size_t const pathLen = sit.get8();
        if (pathLen > maxProofPath)
            return std::nullopt;

        p.path.reserve(pathLen);
        for (std::size_t i = 0; i < pathLen; ++i)
            p.path.push_back(sit.getBitString<256>());

        if (sit.getBytesLeft() != 0)
            return std::nullopt;

        if (p.tx.empty() || p.meta.empty())
            return std::nullopt;

        return p;
    }
    catch (std::exception const&)
    {
        return std::nullopt;
    }
}

bool
verifyProof(
    std::vector<uint256> const& peaks,
    std::uint64_t count,
    std::uint64_t leafIndex,
    Proof const& proof,
    uint256* leaf)
{
    if (!peaksValid(peaks, count))
        return false;

    auto const pos = locate(count, leafIndex);
    if (!pos || pos->peak >= peaks.size() || proof.path.size() != pos->height)
        return false;

    uint256 cur =
        leafHash(proof.ledgerSeq, proof.txIndex, proof.tx, proof.meta);
    if (leaf)
        *leaf = cur;

    for (unsigned level = 0; level < pos->height; ++level)
    {
        auto const& sibling = proof.path[level];
        if ((pos->offset >> level) & 1)
            cur = nodeHash(sibling, cur);
        else
            cur = nodeHash(cur, sibling);
    }

    return cur == peaks[pos->peak];
}

}  // namespace history
}  // namespace ripple
