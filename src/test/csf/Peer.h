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
#ifndef RIPPLE_TEST_CSF_PEER_H_INCLUDED
#define RIPPLE_TEST_CSF_PEER_H_INCLUDED

#include <test/csf/CollectorRef.h>
#include <test/csf/Proposal.h>
#include <test/csf/Scheduler.h>
#include <test/csf/TrustGraph.h>
#include <test/csf/Tx.h>
#include <test/csf/Validation.h>
#include <test/csf/events.h>
#include <test/csf/ledgers.h>
#include <xrpld/consensus/Consensus.h>
#include <xrpld/consensus/Validations.h>
#include <xrpl/basics/base_uint.h>
#include <xrpl/beast/utility/WrappedSink.h>
#include <xrpl/protocol/PublicKey.h>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace ripple {
namespace test {
namespace csf {

namespace bc = boost::container;

/** A single peer in the simulation.

    This is the main work-horse of the consensus simulation framework and is
    where many other components are integrated. The peer

     - Implements the Callbacks required by Consensus
     - Manages trust & network connections with other peers
     - Issues events back to the simulation based on its actions for analysis
       by Collectors
     - Exposes most internal state for forcibly simulating arbitrary scenarios
*/
/// Content-addressed sidecar set store, simulating InboundTransactions.
/// Shared across all peers in a simulation — peers publish sets by hash
/// and fetch them by hash, just like the real SHAMap fetch pipeline.
///
/// Each entry is tagged with its type so fetchRngSetIfNeeded can merge
/// into the correct local set without content-sniffing heuristics.
struct SidecarStore
{
    enum class Type { commit, reveal, exportSig };

    using EntrySet = hash_map<PeerID, uint256>;

    struct TaggedSet
    {
        Type type;
        EntrySet entries;
    };

    void
    publish(uint256 const& hash, Type type, EntrySet const& entries)
    {
        sets_[hash] = {type, entries};
    }

    TaggedSet const*
    fetch(uint256 const& hash) const
    {
        auto it = sets_.find(hash);
        return it != sets_.end() ? &it->second : nullptr;
    }

private:
    std::map<uint256, TaggedSet> sets_;
};

struct Peer
{
    /** Basic wrapper of a proposed position taken by a peer.

        For real consensus, this would add additional data for serialization
        and signing. For simulation, nothing extra is needed.
    */
    class Position
    {
    public:
        using Proposal = csf::Proposal;

        Position(Proposal const& p) : proposal_(p)
        {
        }

        Proposal const&
        proposal() const
        {
            return proposal_;
        }

        Json::Value
        getJson() const
        {
            return proposal_.getJson();
        }

        PeerKey
        publicKey() const
        {
            return {proposal_.nodeID(), 0};
        }

        std::uint64_t
        signature() const
        {
            return 0;
        }

        std::string
        render() const
        {
            return "";
        }

    private:
        Proposal proposal_;
    };

    /** Simulated delays in internal peer processing.
     */
    struct ProcessingDelays
    {
        //! Delay in consensus calling doAccept to accepting and issuing
        //! validation
        //! TODO: This should be a function of the number of transactions
        std::chrono::milliseconds ledgerAccept{0};

        //! Delay in processing validations from remote peers
        std::chrono::milliseconds recvValidation{0};

        // Return the receive delay for message type M, default is no delay
        // Received delay is the time from receiving the message to actually
        // handling it.
        template <class M>
        SimDuration
        onReceive(M const&) const
        {
            return SimDuration{};
        }

        SimDuration
        onReceive(Validation const&) const
        {
            return recvValidation;
        }
    };

    class TestConsensusLogger
    {
    };

    /** Generic Validations adaptor that simply ignores recently stale
     * validations
     */
    class ValAdaptor
    {
        Peer& p_;

    public:
        struct Mutex
        {
            void
            lock()
            {
            }

            void
            unlock()
            {
            }
        };

        using Validation = csf::Validation;
        using Ledger = csf::Ledger;

        ValAdaptor(Peer& p) : p_{p}
        {
        }

        NetClock::time_point
        now() const
        {
            return p_.now();
        }

        std::optional<Ledger>
        acquire(Ledger::ID const& lId)
        {
            if (Ledger const* ledger = p_.acquireLedger(lId))
                return *ledger;
            return std::nullopt;
        }
    };

    //! Type definitions for generic consensus
    using Ledger_t = Ledger;
    using NodeID_t = PeerID;
    using NodeKey_t = PeerKey;
    using TxSet_t = TxSet;
    using PeerPosition_t = Position;
    using Position_t = ProposalPosition;
    using Result = ConsensusResult<Peer>;
    using NodeKey = Validation::NodeKey;

    //! Logging support that prefixes messages with the peer ID
    beast::WrappedSink sink;
    beast::Journal j;

    //! Generic consensus
    Consensus<Peer> consensus;

    //! Our unique ID
    PeerID id;

    //! Current signing key
    PeerKey key;

    //! The oracle that manages unique ledgers
    LedgerOracle& oracle;

    //! Shared sidecar store (simulates InboundTransactions)
    SidecarStore& sidecarStore;

    //! Scheduler of events
    Scheduler& scheduler;

    //! Handle to network for sending messages
    BasicNetwork<Peer*>& net;

    //! Handle to Trust graph of network
    TrustGraph<Peer*>& trustGraph;

    //! openTxs that haven't been closed in a ledger yet
    TxSetType openTxs;

    //! The last ledger closed by this node
    Ledger lastClosedLedger;

    //! Ledgers this node has closed or loaded from the network
    hash_map<Ledger::ID, Ledger> ledgers;

    //! Validations from trusted nodes
    Validations<ValAdaptor> validations;

    //! The most recent ledger that has been fully validated by the network from
    //! the perspective of this Peer
    Ledger fullyValidatedLedger;

    //-------------------------------------------------------------------------
    // Store most network messages; these could be purged if memory use ever
    // becomes problematic

    //! Map from Ledger::ID to vector of Positions with that ledger
    //! as the prior ledger
    bc::flat_map<Ledger::ID, std::vector<Proposal>> peerPositions;
    //! TxSet associated with a TxSet::ID
    bc::flat_map<TxSet::ID, TxSet> txSets;

    // Ledgers/TxSets we are acquiring and when that request times out
    bc::flat_map<Ledger::ID, SimTime> acquiringLedgers;
    bc::flat_map<TxSet::ID, SimTime> acquiringTxSets;

    //! The number of ledgers this peer has completed
    int completedLedgers = 0;

    //! The number of ledgers this peer should complete before stopping to run
    int targetLedgers = std::numeric_limits<int>::max();

    //! Skew of time relative to the common scheduler clock
    std::chrono::seconds clockSkew{0};

    //! Simulated delays to use for internal processing
    ProcessingDelays delays;

    //! Whether to simulate running as validator or a tracking node
    bool runAsValidator = true;

    // TODO: Consider removing these two, they are only a convenience for tests
    // Number of proposers in the prior round
    std::size_t prevProposers = 0;
    // Duration of prior round
    std::chrono::milliseconds prevRoundTime;

    // Quorum of validations needed for a ledger to be fully validated
    // TODO: Use the logic in ValidatorList to set this dynamically
    std::size_t quorum = 0;

    hash_set<NodeKey_t> trustedKeys;

    // Simulation parameters
    ConsensusParms consensusParms;

    /// RNG consensus extensions for CSF. Owns all RNG state and methods,
    /// same pattern as ConsensusExtensions for production.
    struct Extensions
    {
        Peer& peer;
        beast::Journal j_;

        // Sub-state machine
        EstablishState estState_{EstablishState::ConvergingTx};
        std::chrono::steady_clock::time_point revealPhaseStart_{};
        std::chrono::steady_clock::time_point commitHashConflictStart_{};
        bool explicitFinalProposalSent_{false};
        bool entropySetPublished_{false};
        std::chrono::steady_clock::time_point entropyPublishStart_{};
        bool exportSigGateStarted_{false};
        std::chrono::steady_clock::time_point exportSigGateStart_{};
        bool exportSigConvergenceFailed_{false};

        // RNG state
        bool enableRngConsensus_ = false;
        bool enableExportConsensus_ = false;
        hash_set<PeerID> unlNodes_;
        hash_set<PeerID> likelyParticipants_;
        hash_map<PeerID, uint256> pendingCommits_;
        hash_map<PeerID, uint256> pendingReveals_;
        hash_map<PeerID, uint256> pendingExportSigs_;
        hash_map<PeerID, PeerKey> nodeKeys_;
        uint256 myEntropySecret_;
        bool entropyFailed_ = false;

        // Last round summary (for test assertions)
        uint256 lastEntropyDigest_;
        std::uint16_t lastEntropyCount_ = 0;
        bool lastEntropyWasFallback_ = true;
        bool lastExportSucceeded_ = false;
        bool lastExportRetried_ = false;

        // Optional test hook: force a specific commit-set hash
        std::optional<uint256> forcedCommitSetHash_;
        // Optional test hook: force a specific entropy-set hash
        std::optional<uint256> forcedEntropySetHash_;
        // Optional test hook: force a specific export sig-set hash
        std::optional<uint256> forcedExportSigSetHash_;

        // Optional test hook: drop reveals from specific peers
        // (simulates asymmetric reveal delivery / packet loss)
        hash_set<PeerID> dropRevealFrom_;
        // Optional test hook: drop proposal-carried export signatures.
        hash_set<PeerID> dropExportSigFrom_;

        explicit Extensions(Peer& p) : peer(p), j_(p.j)
        {
        }

        // --- RNG methods ---

        bool
        rngEnabled() const
        {
            return enableRngConsensus_;
        }

        bool
        exportEnabled() const
        {
            return enableExportConsensus_;
        }

        std::size_t
        quorumThreshold() const
        {
            if (!enableRngConsensus_)
                return (std::numeric_limits<std::size_t>::max)() / 4;
            auto const base = unlNodes_.size();
            return calculateQuorumThreshold(base == 0 ? 1 : base);
        }

        std::size_t
        exportSigQuorumThreshold() const
        {
            if (!enableExportConsensus_)
                return (std::numeric_limits<std::size_t>::max)() / 4;
            auto const base =
                unlNodes_.empty() ? std::size_t{1} : unlNodes_.size();
            return enableRngConsensus_ ? calculateQuorumThreshold(base) : base;
        }

        std::size_t
        pendingCommitCount() const
        {
            return pendingCommits_.size();
        }

        std::size_t
        pendingRevealCount() const
        {
            return pendingReveals_.size();
        }

        std::size_t
        expectedProposerCount() const
        {
            return likelyParticipants_.size();
        }

        bool
        hasQuorumOfCommits() const
        {
            if (!enableRngConsensus_)
                return false;
            return pendingCommits_.size() >= quorumThreshold();
        }

        bool
        hasMinimumReveals() const
        {
            if (!enableRngConsensus_)
                return false;
            return pendingReveals_.size() >= pendingCommits_.size();
        }

        bool
        hasAnyReveals() const
        {
            if (!enableRngConsensus_)
                return false;
            return !pendingReveals_.empty();
        }

        bool
        shouldZeroEntropy() const
        {
            if (entropyFailed_ || pendingReveals_.empty())
                return true;
            // Match production: zero when reveals < quorum threshold.
            auto const threshold = unlNodes_.empty()
                ? std::size_t{1}
                : calculateQuorumThreshold(unlNodes_.size());
            return pendingReveals_.size() < threshold;
        }

        uint256
        buildCommitSet(Ledger::Seq seq)
        {
            if (forcedCommitSetHash_)
                return *forcedCommitSetHash_;
            auto const hash = hashRngSet(pendingCommits_, seq, "commit");
            peer.sidecarStore.publish(
                hash, SidecarStore::Type::commit, pendingCommits_);
            return hash;
        }

        uint256
        buildEntropySet(Ledger::Seq seq)
        {
            if (forcedEntropySetHash_)
                return *forcedEntropySetHash_;
            auto const hash = hashRngSet(pendingReveals_, seq, "reveal");
            peer.sidecarStore.publish(
                hash, SidecarStore::Type::reveal, pendingReveals_);
            return hash;
        }

        uint256
        buildExportSigSet(Ledger::Seq seq)
        {
            if (forcedExportSigSetHash_)
                return *forcedExportSigSetHash_;
            auto const hash = hashRngSet(pendingExportSigs_, seq, "export-sig");
            peer.sidecarStore.publish(
                hash, SidecarStore::Type::exportSig, pendingExportSigs_);
            return hash;
        }

        void
        generateEntropySecret()
        {
            if (!enableRngConsensus_)
                return;
            auto const seq =
                static_cast<std::uint32_t>(peer.lastClosedLedger.seq()) + 1;
            myEntropySecret_ = sha512Half(
                std::string("csf-rng-secret"),
                static_cast<std::uint32_t>(peer.id),
                peer.key.second,
                seq,
                peer.completedLedgers);
        }

        uint256
        getEntropySecret() const
        {
            return myEntropySecret_;
        }

        void
        selfSeedReveal()
        {
            if (!enableRngConsensus_)
                return;
            // Self-seed our own reveal into pendingReveals_ so it
            // counts toward reveal quorum.  The real code does this
            // in decorateMessage; the CSF does it here since it has
            // no equivalent serialization hook.
            if (myEntropySecret_ != uint256{})
                pendingReveals_[peer.id] = myEntropySecret_;
        }

        void
        setEntropyFailed()
        {
            if (!enableRngConsensus_)
                return;
            entropyFailed_ = true;
        }

        enum class SidecarKind : uint8_t { commit, reveal, exportSig };

        void
        fetchRngSetIfNeeded(
            std::optional<uint256> const& hash,
            SidecarKind kind = SidecarKind::commit)
        {
            if (!hash)
                return;
            auto const* fetched = peer.sidecarStore.fetch(*hash);
            if (!fetched)
                return;
            // Union merge into the correct local set based on type.
            auto& target = [&]() -> hash_map<PeerID, uint256>& {
                switch (fetched->type)
                {
                    case SidecarStore::Type::commit:
                        return pendingCommits_;
                    case SidecarStore::Type::reveal:
                        return pendingReveals_;
                    case SidecarStore::Type::exportSig:
                        return pendingExportSigs_;
                }
                return pendingCommits_;
            }();
            for (auto const& [nodeId, digest] : fetched->entries)
                target.emplace(nodeId, digest);
        }

        void
        fetchSidecarsIfNeeded(ProposalPosition const& pos)
        {
            fetchRngSetIfNeeded(pos.commitSetHash, SidecarKind::commit);
            fetchRngSetIfNeeded(pos.entropySetHash, SidecarKind::reveal);
            fetchRngSetIfNeeded(pos.exportSigSetHash, SidecarKind::exportSig);
        }

        void
        clearRngState()
        {
            pendingCommits_.clear();
            pendingReveals_.clear();
            pendingExportSigs_.clear();
            nodeKeys_.clear();
            likelyParticipants_.clear();
            myEntropySecret_.zero();
            entropyFailed_ = false;
            exportSigGateStarted_ = false;
            exportSigGateStart_ = {};
            exportSigConvergenceFailed_ = false;
        }

        void
        cacheUNLReport()
        {
            unlNodes_.clear();
            for (auto const* p : peer.trustGraph.trustedPeers(&peer))
            {
                if (!peer.runAsValidator && p->id == peer.id)
                    continue;
                unlNodes_.insert(p->id);
            }
            if (peer.runAsValidator)
                unlNodes_.insert(peer.id);
        }

        void
        setExpectedProposers(hash_set<PeerID> proposers)
        {
            bool const includeSelf = peer.runAsValidator;
            if (!proposers.empty())
            {
                hash_set<PeerID> filtered;
                for (auto const& nid : proposers)
                {
                    if (!includeSelf && nid == peer.id)
                        continue;
                    if (isUNLReportMember(nid))
                        filtered.insert(nid);
                }
                if (includeSelf)
                    filtered.insert(peer.id);
                likelyParticipants_ = std::move(filtered);
                return;
            }
            likelyParticipants_.clear();
            if (!unlNodes_.empty())
                likelyParticipants_ = unlNodes_;
        }

        void
        harvestRngData(
            PeerID const& nodeId,
            PeerKey const& publicKey,
            ProposalPosition const& position,
            std::uint32_t,
            NetClock::time_point,
            Ledger::ID const& prevLedger,
            std::uint64_t)
        {
            if (!enableRngConsensus_ && !enableExportConsensus_)
                return;
            if (!isUNLReportMember(nodeId))
                return;

            nodeKeys_.insert_or_assign(nodeId, publicKey);

            if (enableRngConsensus_ && position.myCommitment)
            {
                auto [it, inserted] =
                    pendingCommits_.emplace(nodeId, *position.myCommitment);
                if (!inserted && it->second != *position.myCommitment)
                {
                    it->second = *position.myCommitment;
                    pendingReveals_.erase(nodeId);
                }
            }

            if (!enableRngConsensus_ || !position.myReveal)
            {
                if (enableExportConsensus_ && position.myExportSignature &&
                    dropExportSigFrom_.count(nodeId) == 0)
                    pendingExportSigs_[nodeId] = *position.myExportSignature;
                return;
            }

            // Test hook: drop reveals from specific peers
            if (dropRevealFrom_.count(nodeId) == 0)
            {
                auto const commitIt = pendingCommits_.find(nodeId);
                if (commitIt != pendingCommits_.end())
                {
                    auto const prevIt = peer.ledgers.find(prevLedger);
                    if (prevIt != peer.ledgers.end())
                    {
                        auto const seq =
                            static_cast<std::uint32_t>(prevIt->second.seq()) +
                            1;
                        auto const expected = sha512Half(
                            *position.myReveal,
                            static_cast<std::uint32_t>(publicKey.first),
                            publicKey.second,
                            seq);
                        if (expected == commitIt->second)
                            pendingReveals_[nodeId] = *position.myReveal;
                    }
                }
            }

            if (enableExportConsensus_ && position.myExportSignature &&
                dropExportSigFrom_.count(nodeId) == 0)
                pendingExportSigs_[nodeId] = *position.myExportSignature;
        }

        bool
        isUNLReportMember(PeerID const& nodeId) const
        {
            return unlNodes_.count(nodeId) > 0;
        }

        void
        finalizeRoundEntropy(std::uint32_t seq)
        {
            if (!enableRngConsensus_)
            {
                lastEntropyDigest_.zero();
                lastEntropyCount_ = 0;
                lastEntropyWasFallback_ = true;
                return;
            }

            if (shouldZeroEntropy())
            {
                lastEntropyDigest_.zero();
                lastEntropyCount_ = 0;
                lastEntropyWasFallback_ = true;
                return;
            }

            std::vector<std::pair<PeerKey, uint256>> ordered;
            ordered.reserve(pendingReveals_.size());
            for (auto const& [nodeId, reveal] : pendingReveals_)
            {
                auto const it = nodeKeys_.find(nodeId);
                if (it == nodeKeys_.end())
                    continue;
                ordered.emplace_back(it->second, reveal);
            }

            if (ordered.empty())
            {
                lastEntropyDigest_.zero();
                lastEntropyCount_ = 0;
                lastEntropyWasFallback_ = true;
                return;
            }

            std::sort(
                ordered.begin(),
                ordered.end(),
                [](auto const& a, auto const& b) {
                    if (a.first.first != b.first.first)
                        return a.first.first < b.first.first;
                    return a.first.second < b.first.second;
                });

            uint256 digest = sha512Half(
                std::string("csf-rng-entropy"),
                static_cast<std::uint32_t>(seq));
            for (auto const& [keyId, reveal] : ordered)
            {
                digest = sha512Half(
                    digest,
                    static_cast<std::uint32_t>(keyId.first),
                    keyId.second,
                    reveal);
            }

            lastEntropyDigest_ = digest;
            lastEntropyCount_ = static_cast<std::uint16_t>(ordered.size());
            lastEntropyWasFallback_ = false;
        }

        void
        finalizeRoundExport()
        {
            if (!enableExportConsensus_)
            {
                lastExportSucceeded_ = false;
                lastExportRetried_ = false;
                return;
            }

            auto const activeSigCount = std::count_if(
                pendingExportSigs_.begin(),
                pendingExportSigs_.end(),
                [&](auto const& entry) {
                    return isUNLReportMember(entry.first);
                });
            lastExportSucceeded_ = !exportSigConvergenceFailed_ &&
                static_cast<std::size_t>(activeSigCount) >=
                    exportSigQuorumThreshold();
            lastExportRetried_ = !lastExportSucceeded_;
        }

        // --- Lifecycle hooks (matching design doc) ---

        template <class Ledger_t>
        void
        onRoundStart(
            Ledger_t const& /* prevLedger */,
            hash_set<PeerID> lastProposers)
        {
            clearRngState();
            cacheUNLReport();
            setExpectedProposers(std::move(lastProposers));
            resetSubState();
        }

        void
        onTrustedPeerProposal(
            PeerID const& nodeId,
            PeerKey const& publicKey,
            ProposalPosition const& position,
            std::uint32_t proposeSeq,
            NetClock::time_point closeTime,
            Ledger::ID const& prevLedger,
            std::uint64_t signature)
        {
            harvestRngData(
                nodeId,
                publicKey,
                position,
                proposeSeq,
                closeTime,
                prevLedger,
                signature);
        }

        void
        onAcceptComplete()
        {
        }

        template <class Ledger_t>
        void
        decoratePosition(
            ProposalPosition& pos,
            Ledger_t const& prevLedger,
            bool proposing)
        {
            decorateExportPosition(pos, prevLedger, proposing);

            if (!enableRngConsensus_ || !proposing || !peer.runAsValidator)
                return;
            generateEntropySecret();
            auto const seq = static_cast<std::uint32_t>(prevLedger.seq()) + 1;
            auto const commitment = sha512Half(
                myEntropySecret_,
                static_cast<std::uint32_t>(peer.id),
                peer.key.second,
                seq);
            pos.myCommitment = commitment;
            pendingCommits_[peer.id] = commitment;
            nodeKeys_.insert_or_assign(peer.id, peer.key);
        }

        template <class Ledger_t>
        void
        decorateExportPosition(
            ProposalPosition& pos,
            Ledger_t const& prevLedger,
            bool proposing)
        {
            if (!enableExportConsensus_ || !proposing || !peer.runAsValidator)
                return;

            auto const seq = static_cast<std::uint32_t>(prevLedger.seq()) + 1;
            auto const sig = sha512Half(
                std::string("csf-export-sig"),
                static_cast<std::uint32_t>(peer.id),
                peer.key.second,
                seq);
            pos.myExportSignature = sig;
            pendingExportSigs_[peer.id] = sig;
            nodeKeys_.insert_or_assign(peer.id, peer.key);
        }

        void
        appendJson(Json::Value&) const
        {
        }

        template <class Pos>
        void
        logPosition(
            Pos const&,
            beast::Journal,
            beast::severities::Severity = beast::severities::kTrace) const
        {
        }

        // --- Stubs for features CSF doesn't model ---
        bool
        bootstrapFastStartEnabled() const
        {
            return false;
        }
        bool
        shouldSendExplicitFinalProposal() const
        {
            return false;
        }
        std::optional<TxSet>
        buildExplicitFinalProposalTxSet(TxSet const&, Ledger::Seq)
        {
            return std::nullopt;
        }
        bool
        hasPendingExportSigs() const
        {
            return enableExportConsensus_ && !pendingExportSigs_.empty();
        }
        void
        setExportSigConvergenceFailed()
        {
            if (enableExportConsensus_)
                exportSigConvergenceFailed_ = true;
        }

        // --- Sub-state accessors ---
        bool
        extensionsBusy() const
        {
            return estState_ != EstablishState::ConvergingTx ||
                (exportEnabled() &&
                 (exportSigGateStarted_ || hasPendingExportSigs()));
        }
        EstablishState
        estState() const
        {
            return estState_;
        }
        void
        resetSubState()
        {
            estState_ = EstablishState::ConvergingTx;
            revealPhaseStart_ = {};
            commitHashConflictStart_ = {};
            explicitFinalProposalSent_ = false;
            entropySetPublished_ = false;
            entropyPublishStart_ = {};
            exportSigGateStarted_ = false;
            exportSigGateStart_ = {};
            exportSigConvergenceFailed_ = false;
        }

        /// Defined in test/csf/PeerTick.h (keeps xrpld/app dependency
        /// out of this header).
        template <class Ctx>
        ExtensionTickResult
        onTick(Ctx const& ctx);

    private:
        uint256
        hashRngSet(
            hash_map<PeerID, uint256> const& entries,
            Ledger::Seq seq,
            std::string const& domain) const
        {
            std::vector<std::pair<std::uint32_t, uint256>> ordered;
            ordered.reserve(entries.size());
            for (auto const& [nodeId, digest] : entries)
            {
                if (!isUNLReportMember(nodeId))
                    continue;
                ordered.emplace_back(
                    static_cast<std::uint32_t>(nodeId), digest);
            }
            if (ordered.empty())
                return uint256{};
            std::sort(
                ordered.begin(),
                ordered.end(),
                [](auto const& a, auto const& b) { return a.first < b.first; });
            uint256 out = sha512Half(
                std::string("csf-rng-set"),
                domain,
                static_cast<std::uint32_t>(seq));
            for (auto const& [nodeId, digest] : ordered)
                out = sha512Half(out, nodeId, digest);
            return out;
        }
    };

    Extensions extensions_{*this};

    Extensions&
    ce()
    {
        return extensions_;
    }
    Extensions const&
    ce() const
    {
        return extensions_;
    }

    //! The collectors to report events to
    CollectorRefs& collectors;

    /** Constructor

        @param i Unique PeerID
        @param s Simulation Scheduler
        @param o Simulation Oracle
        @param n Simulation network
        @param tg Simulation trust graph
        @param c Simulation collectors
        @param jIn Simulation journal

    */
    Peer(
        PeerID i,
        Scheduler& s,
        LedgerOracle& o,
        BasicNetwork<Peer*>& n,
        TrustGraph<Peer*>& tg,
        CollectorRefs& c,
        beast::Journal jIn,
        SidecarStore& sc)
        : sink(jIn, "Peer " + to_string(i) + ": ")
        , j(sink)
        , consensus(s.clock(), *this, j)
        , id{i}
        , key{id, 0}
        , oracle{o}
        , sidecarStore{sc}
        , scheduler{s}
        , net{n}
        , trustGraph(tg)
        , lastClosedLedger{Ledger::MakeGenesis{}}
        , validations{ValidationParms{}, s.clock(), *this}
        , fullyValidatedLedger{Ledger::MakeGenesis{}}
        , collectors{c}
    {
        // All peers start from the default constructed genesis ledger
        ledgers[lastClosedLedger.id()] = lastClosedLedger;

        // nodes always trust themselves . . SHOULD THEY?
        trustGraph.trust(this, this);
    }

    /**  Schedule the provided callback in `when` duration, but if
        `when` is 0, call immediately
    */
    template <class T>
    void
    schedule(std::chrono::nanoseconds when, T&& what)
    {
        using namespace std::chrono_literals;

        if (when == 0ns)
            what();
        else
            scheduler.in(when, std::forward<T>(what));
    }

    // Issue a new event to the collectors
    template <class E>
    void
    issue(E const& event)
    {
        // Use the scheduler time and not the peer's (skewed) local time
        collectors.on(id, scheduler.now(), event);
    }

    //--------------------------------------------------------------------------
    // Trust and Network members
    // Methods for modifying and querying the network and trust graphs from
    // the perspective of this Peer

    //< Extend trust to a peer
    void
    trust(Peer& o)
    {
        trustGraph.trust(this, &o);
    }

    //< Revoke trust from a peer
    void
    untrust(Peer& o)
    {
        trustGraph.untrust(this, &o);
    }

    //< Check whether we trust a peer
    bool
    trusts(Peer& o)
    {
        return trustGraph.trusts(this, &o);
    }

    //< Check whether we trust a peer based on its ID
    bool
    trusts(PeerID const& oId)
    {
        for (auto const p : trustGraph.trustedPeers(this))
            if (p->id == oId)
                return true;
        return false;
    }

    /** Create network connection

        Creates a new outbound connection to another Peer if none exists

        @param o The peer with the inbound connection
        @param dur The fixed delay for messages between the two Peers
        @return Whether the connection was created.
    */

    bool
    connect(Peer& o, SimDuration dur)
    {
        return net.connect(this, &o, dur);
    }

    /** Remove a network connection

        Removes a connection between peers if one exists

        @param o The peer we disconnect from
        @return Whether the connection was removed
    */
    bool
    disconnect(Peer& o)
    {
        return net.disconnect(this, &o);
    }

    //--------------------------------------------------------------------------
    // Generic Consensus members

    // Attempt to acquire the Ledger associated with the given ID
    Ledger const*
    acquireLedger(Ledger::ID const& ledgerID)
    {
        if (auto it = ledgers.find(ledgerID); it != ledgers.end())
        {
            return &(it->second);
        }

        // No peers
        if (net.links(this).empty())
            return nullptr;

        // Don't retry if we already are acquiring it and haven't timed out
        auto aIt = acquiringLedgers.find(ledgerID);
        if (aIt != acquiringLedgers.end())
        {
            if (scheduler.now() < aIt->second)
                return nullptr;
        }

        using namespace std::chrono_literals;
        SimDuration minDuration{10s};
        for (auto const link : net.links(this))
        {
            minDuration = std::min(minDuration, link.data.delay);

            // Send a messsage to neighbors to find the ledger
            net.send(
                this, link.target, [to = link.target, from = this, ledgerID]() {
                    if (auto it = to->ledgers.find(ledgerID);
                        it != to->ledgers.end())
                    {
                        // if the ledger is found, send it back to the original
                        // requesting peer where it is added to the available
                        // ledgers
                        to->net.send(to, from, [from, ledger = it->second]() {
                            from->acquiringLedgers.erase(ledger.id());
                            from->ledgers.emplace(ledger.id(), ledger);
                        });
                    }
                });
        }
        acquiringLedgers[ledgerID] = scheduler.now() + 2 * minDuration;
        return nullptr;
    }

    // Attempt to acquire the TxSet associated with the given ID
    TxSet const*
    acquireTxSet(TxSet::ID const& setId)
    {
        if (auto it = txSets.find(setId); it != txSets.end())
        {
            return &(it->second);
        }

        // No peers
        if (net.links(this).empty())
            return nullptr;

        // Don't retry if we already are acquiring it and haven't timed out
        auto aIt = acquiringTxSets.find(setId);
        if (aIt != acquiringTxSets.end())
        {
            if (scheduler.now() < aIt->second)
                return nullptr;
        }

        using namespace std::chrono_literals;
        SimDuration minDuration{10s};
        for (auto const link : net.links(this))
        {
            minDuration = std::min(minDuration, link.data.delay);
            // Send a message to neighbors to find the tx set
            net.send(
                this, link.target, [to = link.target, from = this, setId]() {
                    if (auto it = to->txSets.find(setId);
                        it != to->txSets.end())
                    {
                        // If the txSet is found, send it back to the original
                        // requesting peer, where it is handled like a TxSet
                        // that was broadcast over the network
                        to->net.send(to, from, [from, txSet = it->second]() {
                            from->acquiringTxSets.erase(txSet.id());
                            from->handle(txSet);
                        });
                    }
                });
        }
        acquiringTxSets[setId] = scheduler.now() + 2 * minDuration;
        return nullptr;
    }

    bool
    hasOpenTransactions() const
    {
        return !openTxs.empty();
    }

    std::size_t
    proposersValidated(Ledger::ID const& prevLedger)
    {
        return validations.numTrustedForLedger(prevLedger);
    }

    std::size_t
    proposersFinished(Ledger const& prevLedger, Ledger::ID const& prevLedgerID)
    {
        return validations.getNodesAfter(prevLedger, prevLedgerID);
    }

    Result
    onClose(
        Ledger const& prevLedger,
        NetClock::time_point closeTime,
        ConsensusMode mode)
    {
        issue(CloseLedger{prevLedger, openTxs});

        Position_t pos{TxSet::calcID(openTxs)};

        ce().decoratePosition(
            pos, prevLedger, mode == ConsensusMode::proposing);

        return Result(
            TxSet{openTxs},
            Proposal(
                prevLedger.id(), Proposal::seqJoin, pos, closeTime, now(), id));
    }

    void
    onForceAccept(
        Result const& result,
        Ledger const& prevLedger,
        NetClock::duration const& closeResolution,
        ConsensusCloseTimes const& rawCloseTimes,
        ConsensusMode const& mode,
        Json::Value&& consensusJson)
    {
        onAccept(
            result,
            prevLedger,
            closeResolution,
            rawCloseTimes,
            mode,
            std::move(consensusJson),
            validating());
    }

    void
    onAccept(
        Result const& result,
        Ledger const& prevLedger,
        NetClock::duration const& closeResolution,
        ConsensusCloseTimes const& rawCloseTimes,
        ConsensusMode const& mode,
        Json::Value&& consensusJson,
        const bool validating)
    {
        schedule(delays.ledgerAccept, [=, this]() {
            const bool proposing = mode == ConsensusMode::proposing;
            const bool consensusFail = result.state == ConsensusState::MovedOn;
            auto const seq = static_cast<std::uint32_t>(prevLedger.seq()) + 1;

            ce().finalizeRoundEntropy(seq);
            ce().finalizeRoundExport();

            TxSet const acceptedTxs = injectTxs(prevLedger, result.txns);
            Ledger const newLedger = oracle.accept(
                prevLedger,
                acceptedTxs.txs(),
                closeResolution,
                result.position.closeTime());
            ledgers[newLedger.id()] = newLedger;

            issue(AcceptLedger{newLedger, lastClosedLedger});
            prevProposers = result.proposers;
            prevRoundTime = result.roundTime.read();
            lastClosedLedger = newLedger;

            auto const it = std::remove_if(
                openTxs.begin(), openTxs.end(), [&](Tx const& tx) {
                    return acceptedTxs.exists(tx.id());
                });
            openTxs.erase(it, openTxs.end());

            // Only send validation if the new ledger is compatible with our
            // fully validated ledger
            bool const isCompatible =
                newLedger.isAncestor(fullyValidatedLedger);

            // Can only send one validated ledger per seq
            if (runAsValidator && isCompatible && !consensusFail &&
                validations.canValidateSeq(newLedger.seq()))
            {
                bool isFull = proposing;

                Validation v{
                    newLedger.id(),
                    newLedger.seq(),
                    now(),
                    now(),
                    key,
                    id,
                    isFull};
                // share the new validation; it is trusted by the receiver
                share(v);
                // we trust ourselves
                addTrustedValidation(v);
            }

            checkFullyValidated(newLedger);

            // kick off the next round...
            // in the actual implementation, this passes back through
            // network ops
            ++completedLedgers;
            // startRound sets the LCL state, so we need to call it once after
            // the last requested round completes
            if (completedLedgers <= targetLedgers)
            {
                startRound();
            }
        });
    }

    // Earliest allowed sequence number when checking for ledgers with more
    // validations than our current ledger
    Ledger::Seq
    earliestAllowedSeq() const
    {
        return fullyValidatedLedger.seq();
    }

    Ledger::ID
    getPrevLedger(
        Ledger::ID const& ledgerID,
        Ledger const& ledger,
        ConsensusMode mode)
    {
        // only do if we are past the genesis ledger
        if (ledger.seq() == Ledger::Seq{0})
            return ledgerID;

        Ledger::ID const netLgr =
            validations.getPreferred(ledger, earliestAllowedSeq());

        if (netLgr != ledgerID)
        {
            JLOG(j.trace()) << Json::Compact(validations.getJsonTrie());
            issue(WrongPrevLedger{ledgerID, netLgr});
        }

        return netLgr;
    }

    void
    propose(Proposal const& pos)
    {
        share(pos);
    }

    ConsensusParms const&
    parms() const
    {
        return consensusParms;
    }

    // Not interested in tracking consensus mode changes for now
    void
    onModeChange(ConsensusMode, ConsensusMode)
    {
    }

    // Share a message by broadcasting to all connected peers
    template <class M>
    void
    share(M const& m)
    {
        issue(Share<M>{m});
        send(BroadcastMesg<M>{m, router.nextSeq++, this->id}, this->id);
    }

    // Unwrap the Position and share the raw proposal
    void
    share(Position const& p)
    {
        share(p.proposal());
    }

    //--------------------------------------------------------------------------
    // Validation members

    /** Add a trusted validation and return true if it is worth forwarding */
    bool
    addTrustedValidation(Validation v)
    {
        v.setTrusted();
        v.setSeen(now());
        ValStatus const res = validations.add(v.nodeID(), v);

        if (res == ValStatus::stale)
            return false;

        // Acquire will try to get from network if not already local
        if (Ledger const* lgr = acquireLedger(v.ledgerID()))
            checkFullyValidated(*lgr);
        return true;
    }

    /** Check if a new ledger can be deemed fully validated */
    void
    checkFullyValidated(Ledger const& ledger)
    {
        // Only consider ledgers newer than our last fully validated ledger
        if (ledger.seq() <= fullyValidatedLedger.seq())
            return;

        std::size_t const count = validations.numTrustedForLedger(ledger.id());
        std::size_t const numTrustedPeers = trustGraph.graph().outDegree(this);
        quorum = static_cast<std::size_t>(std::ceil(numTrustedPeers * 0.8));
        if (count >= quorum && ledger.isAncestor(fullyValidatedLedger))
        {
            issue(FullyValidateLedger{ledger, fullyValidatedLedger});
            fullyValidatedLedger = ledger;
        }
    }

    //-------------------------------------------------------------------------
    // Peer messaging members

    // Basic Sequence number router
    //   A message that will be flooded across the network is tagged with a
    //   sequence number by the origin node in a BroadcastMesg. Receivers will
    //   ignore a message as stale if they've already processed a newer sequence
    //   number, or will process and potentially relay the message along.
    //
    //  The various bool handle(MessageType) members do the actual processing
    //  and should return true if the message should continue to be sent to
    //  peers.
    //
    //  WARN: This assumes messages are received and processed in the order they
    //        are sent, so that a peer receives a message with seq 1 from node 0
    //        before seq 2 from node 0, etc.
    //  TODO: Break this out into a class and identify type interface to allow
    //        alternate routing strategies
    template <class M>
    struct BroadcastMesg
    {
        M mesg;
        std::size_t seq;
        PeerID origin;
    };

    struct Router
    {
        std::size_t nextSeq = 1;
        bc::flat_map<PeerID, std::size_t> lastObservedSeq;
    };

    Router router;

    // Send a broadcast message to all peers
    template <class M>
    void
    send(BroadcastMesg<M> const& bm, PeerID from)
    {
        for (auto const link : net.links(this))
        {
            if (link.target->id != from && link.target->id != bm.origin)
            {
                // cheat and don't bother sending if we know it has already been
                // used on the other end
                if (link.target->router.lastObservedSeq[bm.origin] < bm.seq)
                {
                    issue(Relay<M>{link.target->id, bm.mesg});
                    net.send(
                        this,
                        link.target,
                        [to = link.target, bm, id = this->id] {
                            to->receive(bm, id);
                        });
                }
            }
        }
    }

    // Receive a shared message, process it and consider continuing to relay it
    template <class M>
    void
    receive(BroadcastMesg<M> const& bm, PeerID from)
    {
        issue(Receive<M>{from, bm.mesg});
        if (router.lastObservedSeq[bm.origin] < bm.seq)
        {
            router.lastObservedSeq[bm.origin] = bm.seq;
            schedule(delays.onReceive(bm.mesg), [this, bm, from] {
                if (handle(bm.mesg))
                    send(bm, from);
            });
        }
    }

    // Type specific receive handlers, return true if the message should
    // continue to be broadcast to peers
    bool
    handle(Proposal const& p)
    {
        // Only relay untrusted proposals on the same ledger
        if (!trusts(p.nodeID()))
            return p.prevLedger() == lastClosedLedger.id();

        // TODO: This always suppresses relay of peer positions already seen
        // Should it allow forwarding if for a recent ledger ?
        auto& dest = peerPositions[p.prevLedger()];
        if (std::find(dest.begin(), dest.end(), p) != dest.end())
            return false;

        dest.push_back(p);

        // Rely on consensus to decide whether to relay
        return consensus.peerProposal(now(), Position{p});
    }

    bool
    handle(TxSet const& txs)
    {
        bool const inserted =
            txSets.insert(std::make_pair(txs.id(), txs)).second;
        if (inserted)
            consensus.gotTxSet(now(), txs);
        // relay only if new
        return inserted;
    }

    bool
    handle(Tx const& tx)
    {
        // Ignore and suppress relay of transactions already in last ledger
        TxSetType const& lastClosedTxs = lastClosedLedger.txs();
        if (lastClosedTxs.find(tx) != lastClosedTxs.end())
            return false;

        // only relay if it was new to our open ledger
        return openTxs.insert(tx).second;
    }

    bool
    handle(Validation const& v)
    {
        // TODO: This is not relaying untrusted validations
        if (!trusts(v.nodeID()))
            return false;

        // Will only relay if current
        return addTrustedValidation(v);
    }

    bool
    haveValidated() const
    {
        return fullyValidatedLedger.seq() > Ledger::Seq{0};
    }

    Ledger::Seq
    getValidLedgerIndex() const
    {
        return earliestAllowedSeq();
    }

    std::pair<std::size_t, hash_set<NodeKey_t>>
    getQuorumKeys()
    {
        hash_set<NodeKey_t> keys;
        for (auto const p : trustGraph.trustedPeers(this))
            keys.insert(p->key);
        return {quorum, keys};
    }

    std::size_t
    laggards(Ledger::Seq const seq, hash_set<NodeKey_t>& trusted)
    {
        return validations.laggards(seq, trusted);
    }

    bool
    validator() const
    {
        return runAsValidator;
    }

    void
    updateOperatingMode(std::size_t const positions) const
    {
    }

    bool
    validating() const
    {
        // does not matter
        return false;
    }

    //--------------------------------------------------------------------------
    //  A locally submitted transaction
    void
    submit(Tx const& tx)
    {
        issue(SubmitTx{tx});
        if (handle(tx))
            share(tx);
    }

    //--------------------------------------------------------------------------
    // Simulation "driver" members

    //! Heartbeat timer call
    void
    timerEntry()
    {
        consensus.timerEntry(now());
        // only reschedule if not completed
        if (completedLedgers < targetLedgers)
            scheduler.in(parms().ledgerGRANULARITY, [this]() { timerEntry(); });
    }

    // Called to begin the next round
    void
    startRound()
    {
        // Between rounds, we take the majority ledger
        // In the future, consider taking peer dominant ledger if no validations
        // yet
        Ledger::ID bestLCL =
            validations.getPreferred(lastClosedLedger, earliestAllowedSeq());
        if (bestLCL == Ledger::ID{0})
            bestLCL = lastClosedLedger.id();

        issue(StartRound{bestLCL, lastClosedLedger});

        // Not yet modeling dynamic UNL.
        hash_set<PeerID> nowUntrusted;
        consensus.startRound(
            now(), bestLCL, lastClosedLedger, nowUntrusted, runAsValidator, {});
    }

    // Start the consensus process assuming it is not yet running
    // This runs forever unless targetLedgers is specified
    void
    start()
    {
        // TODO: Expire validations less frequently?
        validations.expire(j);
        scheduler.in(parms().ledgerGRANULARITY, [&]() { timerEntry(); });
        startRound();
    }

    NetClock::time_point
    now() const
    {
        // We don't care about the actual epochs, but do want the
        // generated NetClock time to be well past its epoch to ensure
        // any subtractions of two NetClock::time_point in the consensus
        // code are positive. (e.g. proposeFRESHNESS)
        using namespace std::chrono;
        using namespace std::chrono_literals;
        return NetClock::time_point(duration_cast<NetClock::duration>(
            scheduler.now().time_since_epoch() + 86400s + clockSkew));
    }

    Ledger::ID
    prevLedgerID() const
    {
        return consensus.prevLedgerID();
    }

    //-------------------------------------------------------------------------
    // Injects a specific transaction when generating the ledger following
    // the provided sequence.  This allows simulating a byzantine failure in
    // which a node generates the wrong ledger, even when consensus worked
    // properly.
    // TODO: Make this more robust
    hash_map<Ledger::Seq, Tx> txInjections;

    /** Inject non-consensus Tx

        Injects a transactionsinto the ledger following prevLedger's sequence
        number.

        @param prevLedger The ledger we are building the new ledger on top of
        @param src The Consensus TxSet
        @return Consensus TxSet with inject transactions added if prevLedger.seq
                matches a previously registered Tx.
    */
    TxSet
    injectTxs(Ledger prevLedger, TxSet const& src)
    {
        auto const it = txInjections.find(prevLedger.seq());

        if (it == txInjections.end())
            return src;
        TxSetType res{src.txs()};
        res.insert(it->second);

        return TxSet{res};
    }
};

}  // namespace csf
}  // namespace test
}  // namespace ripple
#endif
