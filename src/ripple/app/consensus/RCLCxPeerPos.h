//------------------------------------------------------------------------------
/*
    This file is part of rippled: https://github.com/ripple/rippled
    Copyright (c) 2012, 2013 Ripple Labs Inc.

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

#ifndef RIPPLE_APP_CONSENSUS_RCLCXPEERPOS_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_RCLCXPEERPOS_H_INCLUDED

#include <ripple/basics/CountedObject.h>
#include <ripple/basics/base_uint.h>
#include <ripple/beast/hash/hash_append.h>
#include <ripple/consensus/ConsensusProposal.h>
#include <ripple/json/json_value.h>
#include <ripple/protocol/HashPrefix.h>
#include <ripple/protocol/PublicKey.h>
#include <ripple/protocol/SecretKey.h>
#include <ripple/protocol/Serializer.h>
#include <boost/container/static_vector.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

namespace ripple {

/** Extended position for consensus with RNG entropy support.

    Carries both the consensus targets (set hashes that require agreement)
    and pipelined leaves (per-validator data transported via gossip).

    Critical design:
    - operator== excludes leaves (allows convergence with unique leaves)
    - add() includes ALL fields (prevents signature stripping attacks)
*/
struct ExtendedPosition
{
    // === Consensus Targets (Agreement Required) ===
    uint256 txSetHash;
    std::optional<uint256> commitSetHash;
    std::optional<uint256> entropySetHash;

    // === Pipelined Leaves (No Agreement Required) ===
    std::optional<uint256> myCommitment;
    std::optional<uint256> myReveal;

    ExtendedPosition() = default;
    explicit ExtendedPosition(uint256 const& txSet) : txSetHash(txSet)
    {
    }

    // Implicit conversion for legacy compatibility
    operator uint256() const
    {
        return txSetHash;
    }

    // Helper to update TxSet while preserving sidecar data
    void
    updateTxSet(uint256 const& set)
    {
        txSetHash = set;
    }

    // CRITICAL: Exclude leaves from equality - consensus only on set hashes
    bool
    operator==(ExtendedPosition const& other) const
    {
        return txSetHash == other.txSetHash &&
            commitSetHash == other.commitSetHash &&
            entropySetHash == other.entropySetHash;
    }

    bool
    operator!=(ExtendedPosition const& other) const
    {
        return !(*this == other);
    }

    // Comparison with uint256 (compares txSetHash only)
    bool
    operator==(uint256 const& hash) const
    {
        return txSetHash == hash;
    }

    bool
    operator!=(uint256 const& hash) const
    {
        return txSetHash != hash;
    }

    friend bool
    operator==(uint256 const& hash, ExtendedPosition const& pos)
    {
        return pos.txSetHash == hash;
    }

    friend bool
    operator!=(uint256 const& hash, ExtendedPosition const& pos)
    {
        return pos.txSetHash != hash;
    }

    // CRITICAL: Include ALL fields for signing (prevents stripping attacks)
    void
    add(Serializer& s) const
    {
        s.addBitString(txSetHash);

        std::uint8_t flags = 0;
        if (commitSetHash)
            flags |= 0x01;
        if (entropySetHash)
            flags |= 0x02;
        if (myCommitment)
            flags |= 0x04;
        if (myReveal)
            flags |= 0x08;
        s.add8(flags);

        if (commitSetHash)
            s.addBitString(*commitSetHash);
        if (entropySetHash)
            s.addBitString(*entropySetHash);
        if (myCommitment)
            s.addBitString(*myCommitment);
        if (myReveal)
            s.addBitString(*myReveal);
    }

    Json::Value
    getJson() const
    {
        Json::Value ret = Json::objectValue;
        ret["tx_set"] = to_string(txSetHash);
        if (commitSetHash)
            ret["commit_set"] = to_string(*commitSetHash);
        if (entropySetHash)
            ret["entropy_set"] = to_string(*entropySetHash);
        return ret;
    }
};

// For logging/debugging - returns txSetHash as string
inline std::string
to_string(ExtendedPosition const& pos)
{
    return to_string(pos.txSetHash);
}

// Stream output for logging
inline std::ostream&
operator<<(std::ostream& os, ExtendedPosition const& pos)
{
    return os << pos.txSetHash;
}

// For hash_append (used in sha512Half and similar)
template <class Hasher>
void
hash_append(Hasher& h, ExtendedPosition const& pos)
{
    using beast::hash_append;
    // Serialize full position including all fields
    Serializer s;
    pos.add(s);
    hash_append(h, s.slice());
}

/** A peer's signed, proposed position for use in RCLConsensus.

    Carries a ConsensusProposal signed by a peer. Provides value semantics
    but manages shared storage of the peer position internally.
*/
class RCLCxPeerPos
{
public:
    //< The type of the proposed position (uses ExtendedPosition for RNG
    //support)
    using Proposal = ConsensusProposal<NodeID, uint256, ExtendedPosition>;

    /** Constructor

        Constructs a signed peer position.

        @param publicKey Public key of the peer
        @param signature Signature provided with the proposal
        @param suppress Unique id used for hash router suppression
        @param proposal The consensus proposal
    */

    RCLCxPeerPos(
        PublicKey const& publicKey,
        Slice const& signature,
        uint256 const& suppress,
        Proposal&& proposal);

    //! Verify the signing hash of the proposal
    bool
    checkSign() const;

    //! Signature of the proposal (not necessarily verified)
    Slice
    signature() const
    {
        return {signature_.data(), signature_.size()};
    }

    //! Public key of peer that sent the proposal
    PublicKey const&
    publicKey() const
    {
        return publicKey_;
    }

    //! Unique id used by hash router to suppress duplicates
    uint256 const&
    suppressionID() const
    {
        return suppression_;
    }

    Proposal const&
    proposal() const
    {
        return proposal_;
    }

    //! JSON representation of proposal
    Json::Value
    getJson() const;

private:
    PublicKey publicKey_;
    uint256 suppression_;
    Proposal proposal_;
    boost::container::static_vector<std::uint8_t, 72> signature_;

    template <class Hasher>
    void
    hash_append(Hasher& h) const
    {
        using beast::hash_append;
        hash_append(h, HashPrefix::proposal);
        hash_append(h, std::uint32_t(proposal().proposeSeq()));
        hash_append(h, proposal().closeTime());
        hash_append(h, proposal().prevLedger());
        // Serialize full ExtendedPosition for hashing
        Serializer s;
        proposal().position().add(s);
        hash_append(h, s.slice());
    }
};

/** Calculate a unique identifier for a signed proposal.

    The identifier is based on all the fields that contribute to the signature,
    as well as the signature itself. The "last closed ledger" field may be
    omitted, but the signer will compute the signature as if this field was
    present. Recipients of the proposal will inject the last closed ledger in
    order to validate the signature. If the last closed ledger is left out, then
    it is considered as all zeroes for the purposes of signing.

    @param position The extended position (includes entropy fields)
    @param previousLedger The hash of the ledger the proposal is based upon
    @param proposeSeq Sequence number of the proposal
    @param closeTime Close time of the proposal
    @param publicKey Signer's public key
    @param signature Proposal signature
*/
uint256
proposalUniqueId(
    ExtendedPosition const& position,
    uint256 const& previousLedger,
    std::uint32_t proposeSeq,
    NetClock::time_point closeTime,
    Slice const& publicKey,
    Slice const& signature);

}  // namespace ripple

#endif
