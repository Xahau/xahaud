#ifndef RIPPLE_APP_CONSENSUS_CONSENSUSEXTENSIONS_H_INCLUDED
#define RIPPLE_APP_CONSENSUS_CONSENSUSEXTENSIONS_H_INCLUDED

#include <xrpld/app/consensus/ActiveValidatorView.h>
#include <xrpld/app/consensus/RCLCxLedger.h>
#include <xrpld/app/consensus/RCLCxPeerPos.h>
#include <xrpld/app/consensus/RCLCxTx.h>
#include <xrpld/app/misc/ExportSigCollector.h>
#include <xrpld/consensus/ConsensusParms.h>
#include <xrpld/consensus/ConsensusTypes.h>
#include <xrpld/overlay/Message.h>
#include <xrpld/shamap/SHAMap.h>
#include <xrpl/basics/Buffer.h>
#include <xrpl/basics/Log.h>
#include <xrpl/beast/utility/Journal.h>
#include <xrpl/protocol/EntropyTier.h>
#include <xrpl/protocol/PublicKey.h>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ripple {

#ifndef XAHAUD_ENABLE_SIDECAR_RECONCILIATION
#define XAHAUD_ENABLE_SIDECAR_RECONCILIATION 1
#endif

class Application;
class CanonicalTXSet;
class Ledger;
class STTx;

/// Concrete alias for the consensus tick context.
using TickContext = ConsensusTick<ExtendedPosition, RCLCxPeerPos, RCLTxSet>;

/// Concrete Xahau-owned manager for consensus extensions (RNG + Export).
///
/// Owns all RNG/Export state that was previously scattered across
/// RCLCxAdaptor and Consensus.h. Lifecycle hooks are grouped by
/// caller/threading context.
class ConsensusExtensions
{
    Application& app_;
    ExportSigCollector exportSigCollector_;

public:
    beast::Journal j_;  // public: accessed by extensionsTick template

    // Type of sidecar set, known at fetch time from proposal context.
    enum class SidecarKind : uint8_t { commitSet, entropySet, exportSigSet };

    static constexpr bool
    sidecarReconciliationEnabled()
    {
        return XAHAUD_ENABLE_SIDECAR_RECONCILIATION != 0;
    }

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

    using ActiveValidatorView = ripple::ActiveValidatorView;
    using ActiveValidatorViewPtr = std::shared_ptr<ActiveValidatorView const>;
    using ExportSignatureSnapshot = std::map<PublicKey, Buffer>;

private:
    enum class RngContributionKind : uint8_t { commit, reveal };
    enum class RngProofCachePolicy : uint8_t { keepExisting, replaceExisting };

    // --- RNG Pipelined Storage ---
    hash_map<NodeID, uint256> pendingCommits_;
    hash_map<NodeID, uint256> pendingReveals_;
    hash_map<NodeID, PublicKey> nodeIdToKey_;

    // Ephemeral entropy secret (in-memory only, crash = non-revealer)
    uint256 myEntropySecret_;
    bool entropyFailed_ = false;
    bool commitSetFrozen_ = false;
    // Proposal ingress can harvest export signatures outside the consensus
    // mutex, so round-enable latches are atomic snapshots of the parent-ledger
    // amendment state. Ordering is not used to publish any other data.
    std::atomic<bool> rngEnabledThisRound_{false};
    std::atomic<bool> exportEnabledThisRound_{false};

    // Real SHAMaps for the current round (unbacked, ephemeral)
    std::shared_ptr<SHAMap> commitSetMap_;
    std::shared_ptr<SHAMap> entropySetMap_;
    std::shared_ptr<SHAMap> exportSigSetMap_;
    // Candidate entropy maps can be fetched/merged before they are safe to
    // inject. This hash is set only by the entropy sidecar gate after the
    // alignment/observation checks pass.
    std::optional<uint256> acceptedEntropySetHash_;
    // Export signature maps are also built from local collector state before
    // accept. Closed-ledger export apply may only consume the map root the
    // sidecar gate accepted for this round.
    std::optional<uint256> acceptedExportSigSetHash_;
    std::optional<LedgerIndex> rngRoundSeq_;
    // Consensus parent ledger hash, pinned at round start. Input to the
    // Tier 1 consensus_fallback entropy digest.
    uint256 roundPrevLedgerHash_;
    std::shared_ptr<SHAMap const> consensusTxSetMap_;
    hash_map<uint256, std::shared_ptr<STTx const>> consensusExportTxns_;
    std::optional<uint256> consensusTxSetHash_;

    struct PendingSidecarFetch
    {
        SidecarKind kind;
        char const* origin = "unknown";
    };

    // Track pending sidecar set fetches by hash. Kind/origin are known at fetch
    // time from call-site context, so onAcquiredSidecarSet can dispatch without
    // content-sniffing and logs can attribute eager vs gate-triggered fetches.
    hash_map<uint256, PendingSidecarFetch> pendingSidecarFetches_;

    // Parent-ledger validator view used by RNG and Export quorum logic.
    ActiveValidatorViewPtr activeValidatorView_ =
        std::make_shared<ActiveValidatorView const>();
    mutable std::mutex activeValidatorViewMutex_;

    // Recent proposers intersected with the active UNL (liveness hint)
    hash_set<NodeID> likelyParticipants_;
    std::optional<uint256> observedParticipantsHash_;
    std::size_t observedParticipantsCount_ = 0;
    std::string observedParticipantsBitmapBin_;

    // Current consensus mode (set by adaptor at round start)
    ConsensusMode mode_{ConsensusMode::observing};

public:
    // --- RNG Sub-state Machine (accessed by extensionsTick template) ---
    EstablishState estState_{EstablishState::ConvergingTx};
    std::chrono::steady_clock::time_point revealPhaseStart_{};
    std::chrono::steady_clock::time_point commitHashConflictStart_{};
    bool entropySetPublished_{false};
    std::chrono::steady_clock::time_point entropyPublishStart_{};
    bool exportSigGateStarted_{false};
    std::chrono::steady_clock::time_point exportSigGateStart_{};
    bool exportSigConvergenceFailed_{false};

private:
    void
    clearRngStatePreservingExport();

    bool
    hasProofedCommit(NodeID const& nodeId) const;

    bool
    hasActiveProofedCommit(
        NodeID const& nodeId,
        ActiveValidatorView const& validatorView) const;

    bool
    ingestRngContribution(
        NodeID const& nodeId,
        PublicKey const& publicKey,
        RngContributionKind kind,
        uint256 const& digest,
        std::optional<LedgerIndex> seq,
        std::optional<ProposalProof> const& proof,
        char const* sourceTag,
        RngProofCachePolicy proofCachePolicy);

    // Commit proofs keyed by NodeID. Only seq=0 proofs are cached because the
    // commit sidecar hash must be deterministic across all nodes.
    hash_map<NodeID, ProposalProof> commitProofs_;

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

    std::size_t
    exportSigQuorumThreshold() const;

    static std::size_t
    exportSigQuorumThreshold(ActiveValidatorView const& validatorView);

    /// Tier 2 (participant_aligned) alignment floor: the smallest cohort whose
    /// pairwise intersection exceeds the tolerated Byzantine count (~0.6 of the
    /// ORIGINAL pre-nUNL view; exact value from calculateParticipantThreshold).
    /// Anchored to the original size — unlike quorumThreshold(), which uses the
    /// effective (post-nUNL) size. See ActiveValidatorView.
    std::size_t
    tier2Threshold() const;

    /// Threshold at which the commit/reveal/entropy pipeline engages and the
    /// entropy conflict gate resolves: min(quorumThreshold(),
    /// tier2Threshold()). Governs proceed-vs-fall-back only; the tier LABEL
    /// comes from the agreed participant count in selectEntropy(), never from
    /// this local bar.
    std::size_t
    entropyGateThreshold() const;

    /// Pure threshold helper used by the live gate and optional drift tests.
    /// `effectiveViewSize` is post-nUNL; `originalViewSize` is the pre-nUNL
    /// UNLReport active count.
    static std::size_t
    entropyGateThresholdForView(
        std::size_t effectiveViewSize,
        std::size_t originalViewSize);

    /// Pure selector helper for the tier label only. The digest/count bytes are
    /// still derived from the agreed entropySetMap_ in selectEntropy(); this
    /// isolates the policy that says which EntropyTier a given agreed count
    /// earns under a ledger-anchored validator view.
    static EntropyTier
    selectEntropyTierForView(
        bool fromUNLReport,
        std::size_t participantCount,
        std::size_t effectiveViewSize,
        std::size_t originalViewSize);

    void
    setExpectedProposers(hash_set<NodeID> proposers);

    std::size_t
    pendingCommitCount() const;

    std::size_t
    proofedCommitCount() const;

    std::size_t
    pendingRevealCount() const;

    std::size_t
    proofedRevealCount() const;

    std::size_t
    expectedProposerCount() const;

    bool
    hasQuorumOfCommits() const;

    bool
    hasMinimumReveals() const;

    bool
    hasAnyReveals() const;

    void
    acceptEntropySet(uint256 const& hash);

    void
    clearAcceptedEntropySet();

    /// Result of the shared deterministic entropy selector: the digest to
    /// inject plus its tier/count labels. Both injection paths derive these
    /// identically from the AGREED entropySetMap_ so they cannot drift.
    struct EntropySelection
    {
        uint256 digest;
        std::uint8_t tier = 0;  // EntropyTier; the selector always sets this
        std::uint16_t count = 0;
    };

    /// Deterministically choose the entropy to inject for this round from the
    /// entropy sidecar accepted by the tick gate (never local pendingReveals_),
    /// labelled by agreed participant count: validator_quorum (>=
    /// quorumThreshold), participant_aligned (>= tier2Threshold) or
    /// consensus_fallback. In non-standalone mode, non-fallback labels require
    /// an UNLReport-backed active view; the trusted-fallback view is local
    /// config and mints Tier 1. agreedTxSetHash is the pre-injection consensus
    /// tx set hash used for the fallback digest.
    EntropySelection
    selectEntropy(uint256 const& agreedTxSetHash, LedgerIndex seq) const;

    /// Extend the legacy closed-ledger transaction-order salt with the same
    /// consensus entropy selected for the ledger's entropy pseudo-tx.
    uint256
    txnOrderingSalt(uint256 const& agreedTxSetHash, LedgerIndex seq) const;

    bool
    rngEnabled() const;

    bool
    exportEnabled() const;

    bool
    testSuppressExportSigSetHash() const;

    bool
    testBootstrapFastStartEnabled() const;

    uint256
    buildCommitSet(LedgerIndex seq);

    uint256
    buildEntropySet(LedgerIndex seq);

    uint256
    buildExportSigSet(LedgerIndex seq);

    bool
    hasPendingExportSigs() const;

    bool
    hasConsensusExportTxns() const;

    void
    setExportSigConvergenceFailed();

    bool
    exportSigConvergenceFailed() const;

    void
    acceptExportSigSet(uint256 const& hash);

    void
    clearAcceptedExportSigSet();

    std::optional<ExportSignatureSnapshot>
    agreedExportSignatures(
        STTx const& exportTx,
        uint256 const& txHash,
        std::size_t threshold) const;

    bool
    isSidecarSet(uint256 const& hash) const;

    ActiveValidatorViewPtr
    activeValidatorView() const;

    /// Build an active validator view from a known consensus parent ledger.
    /// A null parent falls back to the local validated ledger for startup,
    /// standalone, and diagnostics paths only; closed-ledger apply paths must
    /// fail closed before calling this if their parent ledger is unavailable.
    ActiveValidatorViewPtr
    makeActiveValidatorView(
        std::shared_ptr<Ledger const> const& prevLedger) const;

    bool
    isActiveValidator(PublicKey const& validationKey) const;

    bool
    isActiveValidator(
        PublicKey const& validationKey,
        ActiveValidatorView const& view) const;

    void
    onAcquiredSidecarSet(std::shared_ptr<SHAMap> const& map);

    void
    fetchSidecarSetIfNeeded(
        std::optional<uint256> const& hash,
        SidecarKind kind = SidecarKind::commitSet,
        char const* origin = "unknown");

    /// Fetch any sidecar sets from a peer's position if needed.
    void
    fetchSidecarsIfNeeded(
        ExtendedPosition const& peerPos,
        char const* origin = "eagerProposal");

    template <class PeerPositions>
    void
    recordParticipantDiagnostics(
        ConsensusMode mode,
        PeerPositions const& peerPositions)
    {
        std::vector<NodeID> peerNodeIds;
        peerNodeIds.reserve(peerPositions.size());
        for (auto const& entry : peerPositions)
            peerNodeIds.push_back(entry.first);
        recordParticipantDiagnostics(mode, std::move(peerNodeIds));
    }

    void
    recordParticipantDiagnostics(
        ConsensusMode mode,
        std::vector<NodeID> peerNodeIds);

    void
    attachParticipantDiagnostics(ExtendedPosition& pos) const;

    std::size_t
    observedParticipantCount() const;

    std::optional<uint256>
    observedParticipantsHash() const;

    std::string const&
    observedParticipantsBitmapBin() const;

    void
    cacheConsensusTxSet(RCLTxSet const& txns);

    std::size_t
    verifyPendingExportSigs(RCLTxSet const& txns, LedgerIndex seq);

    void
    cacheUNLReport(std::shared_ptr<Ledger const> const& prevLedger = {});

    bool
    isUNLReportMember(NodeID const& nodeId) const;

    // True only when THIS node's own validator key is in the active view. Used
    // to gate our own +1 in the sidecar alignment count to the same universe as
    // the peer-membership filter and the entropy/export thresholds.
    bool
    localIsActiveValidator() const;

    void
    generateEntropySecret();

    uint256
    getEntropySecret() const;

    void
    setEntropyFailed();

    /// Freeze commit admission before reveal material can affect the round.
    void
    freezeRngCommitSet();

    /// Self-seed our own reveal into pendingReveals_.
    /// Called from extensionsTick at reveal transition.
    /// In production, decorateMessage also self-seeds (belt + suspenders).
    void
    selfSeedReveal();

    void
    clearRngState();

    /// txSetHash is the agreed pre-injection consensus tx set hash — an input
    /// to the Tier 1 consensus_fallback digest. It must never be the hash of a
    /// set that could contain the entropy pseudo-tx itself.
    void
    onPreBuild(
        CanonicalTXSet& retriableTxs,
        LedgerIndex seq,
        uint256 const& txSetHash);

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
        Slice const& signature,
        std::vector<std::string> const& exportSignatures = {});

    /** Harvest proposal-carried export signatures after the proposal payload is
        known to be signed by `publicKey`. */
    std::size_t
    harvestExportSignatures(
        PublicKey const& publicKey,
        uint256 const& prevLedger,
        std::vector<std::string> const& exportSignatures,
        char const* source);

    /** Extract export signatures from the raw protobuf wire message.
        Called from PeerImp overlay ingress (outside consensus mutex).
        Reads only atomic round latches before touching the independently
        synchronized ExportSigCollector. */
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

    /** Attach export signatures before proposal signing.
        The caller hashes the resulting blobs into ExtendedPosition so the
        proposal signature authenticates the side-channel protobuf field. */
    void
    attachExportSignatures(
        protocol::TMProposeSet& prop,
        RCLCxPeerPos::Proposal const& proposal);

    /** Record post-signature RNG state for the outgoing protobuf.
        Self-seeds own reveal and stores proposal proofs. */
    void
    decorateMessage(
        protocol::TMProposeSet& prop,
        RCLCxPeerPos::Proposal const& proposal,
        ExtendedPosition const& signedPosition,
        Buffer const& proposalSig);

    ExtensionTickResult
    onTick(TickContext const& ctx);

    // --- Accessors for adaptor forwarding ---

    void
    setRngEnabledThisRound(bool v)
    {
        rngEnabledThisRound_.store(v, std::memory_order_relaxed);
    }

    void
    setExportEnabledThisRound(bool v)
    {
        exportEnabledThisRound_.store(v, std::memory_order_relaxed);
    }

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
        entropySetPublished_ = false;
        entropyPublishStart_ = {};
        exportSigGateStarted_ = false;
        exportSigGateStart_ = {};
        exportSigConvergenceFailed_ = false;
    }
};

}  // namespace ripple

#endif
