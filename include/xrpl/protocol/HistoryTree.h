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

#ifndef RIPPLE_PROTOCOL_HISTORYTREE_H_INCLUDED
#define RIPPLE_PROTOCOL_HISTORYTREE_H_INCLUDED

#include <xrpl/basics/Slice.h>
#include <xrpl/basics/base_uint.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace ripple {
namespace history {

/** featureHistoryTree: an append-only Merkle Mountain Range over every
    transaction ever applied, in ledger order then TransactionIndex order.

    Validators keep only the peaks (one hash per set bit of the leaf count)
    in the ltHISTORY_TREE singleton; nobody needs a leaf to append or verify.

    Leaf i for a transaction applied at TransactionIndex t of ledger s is
        sha512Half(HashPrefix::historyLeaf, u32 s, u32 t, VL tx, VL meta)
    where tx is the stored (storable, see STTx::addStorable) serialization and
    meta is the stored metadata. Interior nodes are
        sha512Half(HashPrefix::historyNode, left, right).

    Peaks are kept tallest-first, i.e. in the order of the set bits of the
    leaf count from most to least significant, so that peak p covers the
    p-th set bit.
*/

/// Hash of one leaf.
uint256
leafHash(std::uint32_t ledgerSeq, std::uint32_t txIndex, Slice tx, Slice meta);

/// Hash of one interior node.
uint256
nodeHash(uint256 const& left, uint256 const& right);

/// Append a leaf, merging peaks as the binary carry of count demands.
void
append(std::vector<uint256>& peaks, std::uint64_t& count, uint256 const& leaf);

/// True if peaks could be the peaks of a tree with count leaves.
bool
peaksValid(std::vector<uint256> const& peaks, std::uint64_t count);

/** Which mountain holds a leaf.
    peak: index into the peaks vector; height: path length to that peak;
    offset: the leaf's position within the mountain (its path bits).
*/
struct Position
{
    std::size_t peak;
    unsigned height;
    std::uint64_t offset;
};

std::optional<Position>
locate(std::uint64_t count, std::uint64_t leafIndex);

/** The proof a host submits in sfHistoryProof.

    Wire format (big endian, VL = XRPL variable-length prefix):
        u8  version (currently 1)
        u32 ledger sequence
        u32 transaction index
        VL  transaction (storable serialization)
        VL  metadata
        u8  path length
        32 * path length bytes of sibling hashes, leaf-side first
*/
constexpr std::uint8_t proofVersion = 1;
constexpr std::size_t maxProofPath = 64;

struct Proof
{
    std::uint32_t ledgerSeq;
    std::uint32_t txIndex;
    Slice tx;
    Slice meta;
    std::vector<uint256> path;
};

std::optional<Proof>
parseProof(Slice blob);

/** Verify that proof places its leaf at leafIndex under the current peaks.
    On success the leaf's hash is written to leaf (if non-null).
*/
bool
verifyProof(
    std::vector<uint256> const& peaks,
    std::uint64_t count,
    std::uint64_t leafIndex,
    Proof const& proof,
    uint256* leaf = nullptr);

}  // namespace history
}  // namespace ripple

#endif
