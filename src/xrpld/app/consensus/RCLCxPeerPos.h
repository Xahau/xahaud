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

#include <xrpld/consensus/ConsensusProposal.h>
#include <xrpl/basics/CountedObject.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/hash/hash_append.h>
#include <xrpl/json/json_value.h>
#include <xrpl/protocol/HashPrefix.h>
#include <xrpl/protocol/PublicKey.h>
#include <xrpl/protocol/SecretKey.h>
#include <xrpl/protocol/Serializer.h>
#include <boost/container/static_vector.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>

namespace ripple {

/** Extended position for consensus with RNG entropy support.

    Carries the tx-set hash (the core convergence target), RNG set hashes
    (agreed via sub-state quorum, not via operator==), and per-validator
    leaves (unique to each proposer, piggybacked on proposals).

    Critical design:
    - operator== compares txSetHash ONLY (sub-states handle the rest)
    - add() includes ALL fields for signing (prevents stripping attacks)
*/
struct ExtendedPosition
{
    // === Core Convergence Target ===
    uint256 txSetHash;

    // === RNG Set Hashes (sub-state quorum, not in operator==) ===
    std::optional<uint256> commitSetHash;
    std::optional<uint256> entropySetHash;

    // === Per-Validator Leaves (unique per proposer) ===
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

    // TODO: replace operator== with a named method (e.g. txSetMatches())
    //   so call sites read as intent, not as "full equality".  Overloading
    //   operator== to ignore most fields is surprising and fragile.
    //
    // CRITICAL: Only compare txSetHash for consensus convergence.
    //
    // Why not commitSetHash / entropySetHash?
    //   Nodes transition through sub-states (ConvergingTx → ConvergingCommit
    //   → ConvergingReveal) at slightly different times.  If we included
    //   commitSetHash here, a node that transitions first would set it,
    //   making its position "different" from peers who haven't transitioned
    //   yet — deadlocking haveConsensus() for everyone.
    //
    //   Instead, the sub-state machine in phaseEstablish handles agreement
    //   on those fields via quorum checks (hasQuorumOfCommits, etc.).
    //
    // Implications to consider:
    //   - Two nodes with the same txSetHash but different commitSetHash
    //     will appear to "agree" from the convergence engine's perspective.
    //     This is intentional: tx consensus must not be blocked by RNG.
    //   - A malicious node could propose a different commitSetHash without
    //     affecting tx convergence.  This is safe because commitSetHash
    //     disagreement is caught by the sub-state quorum checks, and the
    //     entropy result is verified deterministically from collected reveals.
    //   - Leaves (myCommitment, myReveal) are also excluded — they are
    //     per-validator data unique to each proposer.
    //@@start rng-extended-position-equality
    bool
    operator==(ExtendedPosition const& other) const
    {
        return txSetHash == other.txSetHash;
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
    //@@end rng-extended-position-equality

    // CRITICAL: Include ALL fields for signing (prevents stripping attacks)
    //@@start rng-extended-position-serialize
    void
    add(Serializer& s) const
    {
        s.addBitString(txSetHash);

        // Wire compatibility: if no extensions, emit exactly 32 bytes
        // so legacy nodes that expect a plain uint256 work unchanged.
        if (!commitSetHash && !entropySetHash && !myCommitment && !myReveal)
            return;

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
    //@@end rng-extended-position-serialize

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

    /** Deserialize from wire format.
        Handles both legacy 32-byte hash and new extended format.
        Returns nullopt if the payload is malformed (truncated for the
        flags advertised).
    */
    //@@start rng-extended-position-deserialize
    static std::optional<ExtendedPosition>
    fromSerialIter(SerialIter& sit, std::size_t totalSize)
    {
        if (totalSize < 32)
            return std::nullopt;

        ExtendedPosition pos;
        pos.txSetHash = sit.get256();

        // Legacy format: exactly 32 bytes
        if (totalSize == 32)
            return pos;

        // Extended format: flags byte + optional uint256 fields
        if (sit.empty())
            return pos;

        std::uint8_t flags = sit.get8();

        // Reject unknown flag bits (reduces wire malleability)
        if (flags & 0xF0)
            return std::nullopt;

        // Validate exact byte count for the flagged fields.
        // Each flag bit indicates a 32-byte uint256.
        int fieldCount = 0;
        for (int i = 0; i < 4; ++i)
            if (flags & (1 << i))
                ++fieldCount;

        if (sit.getBytesLeft() != static_cast<std::size_t>(fieldCount * 32))
            return std::nullopt;

        if (flags & 0x01)
            pos.commitSetHash = sit.get256();
        if (flags & 0x02)
            pos.entropySetHash = sit.get256();
        if (flags & 0x04)
            pos.myCommitment = sit.get256();
        if (flags & 0x08)
            pos.myReveal = sit.get256();

        return pos;
    }
    //@@end rng-extended-position-deserialize
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
    // support)
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
