//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/XRPLF/rippled
    Copyright 2026 Xahau

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

#ifndef RIPPLE_APP_CONSENSUS_ACTIVEVALIDATORVIEW_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_ACTIVEVALIDATORVIEW_H_INCLUDED

#include <xrpld/consensus/ConsensusTypes.h>
#include <xrpl/basics/UnorderedContainers.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/UintTypes.h>

#include <algorithm>
#include <optional>
#include <vector>

namespace ripple {

struct ActiveValidatorView
{
    hash_set<PublicKey> masterKeys;
    hash_set<NodeID> nodeIds;
    std::vector<PublicKey> orderedMasterKeys;
    std::optional<uint256> sourceLedgerHash;
    bool fromUNLReport = false;

    // Master-key count BEFORE the negativeUNL subtraction (see
    // buildActiveValidatorView). size() is the effective (post-nUNL) view;
    // originalViewSize is the original-UNL denominator that tier thresholds
    // anchored to the original view (the participant_aligned intersection
    // floor) must use, because nUNL can shrink the effective view while leaving
    // faulty nodes in it.
    std::size_t originalViewSize = 0;

    // Export paths receive validator keys; RNG sidecars identify validators by
    // NodeID. Keep both indexes in lockstep.
    void
    insertMaster(PublicKey const& masterKey)
    {
        auto const [_, inserted] = masterKeys.insert(masterKey);
        if (!inserted)
            return;

        nodeIds.insert(calcNodeID(masterKey));
        orderedMasterKeys.push_back(masterKey);
    }

    void
    eraseMaster(PublicKey const& masterKey)
    {
        if (masterKeys.erase(masterKey) == 0)
            return;

        nodeIds.erase(calcNodeID(masterKey));
        orderedMasterKeys.erase(
            std::remove(
                orderedMasterKeys.begin(), orderedMasterKeys.end(), masterKey),
            orderedMasterKeys.end());
    }

    std::size_t
    size() const
    {
        return masterKeys.size();
    }

    bool
    containsMaster(PublicKey const& masterKey) const
    {
        return masterKeys.count(masterKey) > 0;
    }

    bool
    containsNode(NodeID const& nodeId) const
    {
        return nodeIds.count(nodeId) > 0;
    }

    void
    canonicalizeOrder()
    {
        std::sort(orderedMasterKeys.begin(), orderedMasterKeys.end());
    }
};

struct ActiveValidatorViewSource
{
    std::optional<uint256> sourceLedgerHash;
    std::optional<hash_set<PublicKey>> unlReportMasterKeys;
    bool negativeUNLEnabled = false;
    hash_set<PublicKey> negativeUNL;
};

struct ActiveValidatorViewFallback
{
    hash_set<PublicKey> trustedMasterKeys;
    std::optional<PublicKey> localMasterKey;
};

ActiveValidatorView
buildActiveValidatorView(
    ActiveValidatorViewSource const& source,
    ActiveValidatorViewFallback const& fallback);

}  // namespace ripple

#endif
