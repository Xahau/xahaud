//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012-2017 Ripple Labs Inc

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
#ifndef RIPPLE_TEST_CSF_PROPOSAL_H_INCLUDED
#define RIPPLE_TEST_CSF_PROPOSAL_H_INCLUDED

#include <test/csf/Tx.h>
#include <test/csf/Validation.h>
#include <test/csf/ledgers.h>
#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/hash_append.h>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

namespace ripple {
namespace test {
namespace csf {
/** Position sidecar for CSF that can model RNG commit/reveal fields.

    Core tx-set convergence remains keyed on txSetHash only, matching
    production's ExtendedPosition behavior.
*/
struct RngPosition
{
    TxSet::ID txSetHash{};
    std::optional<uint256> commitSetHash;
    std::optional<uint256> entropySetHash;
    std::optional<uint256> myCommitment;
    std::optional<uint256> myReveal;

    RngPosition() = default;
    explicit RngPosition(TxSet::ID txSet) : txSetHash(txSet)
    {
    }

    void
    updateTxSet(TxSet::ID txSet)
    {
        txSetHash = txSet;
    }
};

inline std::string
to_string(RngPosition const& pos)
{
    return std::to_string(pos.txSetHash);
}

inline std::ostream&
operator<<(std::ostream& os, RngPosition const& pos)
{
    return os << pos.txSetHash;
}

template <class Hasher>
void
hash_append(Hasher& h, RngPosition const& pos)
{
    using beast::hash_append;
    auto appendOpt = [&](std::optional<uint256> const& o) {
        hash_append(h, static_cast<std::uint8_t>(o.has_value() ? 1 : 0));
        if (o)
            hash_append(h, *o);
    };

    hash_append(h, pos.txSetHash);
    appendOpt(pos.commitSetHash);
    appendOpt(pos.entropySetHash);
    appendOpt(pos.myCommitment);
    appendOpt(pos.myReveal);
}

/** Proposal is a position taken in the consensus process.
 */
using Proposal = ConsensusProposal<PeerID, Ledger::ID, RngPosition>;
using ProposalPosition = RngPosition;

}  // namespace csf
}  // namespace test
}  // namespace ripple

#endif
