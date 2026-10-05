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

#include <xrpld/app/ledger/ManifestSync.h>
#include <xrpld/app/misc/Manifest.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STLedgerEntry.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace ripple {

static_assert(
    manifestSyncPages * dirNodeMaxEntries >= ManifestCache::ledgerCapacity,
    "the early read must be able to fill the ledger tier");

std::vector<std::pair<SHAMapNodeID, uint256>>
missingManifestNodes(
    SHAMap const& stateMap,
    std::size_t max,
    std::vector<uint256> const& keys,
    std::uint64_t maxPages)
{
    std::vector<std::pair<SHAMapNodeID, uint256>> missing;

    // By hash: several keys blocked by the same node report it once.
    hash_set<uint256> reported;
    hash_set<uint256> visited;

    auto const& root = keylet::manifestDir();

    // A stack, so the keys asked for by name go last and are read first: they
    // are the ones that decide whether this node can trust a validation.
    std::vector<uint256> pending{root.key};
    pending.insert(pending.end(), keys.rbegin(), keys.rend());

    maxPages = std::clamp<std::uint64_t>(maxPages, 1, dirNodeMaxPages);

    while (!pending.empty() && missing.size() < max)
    {
        auto const key = pending.back();
        pending.pop_back();

        if (!visited.insert(key).second)
            continue;

        std::optional<std::pair<SHAMapNodeID, uint256>> gap;
        auto const item = stateMap.peekItemPartial(key, gap);

        if (gap)
        {
            if (reported.insert(gap->second).second)
                missing.push_back(*gap);
            continue;
        }

        if (!item)
            continue;

        std::optional<SLE const> sle;
        try
        {
            sle.emplace(SerialIter{item->slice()}, item->key());
        }
        catch (std::exception const&)
        {
            // Not this walk's to judge: the node verified against its hash,
            // so a full read of the ledger would meet the same object.
            continue;
        }

        if (sle->getType() == ltDIR_NODE)
        {
            for (auto const& k : sle->getFieldV256(sfIndexes))
                pending.push_back(k);

            // The root names the last page, every page in use has an index no
            // greater, and the directory only ever grows at its end. Asking
            // for them all now saves a round trip per page; an index not in
            // use costs at most the node that shows it absent.
            if (key == root.key)
            {
                auto const last = std::min<std::uint64_t>(
                    sle->getFieldU64(sfIndexPrevious), maxPages - 1);
                for (std::uint64_t i = 1; i <= last; ++i)
                    pending.push_back(keylet::page(root, i).key);
            }
        }
        else if (
            sle->getType() == ltMANIFEST &&
            sle->isFieldPresent(sfSigningPubKey) &&
            sle->getFieldU32(sfSequence) !=
                std::numeric_limits<std::uint32_t>::max())
        {
            auto const signing = sle->getFieldVL(sfSigningPubKey);
            if (publicKeyType(makeSlice(signing)))
                pending.push_back(
                    keylet::manifest(PublicKey{makeSlice(signing)}).key);
        }
    }

    return missing;
}

std::vector<uint256>
pickManifestCandidates(
    std::vector<uint256> const& reported,
    hash_set<uint256> const& skip,
    std::size_t max)
{
    hash_map<uint256, std::size_t> counts;
    for (auto const& hash : reported)
        if (hash.isNonZero() && !skip.contains(hash))
            ++counts[hash];

    std::vector<std::pair<std::size_t, uint256>> ranked;
    ranked.reserve(counts.size());
    for (auto const& [hash, count] : counts)
        ranked.emplace_back(count, hash);

    auto const keep = std::min(max, ranked.size());
    std::partial_sort(
        ranked.begin(),
        ranked.begin() + keep,
        ranked.end(),
        [](auto const& a, auto const& b) { return a > b; });

    std::vector<uint256> picked;
    picked.reserve(keep);
    for (std::size_t i = 0; i < keep; ++i)
        picked.push_back(ranked[i].second);
    return picked;
}

std::size_t
manifestQuorum(std::size_t validators, bool trusted, std::size_t quorum)
{
    if (trusted)
        return std::min(quorum, validators);

    // ceil(0.8 * validators), in integers.
    return (validators * 4 + 4) / 5;
}

}  // namespace ripple
