#ifndef RIPPLE_APP_CONSENSUS_CONSENSUSEXTENSIONS_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_CONSENSUSEXTENSIONS_H_INCLUDED

#include <xrpld/app/consensus/RCLCxLedger.h>
#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpld/app/consensus/RCLCxTx.h>
#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/consensus/ConsensusTypes.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>
#include <chrono>

namespace ripple {

class Application;
class CanonicalTXSet;
class Ledger;

/// Concrete alias for the consensus tick context.
using TickContext = ConsensusTick<ExtendedPosition, RCLCxPeerPos, RCLTxSet>;

/// Concrete Xahau-owned manager for consensus extensions (RNG + Export).
///
/// Owns all RNG/Export state that was previously scattered across
/// RCLCxAdaptor and Consensus.h. Lifecycle hooks are grouped by
/// caller/threading context.
///
/// See .ai-docs/refactoring-xahaud-consensus-extensions-v8.md.j2
/// for design rationale.
class ConsensusExtensions
{
    Application& app_;
    ExportSigCollector exportSigCollector_;

public:
    beast::Journal j_;  // public: accessed by extensionsTick template

    // Type of sidecar set, known at fetch time from proposal context.
    enum class SidecarKind : uint8_t { commit, reveal, exportSig };

private:
    // --- RNG Pipelined Storage ---
    hash_map<NodeID, uint256> pendingCommits_;
    hash_map<NodeID, uint256> pendingReveals_;
    hash_map<NodeID, PublicKey> nodeIdToKey_;

    // Ephemeral entropy secret (in-memory only, crash = non-revealer)
    uint256 myEntropySecret_;
    bool entropyFailed_ = false;
    bool rngEnabledThisRound_ = false;

    // Real SHAMaps for the current round (unbacked, ephemeral)
    std::shared_ptr<SHAMap> commitSetMap_;
    std::shared_ptr<SHAMap> entropySetMap_;
    std::shared_ptr<SHAMap> exportSigSetMap_;
    std::optional<LedgerIndex> rngRoundSeq_;

    // Track pending sidecar set fetches by hash → kind.
    // Kind is known at fetch time (call site context), so
    // onAcquiredSidecarSet can dispatch without content-sniffing.
    hash_map<uint256, SidecarKind> pendingRngFetches_;

    // Cached set of NodeIDs from UNL Report (or fallback UNL)
    hash_set<NodeID> unlReportNodeIds_;

    // Recent proposers intersected with the active UNL (liveness hint)
    hash_set<NodeID> likelyParticipants_;

    // Current consensus mode (set by adaptor at round start)
    ConsensusMode mode_{ConsensusMode::observing};

public:
    // --- RNG Sub-state Machine (accessed by extensionsTick template) ---
    EstablishState estState_{EstablishState::ConvergingTx};
    std::chrono::steady_clock::time_point revealPhaseStart_{};
    std::chrono::steady_clock::time_point commitHashConflictStart_{};
    bool explicitFinalProposalSent_{false};
    bool entropySetPublished_{false};
    std::chrono::steady_clock::time_point entropyPublishStart_{};
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

private:
    // Proposal proofs keyed by NodeID.
    // commitProofs_: only seq=0 proofs (deterministic across all nodes).
    // proposalProofs_: latest proof with reveal (for entropySet).
    hash_map<NodeID, ProposalProof> commitProofs_;
    hash_map<NodeID, ProposalProof> proposalProofs_;

public:
    ConsensusExtensions(Application& app, beast::Journal j);

    ExportSigCollector&
    exportSigCollector()
    {
        return exportSigCollector_;
    }

    ExportSigCollector const&
    exportSigCollector() const
    {
        return exportSigCollector_;
    }

    /// Set the current consensus mode (called by adaptor).
    void
    setMode(ConsensusMode m)
    {
        mode_ = m;
    }

    // --- RNG Helper Methods ---

    std::size_t
    quorumThreshold() const;

    void
    setExpectedProposers(hash_set<NodeID> proposers);

    std::size_t
    pendingCommitCount() const;

    std::size_t
    pendingRevealCount() const;

    std::size_t
    expectedProposerCount() const;

    bool
    hasQuorumOfCommits() const;

    bool
    hasMinimumReveals() const;

    bool
    hasAnyReveals() const;

    bool
    shouldZeroEntropy() const;

    bool
    rngEnabled() const;

    bool
    bootstrapFastStartEnabled() const;

    bool
    shouldSendExplicitFinalProposal() const;

    std::optional<RCLTxSet>
    buildExplicitFinalProposalTxSet(RCLTxSet const& txns, LedgerIndex seq);

    uint256
    buildCommitSet(LedgerIndex seq);

    uint256
    buildEntropySet(LedgerIndex seq);

    uint256
    buildExportSigSet(LedgerIndex seq);

    bool
    hasPendingExportSigs() const;

    bool
    isSidecarSet(uint256 const& hash) const;

    void
    onAcquiredSidecarSet(std::shared_ptr<SHAMap> const& map);

    void
    fetchRngSetIfNeeded(
        std::optional<uint256> const& hash,
        SidecarKind kind = SidecarKind::commit);

    /// Fetch any sidecar sets from a peer's position if needed.
    void
    fetchSidecarsIfNeeded(ExtendedPosition const& peerPos);

    void
    cacheUNLReport();

    bool
    isUNLReportMember(NodeID const& nodeId) const;

    void
    generateEntropySecret();

    uint256
    getEntropySecret() const;

    void
    setEntropyFailed();

    /// Self-seed our own reveal into pendingReveals_.
    /// Called from extensionsTick at reveal transition.
    /// In production, decorateMessage also self-seeds (belt + suspenders).
    void
    selfSeedReveal();

    void
    clearRngState();

    void
    onPreBuild(CanonicalTXSet& retriableTxs, LedgerIndex seq);

    void
    harvestRngData(
        NodeID const& nodeId,
        PublicKey const& publicKey,
        ExtendedPosition const& position,
        std::uint32_t proposeSeq,
        NetClock::time_point closeTime,
        uint256 const& prevLedger,
        Slice const& signature);

    static Blob
    serializeProof(ProposalProof const& proof);

    static std::optional<ProposalProof>
    deserializeProof(Blob const& proofBlob);

    static bool
    verifyProof(
        Blob const& proofBlob,
        PublicKey const& publicKey,
        uint256 const& expectedDigest,
        bool isCommit);

    /// Append extension diagnostics to consensus JSON.
    void
    appendJson(Json::Value& ret) const;

    /// Log extension-specific position fields at trace level.
    void
    logPosition(
        ExtendedPosition const& pos,
        beast::Journal j,
        beast::severities::Severity level = beast::severities::kTrace) const;

    // --- Consensus/adaptor lifecycle hooks ---

    /** Reset per-round extension state.
        Called from startRoundInternal under RCLConsensus::mutex_. */
    void
    onRoundStart(RCLCxLedger const& prevLedger, hash_set<NodeID> lastProposers);

    /** Extract extension data from the parsed proposal.
        Called from peerProposalInternal under RCLConsensus::mutex_. */
    void
    onTrustedPeerProposal(
        NodeID const& nodeId,
        PublicKey const& publicKey,
        ExtendedPosition const& position,
        std::uint32_t proposeSeq,
        NetClock::time_point closeTime,
        uint256 const& prevLedger,
        Slice const& signature);

    /** Signal that the accept/build path finished successfully.
        Called from doAccept (frozen state, no consensus mutex). */
    void
    onAcceptComplete();

    /** Extract export signatures from the raw protobuf wire message.
        Called from PeerImp overlay ingress (outside consensus mutex).
        Only touches the independently synchronized ExportSigCollector. */
    void
    onTrustedPeerMessage(::protocol::TMProposeSet const& wireMsg);

    /** Attach RNG commitment to the initial proposal position.
        Called from onClose BEFORE signing. Affects proposal identity.
        Generates entropy secret, caches UNL, seeds own commitment. */
    void
    decoratePosition(
        ExtendedPosition& pos,
        std::shared_ptr<Ledger const> const& prevLedger,
        bool proposing);

    /** Attach extension data to the outgoing protobuf AFTER signing.
        Self-seeds own reveal, stores proposal proofs, attaches export
        signatures. Does NOT affect proposal identity. */
    void
    decorateMessage(
        protocol::TMProposeSet& prop,
        RCLCxPeerPos::Proposal const& proposal,
        Buffer const& proposalSig);

    ExtensionTickResult
    onTick(TickContext const& ctx);

    // --- Accessors for adaptor forwarding ---

    void
    setRngEnabledThisRound(bool v)
    {
        rngEnabledThisRound_ = v;
    }

    bool
    extensionsBusy() const
    {
        return estState_ != EstablishState::ConvergingTx;
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
    }
};

}  // namespace ripple

#endif
