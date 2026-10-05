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
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/protocol/Indexes.h>
#include <xrpl/protocol/Protocol.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/STLedgerEntry.h>

#include <limits>
#include <optional>

namespace ripple {

std::vector<std::pair<SHAMapNodeID, uint256>>
missingManifestNodes(SHAMap const& stateMap, std::size_t max)
{
    std::vector<std::pair<SHAMapNodeID, uint256>> missing;

    // By hash: several keys blocked by the same node report it once.
    hash_set<uint256> reported;
    hash_set<uint256> visited;

    auto const& root = keylet::manifestDir();
    std::vector<uint256> pending{root.key};

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
                    sle->getFieldU64(sfIndexPrevious), dirNodeMaxPages - 1);
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

}  // namespace ripple
