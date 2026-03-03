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

#ifndef RIPPLE_APP_CONSENSUS_RCLCONSENSUS_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_RCLCONSENSUS_H_INCLUDED

#include <xrpld/app/consensus/RCLCensorshipDetector.h>
#include <xrpld/app/consensus/RCLCxLedger.h>
#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpld/app/consensus/RCLCxTx.h>
#include <xrpld/app/misc/FeeVote.h>
#include <xrpld/app/misc/NegativeUNLVote.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/consensus/ConsensusTypes.h>
#include <xrpld/core/JobQueue.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/CountedObject.h>
#include <xrpl/basics/Log.h>
#include <xrpl/beast/clock/abstract_clock.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/RippleLedgerHash.h>
#include <xrpl/protocol/STValidation.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>

namespace ripple {

class CanonicalTXSet;
class InboundTransactions;
class LocalTxs;
class LedgerMaster;
class ValidatorKeys;
template <class Adaptor>
class Consensus;

/** Manages the generic consensus algorithm for use by the RCL.
 */
class RCLConsensus
{
    /** Warn for transactions that haven't been included every so many ledgers.
     */
    constexpr static unsigned int censorshipWarnInternal = 15;

    // Implements the Adaptor template interface required by Consensus.
    class Adaptor
    {
        Application& app_;
        std::unique_ptr<FeeVote> feeVote_;
        LedgerMaster& ledgerMaster_;
        LocalTxs& localTxs_;
        InboundTransactions& inboundTransactions_;
        beast::Journal const j_;

        // If the server is validating, the necessary keying information:
        ValidatorKeys const& validatorKeys_;

        // A randomly selected non-zero value used to tag our validations
        std::uint64_t const valCookie_;

        // Ledger we most recently needed to acquire
        LedgerHash acquiringLedger_;
        ConsensusParms parms_;

        // The timestamp of the last validation we used
        NetClock::time_point lastValidationTime_;

        // These members are queried via public accesors and are atomic for
        // thread safety.
        std::atomic<bool> validating_{false};
        std::atomic<std::size_t> prevProposers_{0};
        std::atomic<std::chrono::milliseconds> prevRoundTime_{
            std::chrono::milliseconds{0}};
        std::atomic<ConsensusMode> mode_{ConsensusMode::observing};

        RCLCensorshipDetector<TxID, LedgerIndex> censorshipDetector_;
        NegativeUNLVote nUnlVote_;

        // --- RNG Pipelined Storage ---
        hash_map<NodeID, uint256> pendingCommits_;
        hash_map<NodeID, uint256> pendingReveals_;
        hash_map<NodeID, PublicKey> nodeIdToKey_;

        // Ephemeral entropy secret (in-memory only, crash = non-revealer)
        uint256 myEntropySecret_;
        bool entropyFailed_ = false;

        // Real SHAMaps for the current round (unbacked, ephemeral)
        std::shared_ptr<SHAMap> commitSetMap_;
        std::shared_ptr<SHAMap> entropySetMap_;
        std::optional<LedgerIndex> rngRoundSeq_;

        // Track pending RNG set hashes we've triggered fetches for
        hash_set<uint256> pendingRngFetches_;

        // Cached set of NodeIDs from UNL Report (or fallback UNL)
        hash_set<NodeID> unlReportNodeIds_;

        // Expected proposers for commit quorum — derived from last round's
        // actual proposers (best signal), falling back to UNL Report.
        hash_set<NodeID> expectedProposers_;

        /** Proof data from a proposal signature, for embedding in SHAMap
            entries. Contains everything needed to independently verify
            that a validator committed/revealed a specific value. */
        struct ProposalProof
        {
            std::uint32_t proposeSeq;
            std::uint32_t closeTime;
            uint256 prevLedger;
            Serializer positionData;  // serialized ExtendedPosition
            Buffer signature;
        };

        // Proposal proofs keyed by NodeID.
        // commitProofs_: only seq=0 proofs (deterministic across all nodes).
        // proposalProofs_: latest proof with reveal (for entropySet).
        hash_map<NodeID, ProposalProof> commitProofs_;
        hash_map<NodeID, ProposalProof> proposalProofs_;

    public:
        using Ledger_t = RCLCxLedger;
        using NodeID_t = NodeID;
        using NodeKey_t = PublicKey;
        using TxSet_t = RCLTxSet;
        using PeerPosition_t = RCLCxPeerPos;
        using Position_t = ExtendedPosition;

        using Result = ConsensusResult<Adaptor>;

        Adaptor(
            Application& app,
            std::unique_ptr<FeeVote>&& feeVote,
            LedgerMaster& ledgerMaster,
            LocalTxs& localTxs,
            InboundTransactions& inboundTransactions,
            ValidatorKeys const& validatorKeys,
            beast::Journal journal);

        bool
        validating() const
        {
            return validating_;
        }

        std::size_t
        prevProposers() const
        {
            return prevProposers_;
        }

        std::chrono::milliseconds
        prevRoundTime() const
        {
            return prevRoundTime_;
        }

        ConsensusMode
        mode() const
        {
            return mode_;
        }

        /** Called before kicking off a new consensus round.

            @param prevLedger Ledger that will be prior ledger for next round
            @param nowTrusted the new validators
            @return Whether we enter the round proposing
        */
        bool
        preStartRound(
            RCLCxLedger const& prevLedger,
            hash_set<NodeID> const& nowTrusted);

        bool
        haveValidated() const;

        LedgerIndex
        getValidLedgerIndex() const;

        std::pair<std::size_t, hash_set<NodeKey_t>>
        getQuorumKeys() const;

        std::size_t
        laggards(Ledger_t::Seq const seq, hash_set<NodeKey_t>& trustedKeys)
            const;

        /** Whether I am a validator.
         *
         * @return whether I am a validator.
         */
        bool
        validator() const;

        /** Update operating mode based on current peer positions.
         *
         * If our current ledger has no agreement from the network,
         * then we cannot be in the omFULL mode.
         *
         * @param positions Number of current peer positions.
         */
        void
        updateOperatingMode(std::size_t const positions) const;

        /** Consensus simulation parameters
         */
        ConsensusParms const&
        parms() const
        {
            return parms_;
        }

        // --- RNG Helper Methods ---

        /** Get the quorum threshold (80% of trusted validators) */
        std::size_t
        quorumThreshold() const;

        /** Set expected proposers for this round's commit quorum.
            Cascade: recent proposers > UNL Report > (empty = 80% fallback).
        */
        void
        setExpectedProposers(hash_set<NodeID> proposers);

        /** Number of pending commits (for timeout fallback check) */
        std::size_t
        pendingCommitCount() const;

        /** Check if we have quorum of commits */
        bool
        hasQuorumOfCommits() const;

        /** Check if we have minimum reveals for consensus */
        bool
        hasMinimumReveals() const;

        /** Check if we have any reveals at all */
        bool
        hasAnyReveals() const;

        /** Build real SHAMap from collected commits, register for fetch.
            @param seq The ledger sequence being built
            @return The SHAMap root hash (commitSetHash)
        */
        uint256
        buildCommitSet(LedgerIndex seq);

        /** Build real SHAMap from collected reveals, register for fetch.
            @param seq The ledger sequence being built
            @return The SHAMap root hash (entropySetHash)
        */
        uint256
        buildEntropySet(LedgerIndex seq);

        /** Check if a hash is a known RNG set (commitSet or entropySet) */
        bool
        isRngSet(uint256 const& hash) const;

        /** Handle an acquired RNG set — diff, merge missing entries */
        void
        handleAcquiredRngSet(std::shared_ptr<SHAMap> const& map);

        /** Trigger fetch for a peer's unknown RNG set hash */
        void
        fetchRngSetIfNeeded(std::optional<uint256> const& hash);

        /** Cache the active UNL NodeIDs for this round.
            Reads from UNL Report (in-ledger), falls back to normal UNL.
        */
        void
        cacheUNLReport();

        /** Check if a NodeID is in the active UNL for this round */
        bool
        isUNLReportMember(NodeID const& nodeId) const;

        /** Generate new entropy secret for this round */
        void
        generateEntropySecret();

        /** Get the current entropy secret */
        uint256
        getEntropySecret() const;

        /** Mark entropy as failed for this round */
        void
        setEntropyFailed();

        /** Get our validator public key */
        PublicKey const&
        validatorKey() const;

        /** Clear RNG state for new round */
        void
        clearRngState();

        /** Inject consensus entropy pseudo-transaction into the tx set.

            Creates a ttCONSENSUS_ENTROPY pseudo-transaction from collected
            reveals and injects it into the transaction set. This must be
            called before buildLCL so the entropy is written to the ledger.

            @param retriableTxs The canonical transaction set to inject into
            @param seq The ledger sequence being built
        */
        void
        injectEntropyPseudoTx(CanonicalTXSet& retriableTxs, LedgerIndex seq);

        /** Harvest RNG data from a peer proposal.

            Extracts commits and reveals from the proposal's ExtendedPosition
            and stores them in pending collections for later processing.
            Also captures a ProposalProof for embedding in SHAMap entries.
        */
        void
        harvestRngData(
            NodeID const& nodeId,
            PublicKey const& publicKey,
            ExtendedPosition const& position,
            std::uint32_t proposeSeq,
            NetClock::time_point closeTime,
            uint256 const& prevLedger,
            Slice const& signature);

        /** Serialize a ProposalProof into a blob for sfBlob */
        static Blob
        serializeProof(ProposalProof const& proof);

        /** Deserialize a ProposalProof blob from sfBlob.
            @return Parsed proof or std::nullopt if malformed. */
        static std::optional<ProposalProof>
        deserializeProof(Blob const& proofBlob);

        /** Verify a proof blob against the entry's public key and digest.
            @return true if the proof is valid */
        static bool
        verifyProof(
            Blob const& proofBlob,
            PublicKey const& publicKey,
            uint256 const& expectedDigest,
            bool isCommit);

    private:
        //---------------------------------------------------------------------
        // The following members implement the generic Consensus requirements
        // and are marked private to indicate ONLY Consensus<Adaptor> will call
        // them (via friendship). Since they are called only from
        // Consensus<Adaptor> methods and since RCLConsensus::consensus_ should
        // only be accessed under lock, these will only be called under lock.
        //
        // In general, the idea is that there is only ONE thread that is running
        // consensus code at anytime. The only special case is the dispatched
        // onAccept call, which does not take a lock and relies on Consensus not
        // changing state until a future call to startRound.
        friend class Consensus<Adaptor>;

        /** Attempt to acquire a specific ledger.

            If not available, asynchronously acquires from the network.

            @param hash The ID/hash of the ledger acquire
            @return Optional ledger, will be seated if we locally had the ledger
        */
        std::optional<RCLCxLedger>
        acquireLedger(LedgerHash const& hash);

        /** Share the given proposal with all peers

            @param peerPos The peer position to share.
         */
        void
        share(RCLCxPeerPos const& peerPos);

        /** Share disputed transaction to peers.

            Only share if the provided transaction hasn't been shared recently.

            @param tx The disputed transaction to share.
        */
        void
        share(RCLCxTx const& tx);

        /** Acquire the transaction set associated with a proposal.

            If the transaction set is not available locally, will attempt
            acquire it from the network.

            @param setId The transaction set ID associated with the proposal
            @return Optional set of transactions, seated if available.
       */
        std::optional<RCLTxSet>
        acquireTxSet(RCLTxSet::ID const& setId);

        /** Whether the open ledger has any transactions
         */
        bool
        hasOpenTransactions() const;

        /** Number of proposers that have validated the given ledger

            @param h The hash of the ledger of interest
            @return the number of proposers that validated a ledger
        */
        std::size_t
        proposersValidated(LedgerHash const& h) const;

        /** Number of proposers that have validated a ledger descended from
           requested ledger.

            @param ledger The current working ledger
            @param h The hash of the preferred working ledger
            @return The number of validating peers that have validated a ledger
                    descended from the preferred working ledger.
        */
        std::size_t
        proposersFinished(RCLCxLedger const& ledger, LedgerHash const& h) const;

        /** Propose the given position to my peers.

            @param proposal Our proposed position
        */
        void
        propose(RCLCxPeerPos::Proposal const& proposal);

        /** Share the given tx set to peers.

            @param txns The TxSet to share.
        */
        void
        share(RCLTxSet const& txns);

        /** Get the ID of the previous ledger/last closed ledger(LCL) on the
           network

            @param ledgerID ID of previous ledger used by consensus
            @param ledger Previous ledger consensus has available
            @param mode Current consensus mode
            @return The id of the last closed network

            @note ledgerID may not match ledger.id() if we haven't acquired
                  the ledger matching ledgerID from the network
         */
        uint256
        getPrevLedger(
            uint256 ledgerID,
            RCLCxLedger const& ledger,
            ConsensusMode mode);

        /** Notified of change in consensus mode

            @param before The prior consensus mode
            @param after The new consensus mode
        */
        void
        onModeChange(ConsensusMode before, ConsensusMode after);

        /** Close the open ledger and return initial consensus position.

           @param ledger the ledger we are changing to
           @param closeTime When consensus closed the ledger
           @param mode Current consensus mode
           @return Tentative consensus result
        */
        Result
        onClose(
            RCLCxLedger const& ledger,
            NetClock::time_point const& closeTime,
            ConsensusMode mode);

        /** Process the accepted ledger.

            @param result The result of consensus
            @param prevLedger The closed ledger consensus worked from
            @param closeResolution The resolution used in agreeing on an
                                   effective closeTime
            @param rawCloseTimes The unrounded closetimes of ourself and our
                                 peers
            @param mode Our participating mode at the time consensus was
                        declared
            @param consensusJson Json representation of consensus state
            @param validating whether this is a validator
        */
        void
        onAccept(
            Result const& result,
            RCLCxLedger const& prevLedger,
            NetClock::duration const& closeResolution,
            ConsensusCloseTimes const& rawCloseTimes,
            ConsensusMode const& mode,
            Json::Value&& consensusJson,
            const bool validating);

        /** Process the accepted ledger that was a result of simulation/force
            accept.

            @ref onAccept
        */
        void
        onForceAccept(
            Result const& result,
            RCLCxLedger const& prevLedger,
            NetClock::duration const& closeResolution,
            ConsensusCloseTimes const& rawCloseTimes,
            ConsensusMode const& mode,
            Json::Value&& consensusJson);

        /** Notify peers of a consensus state change

            @param ne Event type for notification
            @param ledger The ledger at the time of the state change
            @param haveCorrectLCL Whether we believe we have the correct LCL.
        */
        void
        notify(
            protocol::NodeEvent ne,
            RCLCxLedger const& ledger,
            bool haveCorrectLCL);

        /** Accept a new ledger based on the given transactions.

            @ref onAccept
         */
        void
        doAccept(
            Result const& result,
            RCLCxLedger const& prevLedger,
            NetClock::duration closeResolution,
            ConsensusCloseTimes const& rawCloseTimes,
            ConsensusMode const& mode,
            Json::Value&& consensusJson);

        /** Build the new last closed ledger.

            Accept the given the provided set of consensus transactions and
            build the last closed ledger. Since consensus just agrees on which
            transactions to apply, but not whether they make it into the closed
            ledger, this function also populates retriableTxs with those that
            can be retried in the next round.

            @param previousLedger Prior ledger building upon
            @param retriableTxs On entry, the set of transactions to apply to
                                the ledger; on return, the set of transactions
                                to retry in the next round.
            @param closeTime The time the ledger closed
            @param closeTimeCorrect Whether consensus agreed on close time
            @param closeResolution Resolution used to determine consensus close
                                   time
            @param roundTime Duration of this consensus round
            @param failedTxs Populate with transactions that we could not
                             successfully apply.
            @return The newly built ledger
        */
        RCLCxLedger
        buildLCL(
            RCLCxLedger const& previousLedger,
            CanonicalTXSet& retriableTxs,
            NetClock::time_point closeTime,
            bool closeTimeCorrect,
            NetClock::duration closeResolution,
            std::chrono::milliseconds roundTime,
            std::set<TxID>& failedTxs);

        /** Validate the given ledger and share with peers as necessary

            @param ledger The ledger to validate
            @param txns The consensus transaction set
            @param proposing Whether we were proposing transactions while
                             generating this ledger.  If we are not proposing,
                             a validation can still be sent to inform peers that
                             we know we aren't fully participating in consensus
                             but are still around and trying to catch up.
        */
        void
        validate(
            RCLCxLedger const& ledger,
            RCLTxSet const& txns,
            bool proposing);
    };

public:
    using clock_type = beast::abstract_clock<std::chrono::steady_clock>;

    //! Constructor
    RCLConsensus(
        Application& app,
        std::unique_ptr<FeeVote>&& feeVote,
        LedgerMaster& ledgerMaster,
        LocalTxs& localTxs,
        InboundTransactions& inboundTransactions,
        clock_type const& clock,
        ValidatorKeys const& validatorKeys,
        beast::Journal journal);

    ~RCLConsensus();

    RCLConsensus(RCLConsensus const&) = delete;

    RCLConsensus&
    operator=(RCLConsensus const&) = delete;

    //! Whether we are validating consensus ledgers.
    bool
    validating() const
    {
        return adaptor_.validating();
    }

    //! Get the number of proposing peers that participated in the previous
    //! round.
    std::size_t
    prevProposers() const
    {
        return adaptor_.prevProposers();
    }

    /** Get duration of the previous round.

        The duration of the round is the establish phase, measured from closing
        the open ledger to accepting the consensus result.

        @return Last round duration in milliseconds
    */
    std::chrono::milliseconds
    prevRoundTime() const
    {
        return adaptor_.prevRoundTime();
    }

    //! @see Consensus::mode
    ConsensusMode
    mode() const
    {
        return adaptor_.mode();
    }

    ConsensusPhase
    phase() const;

    //! @see Consensus::inRngSubState
    bool
    inRngSubState() const;

    //! Check if a hash is a known RNG set (commitSet or entropySet)
    bool
    isRngSet(uint256 const& hash) const
    {
        std::lock_guard _{mutex_};
        return adaptor_.isRngSet(hash);
    }

    //! Handle an acquired RNG set from InboundTransactions
    void
    gotRngSet(std::shared_ptr<SHAMap> const& map)
    {
        std::lock_guard _{mutex_};
        adaptor_.handleAcquiredRngSet(map);
    }

    //! @see Consensus::getJson
    Json::Value
    getJson(bool full) const;

    /** Adjust the set of trusted validators and kick-off the next round of
       consensus. For more details, @see Consensus::startRound
     */
    void
    startRound(
        NetClock::time_point const& now,
        RCLCxLedger::ID const& prevLgrId,
        RCLCxLedger const& prevLgr,
        hash_set<NodeID> const& nowUntrusted,
        hash_set<NodeID> const& nowTrusted,
        std::unique_ptr<std::stringstream> const& clog);

    //! @see Consensus::timerEntry
    void
    timerEntry(
        NetClock::time_point const& now,
        std::unique_ptr<std::stringstream> const& clog = {});

    //! @see Consensus::gotTxSet
    void
    gotTxSet(NetClock::time_point const& now, RCLTxSet const& txSet);

    // @see Consensus::prevLedgerID
    RCLCxLedger::ID
    prevLedgerID() const;

    //! @see Consensus::simulate
    void
    simulate(
        NetClock::time_point const& now,
        std::optional<std::chrono::milliseconds> consensusDelay);

    //! @see Consensus::proposal
    bool
    peerProposal(
        NetClock::time_point const& now,
        RCLCxPeerPos const& newProposal);

    ConsensusParms const&
    parms() const
    {
        return adaptor_.parms();
    }

private:
    // Since Consensus does not provide intrinsic thread-safety, this mutex
    // guards all calls to consensus_. adaptor_ uses atomics internally
    // to allow concurrent access of its data members that have getters.
    mutable std::recursive_mutex mutex_;

    Adaptor adaptor_;
    std::unique_ptr<Consensus<Adaptor>> consensus_;
    beast::Journal const j_;
};

/** Collects logging information.
 *
 * Eases correlating multiple data points together to
 * help follow flow of a complex activity, such as
 * consensus heartbeat timer. If we
 * are a validator, then always write output to
 * debug log on destruction. Otherwise, INFO
 * level.
 */
class RclConsensusLogger
{
    std::string header_;
    beast::Journal j_;
    std::unique_ptr<std::stringstream> ss_;
    std::chrono::steady_clock::time_point start_;

public:
    explicit RclConsensusLogger(
        const char* label,
        bool validating,
        beast::Journal j);
    ~RclConsensusLogger();

    std::unique_ptr<std::stringstream> const&
    ss()
    {
        return ss_;
    }
};
}  // namespace ripple

#endif
